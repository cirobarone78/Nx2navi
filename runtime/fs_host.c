// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
// nn::fs host implementation of the functions main imports. RomFS paths and
// handles go to the read-only backend (romfs.c); save, temporary and cache storage are host directories under the
// profile directory (rt_config()->profile, default build/profile):
//
//   <profile>/save/<uid hi><uid lo>/   committed application save data, created on first use as the system launcher
//                                      does for titles that declare save data
//   <profile>/save-tx/                 the save mount's working copy and Commit's staging (section "save
//                                      transactions")
//   <profile>/save-backup/<time>/      the three committed saves before the current one
//   <profile>/save-files/              the player's other save files, each laid out as these three (section "save
//                                      files"); the profile's own is file 1
//   <profile>/save-import/             a save the menu imported, a new save file at the next start (section "a shared
//                                      save")
//   <profile>/temp/                    emptied at start
//   <profile>/cache/<index>/           cache storage
//
// The save mount is journaled as on Horizon: changes go to a working copy and become the committed save atomically at
// Commit; a crash or an Unmount before Commit leaves the previous committed save. Commit returns at once and the copy
// runs on a background thread (section "save transactions"); fs_save_flush lets the exit paths wait for it. Paths are
// UTF-8 (platform.h plat_fs_*).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_path.h"
#include "save_summary.h"

#define NX2NAVI_APPLICATION_ID 0x0400486024dec000ull   // the game's title id, as svc.c PROGRAM_ID tells it
#define FILE_BASE 0x200000000ull
#define DIR_BASE 0x300000000ull
#define NFILES 1024
#define NDIRS 256
#define NMOUNTS 16
#define PATH_CAP 1100

enum { M_FREE, M_SAVE, M_TEMP, M_CACHE, M_SAVE_RO };   // M_SAVE: the working copy; M_SAVE_RO: the committed save

typedef struct { int kind; char name[32]; char root[1024]; } Mount;
typedef struct { PlatFile *h; int mode, save; } File;   // h NULL = free; save: on a save-data mount
typedef struct { char *name; int kind; uint64_t size; } Listed;
typedef struct { Listed *e; int n, next, mode, used; } Dir;   // the directory's entries at open; used 0 = free

static Mount mounts[NMOUNTS];
static File files[NFILES];
static Dir dirs[NDIRS];
static PlatMutex lock = PLAT_MUTEX_INIT;
static char profile[1024], save_root[1100], work_root[1100];
static volatile int32_t storage_ready;

static void need(int c, const char *m) {
    if (!c) rt_fatal("fs: %s", m);
}

// ---- directory listings: every entry but "." and "..", taken whole before the caller changes the directory.
typedef struct { Listed *e; int n, cap; } Listing;

static int listing_add(const char *name, int kind, uint64_t size, void *ctx) {
    Listing *l = ctx;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->e = realloc(l->e, (size_t)l->cap * sizeof *l->e);
        need(l->e != NULL, "directory listing");
    }
    l->e[l->n].name = strdup(name);
    need(l->e[l->n].name != NULL, "directory listing");
    l->e[l->n].kind = kind, l->e[l->n].size = size;
    l->n++;
    return 1;
}

static int list_dir(const char *dir, Listing *l) {
    *l = (Listing){0};
    return plat_fs_list(dir, listing_add, l);
}

static void listing_free(Listed *e, int n) {
    for (int i = 0; i < n; i++) free(e[i].name);
    free(e);
}

// dir/name in a malloc'd string.
static char *join(const char *dir, const char *name) {
    size_t n = strlen(dir), k = strlen(name);
    char *p = malloc(n + k + 2);
    need(p != NULL, "path allocation");
    memcpy(p, dir, n);
    p[n] = '/';
    memcpy(p + n + 1, name, k + 1);
    return p;
}

// Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet
// (NX2navi writes a slot's files back to back and then commits; between the files nothing is open for a moment).
static volatile int32_t save_writers, save_dirty;
static volatile int64_t save_changed_ns;
static void save_wait_commit(void);
static void save_touched(void) {
    save_wait_commit();   // every change to save data comes here first: none lands in a commit's snapshot
    plat_xchg64(&save_changed_ns, (int64_t)plat_now_ns());
    plat_xchg32(&save_dirty, 1);
}
int fs_save_busy(void) {
    if (plat_cas32(&save_writers, 0, 0) > 0) return 1;
    return plat_cas32(&save_dirty, 0, 0) &&
           plat_now_ns() - (uint64_t)plat_cas64(&save_changed_ns, 0, 0) < 3000000000ull;
}

// ---- host directories
// A junction or symbolic link to a folder counts as the folder: a player who moved the profile to another drive links
// it back, and the start stopped at "cannot create directory" for it.
static int make_dir(const char *p) {
    int e = plat_fs_mkdir(p);
    if (!e) return 1;
    if (e != PLAT_FS_EXISTS) return 0;
    int k = plat_fs_kind(p);
    return k == PLAT_FS_DIR || (k == PLAT_FS_LINK && plat_dir_exists(p));
}

// p and every missing parent.
static void make_dirs(const char *p) {
    char buf[1100];
    need(snprintf(buf, sizeof buf, "%s", p) < (int)sizeof buf, "profile path");
    for (char *q = buf + 1; *q; q++)
        if ((*q == '/' || *q == '\\') && q[-1] != ':') {
            char c = *q;
            *q = 0;
            make_dir(buf);
            *q = c;
        }
    if (!make_dir(buf)) rt_fatal("fs: cannot create directory %s", buf);
}

// The first path remove_tree could not delete and the OS error code, for the message of a fatal.
static char remove_failed[1100];
static int remove_error;
static void remove_note(const char *path) {
    if (*remove_failed) return;
    snprintf(remove_failed, sizeof remove_failed, "%s", path);
    remove_error = plat_fs_os_error();
}

// Remove a directory tree; a link (symbolic link, junction) is removed as a link, never followed. 0 also when
// nothing is there.
static int remove_tree(const char *path) {
    int kind = plat_fs_kind(path);
    if (kind == PLAT_FS_MISSING) return 0;
    if (kind == PLAT_FS_FILE) {
        if (!plat_fs_unlink(path)) return 0;
        remove_note(path);
        return -1;
    }
    if (kind == PLAT_FS_LINK) {   // a link to a file goes by unlink, a link to a directory by rmdir
        if (!plat_fs_unlink(path) || !plat_fs_rmdir(path)) return 0;
        remove_note(path);
        return -1;
    }
    int bad = 0;
    if (kind == PLAT_FS_DIR) {
        // a read-only folder: Linux refuses to delete the entries of a folder without write permission
        plat_file_set_readonly(path, 0);
        Listing l;
        list_dir(path, &l);
        for (int i = 0; i < l.n; i++) {
            char *child = join(path, l.e[i].name);
            bad |= remove_tree(child);
            free(child);
        }
        listing_free(l.e, l.n);
    }
    if (plat_fs_rmdir(path)) {
        remove_note(path);
        bad |= -1;
    }
    return bad;
}

// remove_tree, tried again every 50 ms for up to 2 s while another process (the previous run still exiting, a virus
// scanner, a sync client) holds a file in the tree. The failing path and its OS error stay in remove_failed.
#define MOVE_TRIES 40   // 2 s, also move_dir's
static int remove_tree_retry(const char *path) {
    for (int i = 0;; i++) {
        *remove_failed = 0;
        if (!remove_tree(path)) return 0;
        if (i == MOVE_TRIES) return -1;
        plat_sleep_ms(50);
    }
}

// ---- save transactions
// The save mount's root is a working copy, made from the newest save when the first save mount appears. Commit copies
// it to a staging directory, which a rename makes a finished commit, and swaps that in for the committed save:
//
//   <profile>/save/<uid>/          committed save: what the next start loads when no finished commit waits
//   <profile>/save-tx/work/        working copy, the save mount's root; deleted at the last Unmount and at start
//   <profile>/save-tx/new/         Commit's staging copy of the working copy
//   <profile>/save-tx/ready-<n>/   a finished commit not swapped in yet: the newest save
//   <profile>/save-tx/old/         the committed save a Commit replaced, on its way to save-backup
//
// Commit: (1) copy work to new and flush every file (FlushFileBuffers) and directory, a file equal to the newest
// save's as a hard link to it; (2) rename new to ready-<n>, the commit point, and delete the ready-<n> it supersedes;
// (3) rename save/<uid> to old; (4) rename ready-<n> to save/<uid>; (5) move old to save-backup/<UTC time> and keep
// the newest three. Every rename is durable (plat_fs_rename). Windows refuses (3) while any file under save/<uid> is
// open, in this or another process (a virus scanner, Explorer's preview, a sync tool), and a folder an elevated run
// made can refuse it too. ready-<n> then stays the newest save: later working copies are made from it, and the next
// Commit and the next start try (3) to (5) again. The game's Commit returns 0 once the steps are queued; a thread runs
// them (95-135 ms on the game thread before, in the 1.4.3 fight save) while every change to the working copy waits in
// save_touched, and Unmount and the next Commit wait too. A crash in that time loses this Commit only, and a failed
// one is logged instead of returned. The recovery rule at start (save_recover):
//   new present          an unfinished copy: deleted (rollback). An older build renamed save/<uid> away before its
//                        finished copy: with save/<uid> and ready-<n> absent, new is that copy and becomes save/<uid>;
//   ready-<n> present    the highest n is the newest save and (3) to (5) run (roll forward); the others are deleted;
//   save/<uid> absent    with no ready-<n> either, old goes back to save/<uid>.
// Then old, if present, goes to save-backup, and work (changes no Commit took) is deleted.
#define SAVE_BACKUPS 3
#define COPY_CHUNK (1u << 20)

static char tx_root[1100], backup_root[1100], new_root[1100], old_root[1100];
static char ready_root[1100];   // the newest finished commit not swapped in, empty when none
static unsigned ready_seq;      // the highest ready-<n> this run made or found
static PlatMutex tx_lock = PLAT_MUTEX_INIT;   // taken before lock, never inside it
static int save_mounts;                  // M_SAVE mounts: the working copy exists while this is above 0

// Probe hook: Commit stops at its tx_crash_at-th step (from 1) and leaves the disk as a process killed there would.
static int tx_crash_at, tx_step, tx_dead;
static int crashed(void) {
    if (tx_crash_at && ++tx_step == tx_crash_at) tx_dead = 1;
    return tx_dead;
}

// Probe hook: a step that fails without a crash, as a full disk or a file another program holds open makes one fail.
// fail_at: the fail_at-th step (from 1) fails; fail_space_from: from that step on every step that needs room on the
// disk (a folder, a file) fails, as on a full disk, while renames still work; fail_move: every move of a path holding
// that text fails, as for a file a virus scanner holds open; busy_move: every try of such a move is refused as busy,
// so move_dir gives up only after its whole retry time, as while a sync client keeps a file in it open. fail_step
// counts the steps. All 0 outside the probe.
static int fail_at, fail_space_from, fail_step;
static const char *fail_move, *busy_move;
static int failing(int space) {
    if (!fail_at && !fail_space_from) return 0;
    ++fail_step;
    return fail_step == fail_at || (space && fail_space_from && fail_step >= fail_space_from);
}

// plat_fs_mkdir with the probe's failures.
static int files_mkdir(const char *p) { return failing(1) ? -1 : plat_fs_mkdir(p); }

// A save file operation starts counting the probe's steps. Only while the probe arms a hook: the commit thread reads
// tx_dead, and outside the probe nothing writes it.
static void steps_reset(void) {
    if (tx_crash_at || fail_at || fail_space_from) tx_step = tx_dead = fail_step = 0;
}

static int exists(const char *p) { return plat_fs_kind(p) != PLAT_FS_MISSING; }

// Rename a directory, durably; retried every 50 ms, tries times in all, while another process (a virus scanner, the
// indexer) holds a file in it. 0 on success.
static int move_dir(const char *from, const char *to, int tries) {
    if (failing(0) || (fail_move && strstr(from, fail_move))) {
        rt_log("fs: cannot move %s to %s (the probe's refusal)", from, to);
        return -1;
    }
    for (int i = 0; i < tries; i++) {
        int e = busy_move && strstr(from, busy_move) ? PLAT_FS_BUSY : plat_fs_rename(from, to, 1);
        if (!e) return 0;
        if (e != PLAT_FS_BUSY) {
            // a folder rename cannot cross drives: save/ linked to another drive than save-tx/ (link the whole
            // profile folder instead) fails every commit here
            rt_log("fs: cannot move %s to %s (error %d)%s", from, to, plat_fs_os_error(),
                   plat_fs_os_error() == 17 ? ": they are on different drives; keep save and save-tx on one drive" : "");
            return -1;
        }
        plat_sleep_ms(50);
    }
    return -1;
}

// Copy one file; the source may be open for writing (a save file the game has not closed). 0 on success.
static int copy_file(const char *src, const char *dst, int durable, uint64_t *bytes) {
    PlatFile *in = plat_fs_open(src, 0, NULL);
    if (!in) return -1;
    PlatFile *out = plat_fs_create(dst, NULL);
    if (!out) {
        plat_file_close(in);
        return -1;
    }
    uint8_t *buf = malloc(COPY_CHUNK);
    need(buf != NULL, "copy buffer");
    int ok = !failing(1);   // the probe's full disk: the file is made, its bytes are not
    for (uint64_t at = 0;;) {
        int64_t got = plat_file_pread(in, buf, COPY_CHUNK, at);
        if (got < 0) ok = 0;
        if (!ok || !got) break;
        if (plat_file_pwrite(out, buf, (uint32_t)got, at) != got) ok = 0;
        if (!ok) break;
        at += (uint64_t)got;
        *bytes += (uint64_t)got;
    }
    if (ok && durable) ok = plat_file_fsync(out);
    free(buf);
    plat_file_close(in);
    plat_file_close(out);
    return ok ? 0 : -1;
}

// Whether files a and b hold the same bytes; 0 when either cannot be read.
static int same_file(const char *a, const char *b) {
    PlatFile *h[2] = {plat_fs_open(a, 0, NULL), plat_fs_open(b, 0, NULL)};
    PlatFileId ia, ib;
    int same = h[0] && h[1] && plat_file_id(h[0], &ia) && plat_file_id(h[1], &ib) && ia.size == ib.size;
    uint8_t *buf = same ? malloc(2 * (size_t)COPY_CHUNK) : NULL;
    if (same) need(buf != NULL, "compare buffer");
    for (uint64_t at = 0; same;) {
        int64_t ga = plat_file_pread(h[0], buf, COPY_CHUNK, at), gb = plat_file_pread(h[1], buf + COPY_CHUNK, COPY_CHUNK, at);
        if (ga < 0 || ga != gb || memcmp(buf, buf + COPY_CHUNK, (size_t)ga)) same = 0;
        else if (!ga) break;
        at += (uint64_t)ga;
    }
    free(buf);
    for (int i = 0; i < 2; i++)
        if (h[i]) plat_file_close(h[i]);
    return same;
}

// Copy the tree src to dst, which must not exist; links are skipped. durable: every file is flushed, and every
// directory as far as the file system allows. base (or NULL): the committed save; a file whose bytes equal base's file
// at the same path becomes a hard link to it instead of a copy (*nlinked), as committed files are never written in
// place (the game writes the working copy). A save commit then writes the 3-4 files of the slot the game saved instead
// of all 82 (300-380 ms of flushes on the game thread). 0 on success.
static int copy_tree(const char *src, const char *dst, const char *base, int durable, int *nfiles, int *nlinked,
                     uint64_t *bytes) {
    if (plat_fs_mkdir(dst)) return -1;
    Listing l;
    if (list_dir(src, &l)) return -1;
    int bad = 0;
    for (int i = 0; i < l.n && !bad; i++) {
        if (l.e[i].kind == PLAT_FS_LINK) continue;
        char *s = join(src, l.e[i].name), *d = join(dst, l.e[i].name);
        char *b = base ? join(base, l.e[i].name) : NULL;
        if (b && !exists(b)) free(b), b = NULL;
        if (l.e[i].kind == PLAT_FS_DIR) bad = copy_tree(s, d, b, durable, nfiles, nlinked, bytes);
        else {
            if (b && same_file(s, b) && !plat_fs_link(b, d)) bad = 0, ++*nlinked;
            else bad = copy_file(s, d, durable, bytes);
            *nfiles += !bad;
            if (!bad && durable && crashed()) bad = -1;
        }
        free(s);
        free(d);
        free(b);
    }
    listing_free(l.e, l.n);
    if (!bad && durable) plat_fs_sync_dir(dst);
    return bad;
}

static int listed_cmp(const void *a, const void *b) {
    return strcmp(((const Listed *)a)->name, ((const Listed *)b)->name);
}

// Whether paths a and b are one file (hard links of it).
static int same_inode(const char *a, const char *b) {
    PlatFile *h[2] = {plat_fs_open(a, 0, NULL), plat_fs_open(b, 0, NULL)};
    PlatFileId ia, ib;
    int same = h[0] && h[1] && plat_file_id(h[0], &ia) && plat_file_id(h[1], &ib) && ia.volume == ib.volume &&
               ia.file_id == ib.file_id;
    for (int i = 0; i < 2; i++)
        if (h[i]) plat_file_close(h[i]);
    return same;
}

// Whether tree a holds the names and kinds of tree b and each file of it as a hard link: what copy_tree makes from a
// working copy whose every file equals base's.
static int same_tree(const char *a, const char *b) {
    Listing la, lb;
    int ea = list_dir(a, &la), eb = list_dir(b, &lb);
    int same = !ea && !eb && la.n == lb.n;
    if (same) {
        qsort(la.e, (size_t)la.n, sizeof *la.e, listed_cmp);
        qsort(lb.e, (size_t)lb.n, sizeof *lb.e, listed_cmp);
    }
    for (int i = 0; same && i < la.n; i++) {
        same = !strcmp(la.e[i].name, lb.e[i].name) && la.e[i].kind == lb.e[i].kind && la.e[i].kind != PLAT_FS_LINK;
        if (!same) break;
        char *pa = join(a, la.e[i].name), *pb = join(b, lb.e[i].name);
        same = la.e[i].kind == PLAT_FS_DIR ? same_tree(pa, pb) : same_inode(pa, pb);
        free(pa);
        free(pb);
    }
    listing_free(la.e, la.n);
    listing_free(lb.e, lb.n);
    return same;
}

static int name_cmp(const void *a, const void *b) { return strcmp(a, b); }

// The backups' folder names (digits first), sorted: oldest first, as rotate_backup names them in creation order.
static int backup_names(char (*names)[48]) {
    int n = 0;
    Listing l;
    list_dir(backup_root, &l);
    for (int i = 0; i < l.n; i++)
        if (l.e[i].kind == PLAT_FS_DIR && l.e[i].name[0] >= '0' && l.e[i].name[0] <= '9' && strlen(l.e[i].name) < 48 &&
            n < 64)
            strcpy(names[n++], l.e[i].name);
    listing_free(l.e, l.n);
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    return n;
}

// Move old to save-backup/<UTC time> and delete all backups but the newest SAVE_BACKUPS. The name always sorts after
// every existing one: a commit in the same millisecond as the newest backup (or after the clock went back) takes its
// time and the next -NNN suffix. A name that sorted lower would be the first one pruned, losing the backup just made.
static void rotate_backup(void) {
    if (!exists(old_root)) return;
    static char names[64][48];
    int n = backup_names(names);
    uint64_t ns = plat_unix_time_ns();
    struct tm t;
    plat_gmtime((int64_t)(ns / 1000000000u), &t);
    char name[48];
    int len = snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d-%03u", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                       t.tm_hour, t.tm_min, t.tm_sec, (unsigned)(ns / 1000000u % 1000u));
    const char *newest = n ? names[n - 1] : "";
    if (strcmp(name, newest) <= 0 && strlen(newest) >= (size_t)len) {
        unsigned k = 0;
        sscanf(newest + len, "-%u", &k);   // no suffix: k stays 0
        do   // until it sorts after newest, even after an unpadded "-1" of older builds
            snprintf(name, sizeof name, "%.*s-%03u", len, newest, ++k);
        while (strcmp(name, newest) <= 0 && k < 100000);
    }
    char *dst = join(backup_root, name);
    if (move_dir(old_root, dst, MOVE_TRIES)) {
        rt_log("fs: cannot move the replaced save to save-backup; deleted");
        remove_tree(old_root);
    }
    free(dst);
    n = backup_names(names);
    for (int i = 0; i + SAVE_BACKUPS < n; i++) {
        char *p = join(backup_root, names[i]);
        remove_tree(p);
        free(p);
    }
}

// The newest save: the finished commit waiting to be swapped in, else the committed save.
static const char *newest_save(void) { return *ready_root ? ready_root : save_root; }

// Steps (3) to (5): swap the finished commit ready_root in for the committed save. 0 on success; on failure
// ready_root stays the newest save. (3) is tried for 200 ms only: the commit is safe in ready_root, the next Commit
// or start tries again, and the game's next change to its save data waits for this one.
#define SWAP_TRIES 4
static int swap_in(void) {
    if (!*ready_root) return 0;
    rotate_backup();   // an old left by a failed (5): (3) needs its name
    if (exists(save_root) && move_dir(save_root, old_root, SWAP_TRIES)) {
        char why[512];
        plat_fs_busy_reason(save_root, why, sizeof why);
        rt_log("fs: save commit: the committed save cannot be replaced (%s); the commit waits in %s and the next "
               "Commit or start retries", *why ? why : "a file in it is in use", ready_root);
        return -1;
    }
    if (crashed()) return -1;
    if (move_dir(ready_root, save_root, MOVE_TRIES)) {
        rt_log("fs: save commit: cannot move the finished commit to the committed save; it waits in %s", ready_root);
        move_dir(old_root, save_root, MOVE_TRIES);   // else save/<uid> stays absent and the next start rolls forward
        return -1;
    }
    *ready_root = 0;
    if (crashed()) return -1;
    rotate_backup();
    return 0;
}

// The number n of a "ready-<n>" name, 0 for any other name.
static unsigned ready_number(const char *name) {
    char *end;
    if (strncmp(name, "ready-", 6) || name[6] < '0' || name[6] > '9') return 0;
    unsigned long n = strtoul(name + 6, &end, 10);
    return *end || n > 0xffffffffu ? 0 : (unsigned)n;
}

// Startup: the newest finished commit into ready_root; the older ones, which it supersedes, are deleted.
static void ready_scan(void) {
    Listing l;
    list_dir(tx_root, &l);
    *ready_root = 0, ready_seq = 0;
    for (int i = 0; i < l.n; i++)
        if (l.e[i].kind == PLAT_FS_DIR && ready_number(l.e[i].name) > ready_seq) ready_seq = ready_number(l.e[i].name);
    for (int i = 0; i < l.n; i++) {
        unsigned n = l.e[i].kind == PLAT_FS_DIR ? ready_number(l.e[i].name) : 0;
        char *p = n ? join(tx_root, l.e[i].name) : NULL;
        if (n == ready_seq && p) snprintf(ready_root, sizeof ready_root, "%s", p);
        else if (p) remove_tree(p);
        free(p);
    }
    listing_free(l.e, l.n);
}

// Startup: finish or roll back an interrupted Commit and drop uncommitted changes (the rule above).
static void save_recover(void) {
    const char *did = "nothing to recover";
    ready_scan();
    int has_save = exists(save_root);
    if (exists(new_root) && !has_save && !*ready_root) {
        if (move_dir(new_root, save_root, MOVE_TRIES)) rt_fatal("fs: cannot complete an interrupted save commit");
        did = "completed an interrupted commit";
        has_save = 1;
    } else if (exists(new_root)) {
        if (remove_tree(new_root)) rt_fatal("fs: cannot delete an unfinished save commit");
        did = "rolled back an unfinished commit";
    }
    if (!has_save && !*ready_root && exists(old_root)) {
        if (move_dir(old_root, save_root, MOVE_TRIES)) rt_fatal("fs: cannot roll back an interrupted save commit");
        did = "rolled back an interrupted commit";
    } else if (exists(old_root))
        did = "moved the save an interrupted commit replaced to save-backup";
    rotate_backup();
    if (*ready_root)
        did = swap_in() ? "a finished commit still waits in save-tx; the game loads it from there"
                        : "completed a finished commit";
    if (exists(work_root)) {
        if (remove_tree_retry(work_root))
            rt_fatal("fs: cannot delete the old save working copy: %s (OS error %d)", remove_failed, remove_error);
        if (!strcmp(did, "nothing to recover"))
            did = "deleted the last run's working copy (changes after its last Commit)";
    }
    rt_log("host save recovery: %s", did);
}

// The first save mount: a fresh working copy of the newest save.
static void work_create(void) {
    int n = 0, linked = 0;
    uint64_t bytes = 0;
    // remove_tree_retry: the last unmount's delete may have been held up by a scanner, as at the start
    if (remove_tree_retry(work_root) || copy_tree(newest_save(), work_root, NULL, 0, &n, &linked, &bytes))
        rt_fatal("fs: cannot make the save working copy");
    rt_log("host save working copy: %d files, %.1f MiB", n, bytes / 1048576.0);
}

// The last save unmount: changes no Commit took are dropped.
static void work_discard(void) {
    if (plat_xchg32(&save_dirty, 0)) rt_log("fs: save unmounted without Commit; changes discarded");
    if (remove_tree(work_root)) rt_log("fs: save working copy not fully deleted (a file still open?)");
}

// Commit of the save mount, tx_lock held. Steps (1) to (5) above; a failure before (2) leaves the newest save as it
// was, one after it leaves the commit waiting in ready_root.
static uint32_t save_commit_locked(void) {
    uint64_t t0 = plat_now_ns(), bytes = 0;
    int nfiles = 0, linked = 0;
    steps_reset();
    const char *base = exists(newest_save()) ? newest_save() : NULL;
    if (remove_tree(new_root) || copy_tree(work_root, new_root, base, 1, &nfiles, &linked, &bytes)) {
        if (tx_dead) return FS_R_INVALID_ARGUMENT;
        rt_log("fs: save commit: copying the working copy failed; the newest save is unchanged");
        remove_tree(new_root);
        return FS_R_INVALID_ARGUMENT;
    }
    if (crashed()) return FS_R_INVALID_ARGUMENT;
    if (base && linked == nfiles && same_tree(new_root, base)) {   // the game rewrote equal bytes: no save, no backup
        remove_tree(new_root);
        plat_xchg32(&save_dirty, 0);
        int waits = swap_in();
        if (tx_dead) return FS_R_INVALID_ARGUMENT;
        rt_log("fs commit save: %d files, none changed, in %.0f ms%s", nfiles, (plat_now_ns() - t0) / 1e6,
               waits ? "; the newest save waits in save-tx" : "");
        return 0;
    }
    char ready[1100];
    snprintf(ready, sizeof ready, "%s/ready-%010u", tx_root, ready_seq + 1);
    if (move_dir(new_root, ready, MOVE_TRIES)) {
        rt_log("fs: save commit: cannot finish the staging copy; the newest save is unchanged");
        remove_tree(new_root);
        return FS_R_INVALID_ARGUMENT;
    }
    ready_seq++;
    if (*ready_root) remove_tree(ready_root);   // superseded: the working copy it was made from held all of it
    snprintf(ready_root, sizeof ready_root, "%s", ready);
    plat_xchg32(&save_dirty, 0);
    if (crashed()) return FS_R_INVALID_ARGUMENT;
    int waits = swap_in();
    if (tx_dead) return FS_R_INVALID_ARGUMENT;
    rt_log("fs commit save: %d files, %d changed (%.1f MiB written), %d unchanged linked, in %.0f ms%s", nfiles,
           nfiles - linked, bytes / 1048576.0, linked, (plat_now_ns() - t0) / 1e6, waits ? "; waits in save-tx" : "");
    return 0;
}

// The background commit: commit_pending from the game's Commit until the thread has run save_commit_locked.
static PlatMutex pend_lock = PLAT_MUTEX_INIT;
static PlatCond pend_cv = PLAT_COND_INIT;
static volatile int32_t commit_pending;
static int exiting;   // under pend_lock: fs_save_flush ran, the process is ending

static void save_wait_commit(void) {
    if (!plat_cas32(&commit_pending, 0, 0)) return;
    plat_mutex_lock(&pend_lock);
    while (commit_pending) plat_cond_wait(&pend_cv, &pend_lock);
    plat_mutex_unlock(&pend_lock);
}

// The window closed: a Commit already started finishes first, and none starts afterwards, as the process exit would
// cut it off halfway (the startup recovery then keeps the previous one; here it is kept outright).
void fs_save_flush(void) {
    plat_mutex_lock(&pend_lock);
    exiting = 1;
    while (commit_pending) plat_cond_wait(&pend_cv, &pend_lock);
    plat_mutex_unlock(&pend_lock);
}

static uint32_t commit_run(void) {
    plat_mutex_lock(&tx_lock);
    uint32_t r = save_commit_locked();
    plat_dec32(&save_writers);
    plat_mutex_unlock(&tx_lock);
    plat_mutex_lock(&pend_lock);
    commit_pending = 0;
    plat_mutex_unlock(&pend_lock);
    plat_cond_broadcast(&pend_cv);
    return r;
}

static void commit_main(void *arg) {
    (void)arg;
    commit_run();
    return;
}

static uint32_t save_commit(void) {
    save_wait_commit();   // the previous Commit
    static int quiet;
    if (!plat_cas32(&save_dirty, 0, 0)) {
        if (!quiet++) rt_log("fs commit save: nothing changed (logged once)");
        return 0;
    }
    plat_mutex_lock(&pend_lock);
    if (exiting) {
        plat_mutex_unlock(&pend_lock);
        rt_log("fs commit save: the game is closing; the last finished commit stays");
        return 0;
    }
    plat_inc32(&save_writers);   // fs_save_busy until the thread is done
    plat_xchg32(&commit_pending, 1);
    plat_mutex_unlock(&pend_lock);
    if (plat_thread_spawn(commit_main, NULL, "save commit", 0)) {
        return 0;
    }
    rt_log("fs: cannot start the save commit thread; committing on the game thread");
    return commit_run();
}

// ---- a shared save (save_share.c): the export reads the newest save, the menu's import waits for the next start
//
//   <profile>/save-import/        a save the menu took from a zip: a new save file at the next start
//   <profile>/save-import.part/   that save while the menu writes it; renamed to save-import when complete
//
// The import cannot become a save while the game runs: the game holds its progress in memory and writes it back at its
// next save, and the save mount's working copy is made once per mount. So the next start takes it (import_pending,
// section "save files") before it picks the file to mount: the import becomes a save file of its own, which that start
// mounts, and no save is replaced.
static void ready(void);

// Visits every file under dir (rel: its path below the save, "" at the top), in name order; links are left out.
// -1 when a file cannot be read.
static int export_tree(const char *dir, const char *rel, FsSaveVisit visit, void *ctx) {
    Listing l;
    if (list_dir(dir, &l)) return -1;
    qsort(l.e, (size_t)l.n, sizeof *l.e, listed_cmp);
    int n = 0;
    for (int i = 0; i < l.n && n >= 0; i++) {
        if (l.e[i].kind == PLAT_FS_LINK) continue;
        char *path = join(dir, l.e[i].name);
        char *sub = *rel ? join(rel, l.e[i].name) : strdup(l.e[i].name);
        need(sub != NULL, "save export path");
        if (l.e[i].kind == PLAT_FS_DIR) {
            int k = export_tree(path, sub, visit, ctx);
            n = k < 0 ? -1 : n + k;
        } else {
            uint64_t size = 0;
            uint8_t *data = plat_file_read(path, &size);   // NULL for an empty file as well
            if (!data && plat_file_size(path) != 0) n = -1;
            else if (!visit(ctx, sub, data ? data : (const uint8_t *)"", (size_t)size)) n = -1;
            else n++;
            free(data);
        }
        free(path);
        free(sub);
    }
    listing_free(l.e, l.n);
    return n;
}

int fs_save_export(FsSaveVisit visit, void *ctx) {
    ready();
    // the commit thread holds tx_lock while it swaps a commit in, and the committed save is never written in place
    plat_mutex_lock(&tx_lock);
    const char *src = newest_save();
    int n = exists(src) ? export_tree(src, "", visit, ctx) : 0;
    plat_mutex_unlock(&tx_lock);
    return n;
}

// Writes one file and flushes it; 0 on success.
static int stage_file(const char *path, const uint8_t *data, size_t n) {
    char dir[PATH_CAP];
    snprintf(dir, sizeof dir, "%s", path);
    char *cut = strrchr(dir, '/');
    if (cut) {
        *cut = 0;
        plat_file_mkdir_p(dir);
    }
    PlatFile *f = plat_fs_create(path, NULL);
    if (!f) return -1;
    int ok = 1;
    for (size_t at = 0; ok && at < n;) {
        uint32_t k = n - at > COPY_CHUNK ? COPY_CHUNK : (uint32_t)(n - at);
        ok = plat_file_pwrite(f, data + at, k, at) == k;
        at += k;
    }
    ok = ok && plat_file_fsync(f);
    plat_file_close(f);
    return ok ? 0 : -1;
}

int fs_save_import_stage(int count, const char *const *rel, const uint8_t *const *data, const size_t *size) {
    ready();   // the start's own import_pending has run: this one waits for the next start
    char imp[1100], part[1100];
    snprintf(imp, sizeof imp, "%s/save-import", profile);
    snprintf(part, sizeof part, "%s/save-import.part", profile);
    if (remove_tree_retry(part) || plat_fs_mkdir(part)) {
        rt_log("host save import: cannot make %s", part);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        char *path = join(part, rel[i]);
        int bad = strlen(path) >= PATH_CAP || stage_file(path, data[i], size[i]);
        free(path);
        if (bad) {
            rt_log("host save import: cannot write %s into %s", rel[i], part);
            remove_tree(part);
            return -1;
        }
    }
    plat_fs_sync_dir(part);
    if (remove_tree_retry(imp) || move_dir(part, imp, MOVE_TRIES)) {
        rt_log("host save import: cannot put the import in place as %s", imp);
        remove_tree(part);
        return -1;
    }
    rt_log("host save import: %d files wait in %s for the next start", count, imp);
    return 0;
}

// A .import-<n> folder that a start could not make a file (a full disk, its rename refused) waits in save-files, and
// the next start finishes it and mounts it as save-import's would be: storage_files sets this, a Cancel clears it.
static volatile int32_t import_left;
static int import_drop_left(void);

int fs_save_import_waiting(void) {
    // the menu asks every frame: before the game's first storage use the start's own import_pending has not run, and
    // this is no reason to run it on the present thread
    if (!plat_cas32(&storage_ready, 0, 0)) return 0;
    char imp[1100];
    snprintf(imp, sizeof imp, "%s/save-import", profile);
    return plat_fs_kind(imp) == PLAT_FS_DIR || plat_cas32(&import_left, 0, 0);
}

int fs_save_import_cancel(void) {
    ready();
    char imp[1100];
    snprintf(imp, sizeof imp, "%s/save-import", profile);
    if (remove_tree_retry(imp) || import_drop_left()) return -1;
    rt_log("host save import: the import waiting for the next start was dropped");
    return 0;
}

// ---- save files
// A save file is one playthrough: a committed save with its own journal and backups. The profile folder itself is
// file 1, the save every profile had before; the others live under save-files, each laid out as the profile is:
//
//   <profile>/save/<uid>/, save-tx/, save-backup/   file 1
//   <profile>/save-name.txt                         file 1's name (UTF-8); absent: the menu calls it "File 1"
//   <profile>/save-files/<n>/                       file n (2, 3, ...): save/<uid>/, save-tx/, save-backup/ and
//                                                   save-name.txt
//   <profile>/save-files/current                    the number of the file the next start mounts; absent: file 1
//   <profile>/save-files/.new-<n>/                  file n while New or Copy makes it: deleted at the next start
//   <profile>/save-files/.import-<n>/               file n while a start makes it from save-import: finished at the
//                                                   next start (or dropped by the menu's Cancel)
//   <profile>/save-files/deleted/<UTC time>-<n>/    file n after Delete, laid out as a file (moved back to
//                                                   save-files/<m>, m unused, it is a file again); the newest three
//                                                   and any younger than 30 days are kept
//   <profile>/save-files/deleted/.part-<UTC time>-1/   file 1 while Delete moves it there: put back at the next start
//
// Nothing here runs or is written until the player makes, renames, deletes or picks a file: a profile without
// save-files mounts file 1 exactly as before. Each change is one durable rename or one atomic file write, so a crash
// leaves every file as it was or wholly in its new place: New and Copy build the file under .new-<n> and rename it
// into place, Rename and the choice of the next file rewrite one small file, Delete renames save-files/<n> into
// deleted. File 1 is four entries of the profile folder: Delete moves them one by one into a .part folder, and one
// rename of that folder to its name in deleted is the delete. Until that rename, everything in .part goes back: at once
// when a step fails without a crash (a full disk, a file another program holds), and what cannot go back then, or what
// a crash left, at the next start. So the files are what the menu says, also when it says the delete did not happen;
// the price is that a crash during a Delete of file 1 undoes it. Every start (storage_files) first finishes or
// drops those, makes a waiting import a file, then picks the file to mount: the one NX2NAVI_SAVE_FILE names
// (save_file in nx2navi.ini, --save-file; a number or a name) when set, else current, else file 1, else the lowest.
// The game mounts its save once, at boot, and keeps its progress in memory, so another file needs a restart; only
// the file this run mounts is written by the game, and neither it, the next start's file nor the one current names
// (save_file may pick another) can be deleted.
#define NAME_FILE "save-name.txt"
#define DELETED_KEEP 3
#define DELETED_DAYS 30
#define FILES_MAX 1024

static char files_root[1100], uid_dir[40];   // <profile>/save-files; the account folder's name, <uid hi><uid lo>
static int running_file;                     // the file this run mounts
static int import_file;                      // the file this start made from an import, 0 none
static PlatMutex files_lock = PLAT_MUTEX_INIT;   // one operation at a time; taken before tx_lock, never inside it

static void file_root(int id, char *out, size_t cap) {
    if (id == 1) snprintf(out, cap, "%s", profile);
    else snprintf(out, cap, "%s/%d", files_root, id);
}

// n of a decimal name "<n>" from 2 to 999999 without a leading zero, else 0.
static int file_number(const char *s) {
    if (*s < '1' || *s > '9') return 0;
    int n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 99999) return 0;
        n = n * 10 + (*s - '0');
    }
    return n >= 2 ? n : 0;
}

static int one_pending(void);

// File 1 is there while its committed save or its journal is, and no Delete of it waits half done (a .part
// folder holding some of it: playing file 1 then would make its folders again, empty, beside the moved ones); a file n
// while its folder is.
static int file_exists(int id) {
    char p[PATH_CAP];
    if (id >= 2) {
        file_root(id, p, sizeof p);
        return plat_fs_kind(p) == PLAT_FS_DIR;
    }
    if (id != 1) return 0;
    snprintf(p, sizeof p, "%s/save/%s", profile, uid_dir);
    int there = exists(p);
    snprintf(p, sizeof p, "%s/save-tx", profile);
    return (there || exists(p)) && !one_pending();
}

static int id_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int file_next(void);

// Every file, ascending; the running one is listed even before its first save. More files than max: the lowest, all
// of them read and sorted first (the directory's order is no order), with the running file and the next start's kept
// in place of the highest.
static int file_ids(int *ids, int max) {
    Listing l;
    list_dir(files_root, &l);
    int *all = malloc(sizeof *all * (size_t)(l.n + 1)), n = 0;
    need(all != NULL, "save file list");
    if (file_exists(1) || running_file == 1) all[n++] = 1;
    for (int i = 0; i < l.n; i++)
        if (l.e[i].kind == PLAT_FS_DIR && file_number(l.e[i].name)) all[n++] = file_number(l.e[i].name);
    listing_free(l.e, l.n);
    qsort(all, (size_t)n, sizeof *all, id_cmp);
    int k = n < max ? n : max;
    memcpy(ids, all, sizeof *ids * (size_t)k);
    if (n > max && max >= 2 && max < FILES_MAX) {   // (file_next lists up to FILES_MAX: no loop back here)
        int keep[2] = {running_file, file_next()};
        for (int j = 0; j < 2; j++) {
            if (!keep[j] || bsearch(&keep[j], ids, (size_t)k, sizeof *ids, id_cmp) ||
                !bsearch(&keep[j], all, (size_t)n, sizeof *all, id_cmp))
                continue;
            ids[ids[k - 1] == keep[1 - j] ? k - 2 : k - 1] = keep[j];
            qsort(ids, (size_t)k, sizeof *ids, id_cmp);
        }
    }
    free(all);
    return k;
}

// A name as the files keep it: control characters dropped, spaces trimmed, cut to FS_SAVE_NAME_MAX - 1 bytes at a
// character boundary. 0 when nothing is left.
static int name_clean(const char *in, char *out) {
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k + 1 < FS_SAVE_NAME_MAX; p++)
        if (*p >= 0x20 && *p != 0x7f) out[k++] = (char)*p;
    size_t lead = k;   // a character the cut split: from its first byte on
    while (lead && ((unsigned char)out[lead - 1] & 0xc0) == 0x80) lead--;
    if (lead) {
        unsigned char c = (unsigned char)out[lead - 1];
        size_t len = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (k - (lead - 1) < len) k = lead - 1;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    size_t skip = strspn(out, " ");
    memmove(out, out + skip, k - skip + 1);
    return out[0] != 0;
}

// The name the player gave the file; "" when it has none.
static void file_name(int id, char *out) {
    char root[PATH_CAP], p[PATH_CAP + 32];
    file_root(id, root, sizeof root);
    snprintf(p, sizeof p, "%s/%s", root, NAME_FILE);
    uint64_t n = 0;
    char *b = (char *)plat_file_read(p, &n);
    *out = 0;
    if (b) {
        b[strcspn(b, "\r\n")] = 0;
        if (!name_clean(b, out)) *out = 0;
    }
    free(b);
}

static int name_write(const char *root, const char *name) {
    char p[PATH_CAP + 32];
    snprintf(p, sizeof p, "%s/%s", root, NAME_FILE);
    return !failing(1) && plat_file_write_atomic(p, name, strlen(name)) ? 0 : -1;
}

// The number save-files/current holds; 0 when it holds none.
static int file_current(void) {
    char p[PATH_CAP], t[16];
    snprintf(p, sizeof p, "%s/current", files_root);
    uint64_t n = 0;
    uint8_t *b = plat_file_read(p, &n);
    int id = 0;
    if (b) {
        size_t k = n < sizeof t - 1 ? (size_t)n : sizeof t - 1;
        memcpy(t, b, k);
        t[k] = 0;
        t[strcspn(t, "\r\n ")] = 0;
        id = !strcmp(t, "1") ? 1 : file_number(t);
    }
    free(b);
    return id;
}

static int current_write(int id) {
    char p[PATH_CAP], t[16];
    plat_file_mkdir_p(files_root);
    snprintf(p, sizeof p, "%s/current", files_root);
    int n = snprintf(t, sizeof t, "%d\n", id);
    return !failing(1) && plat_file_write_atomic(p, t, (size_t)n) ? 0 : -1;
}

static int ascii_same(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | (*a >= 'A' && *a <= 'Z' ? 0x20 : 0)) != (*b | (*b >= 'A' && *b <= 'Z' ? 0x20 : 0))) return 0;
    return !*a && !*b;
}

// The file a setting names, by number or by name (ASCII letters in any case); 0 for none.
static int file_find(const char *want) {
    if (!strcmp(want, "1")) return 1;
    if (file_number(want)) return file_number(want);
    static int ids[FILES_MAX];
    int n = file_ids(ids, FILES_MAX);
    for (int i = 0; i < n; i++) {
        char name[FS_SAVE_NAME_MAX];
        file_name(ids[i], name);
        if (*name && ascii_same(name, want)) return ids[i];
    }
    return 0;
}

const char *fs_save_file_setting(void) {
    const char *v = cfg_get("NX2NAVI_SAVE_FILE");
    return v && *v ? v : NULL;
}

// Whether some file from 2 up is there.
static int files_numbered(void) {
    Listing l;
    if (list_dir(files_root, &l)) return 0;
    int found = 0;
    for (int i = 0; i < l.n && !found; i++) found = l.e[i].kind == PLAT_FS_DIR && file_number(l.e[i].name);
    listing_free(l.e, l.n);
    return found;
}

// The file save_file names by its name (not by a number); 0 for none. Renaming it would stop the next start.
static int file_named(void) {
    const char *want = fs_save_file_setting();
    return want && strcmp(want, "1") && !file_number(want) ? file_find(want) : 0;
}

// The file the next start mounts (file_pick's rule); 0 when the setting names no file.
static int file_next(void) {
    const char *want = fs_save_file_setting();
    if (want) {
        // 1 names the profile's own save, which a profile without other files has before its first save too; once
        // other files exist, a file 1 that is not there (deleted) stops the start like any other number
        int id = file_find(want);
        return id && (file_exists(id) || (id == 1 && !files_numbered())) ? id : 0;
    }
    int cur = file_current();
    if (cur && file_exists(cur)) return cur;
    if (file_exists(1)) return 1;
    static int ids[FILES_MAX];
    int n = file_ids(ids, FILES_MAX);
    return n ? ids[0] : 1;
}

// Start: the file to mount. A setting that names no file stops the start, as playing another file than the one asked
// for would put this run's saves into it.
static int file_pick(void) {
    int id = file_next(), cur = file_current();
    if (!id) {
        char list[600] = "";
        static int ids[FILES_MAX];
        int n = file_ids(ids, FILES_MAX);
        for (int i = 0, k = 0; i < n && k < (int)sizeof list - 80; i++) {
            char name[FS_SAVE_NAME_MAX];
            file_name(ids[i], name);
            k += snprintf(list + k, sizeof list - (size_t)k, "%s%d%s%s%s", i ? ", " : "", ids[i], *name ? " \"" : "",
                          name, *name ? "\"" : "");
        }
        rt_fatal("fs: save_file (nx2navi.ini, NX2NAVI_SAVE_FILE or --save-file) is \"%s\", which is no save file of %s; "
                 "the files are %s", fs_save_file_setting(), profile, n ? list : "none");
    }
    if (!fs_save_file_setting() && cur && cur != id)
        rt_log("fs: save-files/current names file %d, which is gone; file %d starts", cur, id);
    return id;
}

// The highest number any file, file in the making or deleted file has, plus one: a number is not used again while
// its file or its deleted folder is kept (once the prune removes a deleted file, its number can come back).
static int next_id(void) {
    int top = 1;
    Listing l;
    list_dir(files_root, &l);
    for (int i = 0; i < l.n; i++) {
        const char *s = l.e[i].name;
        int n = file_number(s);
        if (!n && !strncmp(s, ".new-", 5)) n = file_number(s + 5);
        if (!n && !strncmp(s, ".import-", 8)) n = file_number(s + 8);
        if (n > top) top = n;
    }
    listing_free(l.e, l.n);
    char del[PATH_CAP];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    list_dir(del, &l);
    for (int i = 0; i < l.n; i++) {
        const char *dash = strrchr(l.e[i].name, '-');
        int n = dash ? file_number(dash + 1) : 0;
        if (n > top) top = n;
    }
    listing_free(l.e, l.n);
    return top + 1;
}

// The newest save of a file this run does not mount, as save_recover would find it: the newest finished commit, else
// the committed save, else an interrupted commit's copy, else the save it replaced. 0 when it has none.
static int file_newest(int id, char *out, size_t cap) {
    char root[PATH_CAP], tx[PATH_CAP + 16];
    file_root(id, root, sizeof root);
    snprintf(tx, sizeof tx, "%s/save-tx", root);
    Listing l;
    list_dir(tx, &l);
    unsigned best = 0;
    for (int i = 0; i < l.n; i++)
        if (l.e[i].kind == PLAT_FS_DIR && ready_number(l.e[i].name) > best) {
            best = ready_number(l.e[i].name);
            snprintf(out, cap, "%s/%s", tx, l.e[i].name);
        }
    listing_free(l.e, l.n);
    if (best) return 1;
    snprintf(out, cap, "%s/save/%s", root, uid_dir);
    if (plat_fs_kind(out) == PLAT_FS_DIR) return 1;
    snprintf(out, cap, "%s/new", tx);
    if (plat_fs_kind(out) == PLAT_FS_DIR) return 1;
    snprintf(out, cap, "%s/old", tx);
    return plat_fs_kind(out) == PLAT_FS_DIR;
}

static uint64_t copy_lock_ns;   // how long the last Copy of the running file held the commit lock (the probe prints it)

// Every file of the tree dir to the disk, then its folders, as copy_tree's durable copy would have. 0 on success.
static int flush_tree(const char *dir) {
    Listing l;
    if (list_dir(dir, &l)) return -1;
    int bad = 0;
    for (int i = 0; i < l.n && !bad; i++) {
        char *p = join(dir, l.e[i].name);
        if (l.e[i].kind == PLAT_FS_DIR) {
            bad = flush_tree(p);
        } else if (l.e[i].kind == PLAT_FS_FILE) {
            PlatFile *f = plat_fs_open(p, 1, NULL);   // FlushFileBuffers wants a handle that may write
            bad = f && plat_file_fsync(f) ? 0 : -1;
            if (f) plat_file_close(f);
            if (!bad && crashed()) bad = -1;
        }
        free(p);
    }
    listing_free(l.e, l.n);
    if (!bad) plat_fs_sync_dir(dir);
    return bad;
}

// The newest save of file from into dst (made here), flushed. The running file's is copied under the commit lock, as
// the commit thread swaps commits in, but flushed after it: a Commit the game makes meanwhile waits for the lock, and
// the game's next save write for that Commit, so the lock is held only for the copy into the cache, not for a flush
// of every file (seconds on an SD card or a hard disk). A file with no save yet gives an empty folder. 0 on success.
static int copy_newest(int from, const char *dst) {
    int nfiles = 0, linked = 0, bad;
    uint64_t bytes = 0;
    char src[PATH_CAP + 32];
    if (from == running_file) {
        uint64_t t0 = plat_now_ns();
        plat_mutex_lock(&tx_lock);
        const char *newest = newest_save();
        bad = exists(newest) ? copy_tree(newest, dst, NULL, 0, &nfiles, &linked, &bytes) : files_mkdir(dst) ? -1 : 0;
        plat_mutex_unlock(&tx_lock);
        copy_lock_ns = plat_now_ns() - t0;
    } else {
        bad = file_newest(from, src, sizeof src) ? copy_tree(src, dst, NULL, 0, &nfiles, &linked, &bytes)
                                                 : files_mkdir(dst) ? -1 : 0;
    }
    return bad ? bad : flush_tree(dst);
}

// <UTC time> for a folder name, sorting in time order.
static void utc_stamp(char *out, size_t cap) {
    struct tm t;
    plat_gmtime((int64_t)(plat_unix_time_ns() / 1000000000u), &t);
    snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec);
}

// Seconds since 1970 of a <UTC time> at the start of s; -1 when s does not start with one.
static int64_t stamp_seconds(const char *s) {
    int y, mo, d, h, mi, se;
    if (strlen(s) < 15 || sscanf(s, "%4d%2d%2d-%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) != 6 || mo < 1 || mo > 12)
        return -1;
    y -= mo <= 2;   // days from 1970-01-01 of the civil date, with March as the first month of the year
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + se;
}

// A folder, or a link (symbolic link, junction) to one: make_dir's rule, as players move folders to other drives.
static int dir_there(const char *p) {
    int k = plat_fs_kind(p);
    return k == PLAT_FS_DIR || (k == PLAT_FS_LINK && plat_dir_exists(p));
}

// A folder there, made with its parents when missing. Only the making needs room on the disk (the probe's full disk).
static int make_dir_ok(const char *p) {
    if (!dir_there(p) && !failing(1)) plat_file_mkdir_p(p);
    return dir_there(p);
}

// Entry i of file 1's four (its name, committed save, journal and backups, in the order Delete moves them) in base: the
// profile, or a .part folder laid out like it. sub: the folder it is in.
static void one_entry(const char *base, int i, char *path, size_t cap, char *sub, size_t sub_cap) {
    static const char *const parents[] = {"", "save", "", ""};
    const char *names[] = {NAME_FILE, uid_dir, "save-tx", "save-backup"};
    snprintf(sub, sub_cap, "%s%s%s", base, *parents[i] ? "/" : "", parents[i]);
    snprintf(path, cap, "%s/%s", sub, names[i]);
}

// An entry in the way of a move: an empty folder (a start made file 1's folders again, an older build's start too)
// goes; one with anything in it stays, as nothing here deletes a save. 1 when p is not there now.
static int out_of_the_way(const char *p) {
    return !exists(p) || (plat_fs_kind(p) == PLAT_FS_DIR && !plat_fs_rmdir(p));
}

// File 1's entries that are still in the profile folder into part, the name first; 0 when none is left behind.
static int one_move(const char *part) {
    for (int i = 0; i < 4; i++) {
        char from[PATH_CAP + 64], to[PATH_CAP + 128], sub[PATH_CAP + 64], dsub[PATH_CAP + 128];
        one_entry(profile, i, from, sizeof from, sub, sizeof sub);
        one_entry(part, i, to, sizeof to, dsub, sizeof dsub);
        if (!exists(from)) continue;
        if (exists(to) && !out_of_the_way(from)) {
            rt_log("fs: save files: %s and %s both hold data; the delete of file 1 waits", from, to);
            return -1;
        }
        if (!exists(from)) continue;
        if (!make_dir_ok(dsub) || move_dir(from, to, MOVE_TRIES) || crashed()) return -1;
    }
    return 0;
}

// What a Delete of file 1 moved into its .part folder dir back into the profile, the name last, then dir and its
// emptied save folder go (rmdir only: anything else found there stays). Nothing goes back while any of it would land
// on data (an older build's start that made a new file 1 meanwhile): parts of two saves never meet in one file then,
// and dir waits. 0 when file 1 is whole again.
static int one_back(const char *dir) {
    for (int i = 0; i < 4; i++) {
        char from[PATH_CAP + 128], to[PATH_CAP + 64], sub[PATH_CAP + 128], dsub[PATH_CAP + 64];
        one_entry(dir, i, from, sizeof from, sub, sizeof sub);
        one_entry(profile, i, to, sizeof to, dsub, sizeof dsub);
        if (exists(from) && !out_of_the_way(to)) {
            rt_log("fs: save files: %s and %s both hold data; file 1 stays in %s", from, to, dir);
            return -1;
        }
    }
    int bad = 0;
    for (int i = 3; i >= 0; i--) {
        char from[PATH_CAP + 128], to[PATH_CAP + 64], sub[PATH_CAP + 128], dsub[PATH_CAP + 64];
        one_entry(dir, i, from, sizeof from, sub, sizeof sub);
        one_entry(profile, i, to, sizeof to, dsub, sizeof dsub);
        if (exists(from) && (!make_dir_ok(dsub) || move_dir(from, to, MOVE_TRIES))) bad = -1;   // the others still go
        if (crashed()) return -1;
    }
    if (bad) return -1;
    char p[PATH_CAP + 128];
    snprintf(p, sizeof p, "%s/save", dir);
    if (exists(p)) plat_fs_rmdir(p);
    if (plat_fs_rmdir(dir)) rt_log("fs: save files: %s stays: something else is in it", dir);
    return 0;
}

// A folder of deleted named .part-<UTC time>-1 that holds any of file 1's entries.
static int one_left_in(const char *name, const char *dir) {
    if (strncmp(name, ".part-", 6)) return 0;
    for (int i = 0; i < 4; i++) {
        char p[PATH_CAP + 128], sub[PATH_CAP + 128];
        one_entry(dir, i, p, sizeof p, sub, sizeof sub);
        if (exists(p)) return 1;
    }
    return 0;
}

// A Delete of file 1 waits half done: some of file 1 is in a .part folder, which the next start puts back.
static int one_pending(void) {
    char del[PATH_CAP];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    Listing l;
    if (list_dir(del, &l)) return 0;
    int found = 0;
    for (int i = 0; i < l.n && !found; i++) {
        char *p = join(del, l.e[i].name);
        found = one_left_in(l.e[i].name, p);
        free(p);
    }
    listing_free(l.e, l.n);
    return found;
}

// File 1's entries into its fresh .part folder, then that folder to its place in deleted: the one rename that is the
// delete. 0 on success.
static int one_finish(const char *part) {
    char to[PATH_CAP + 64];
    const char *name = strrchr(part, '/') + 1 + 6;   // ".part-<UTC time>-1"
    snprintf(to, sizeof to, "%s/deleted/%s", files_root, name);
    if (one_move(part) || move_dir(part, to, MOVE_TRIES)) return -1;
    return crashed() ? -1 : 0;
}

// An import moved into its .import-<n> folder becomes file n and the next start's file; one not moved yet is dropped
// (save-import still holds it). 0 when file n is in place.
static int import_finish(const char *stage, int id) {
    char acct[PATH_CAP + 64], dst[PATH_CAP];
    snprintf(acct, sizeof acct, "%s/save/%s", stage, uid_dir);
    if (!exists(acct)) {
        remove_tree(stage);
        return -1;
    }
    file_root(id, dst, sizeof dst);
    // current first: a crash before the rename leaves the folder for the next start, which finishes it the same way
    if (current_write(id) || crashed() || move_dir(stage, dst, MOVE_TRIES) || crashed()) {
        if (!tx_dead) rt_log("host save import: cannot make %s save file %d; the next start tries again", stage, id);
        return -1;
    }
    import_file = id;
    rt_log("host save import: the save imported in the menu is save file %d now", id);
    return 0;
}

// A save the menu imported becomes a new file, which the next file pick takes (section "a shared save").
static void import_pending(void) {
    char imp[1100], part[1100];
    snprintf(imp, sizeof imp, "%s/save-import", profile);
    snprintf(part, sizeof part, "%s/save-import.part", profile);
    if (exists(part) && remove_tree(part)) rt_log("host save import: cannot delete the unfinished %s", part);
    if (plat_fs_kind(imp) != PLAT_FS_DIR) return;
    int id = next_id();
    char stage[PATH_CAP], save[PATH_CAP + 8], acct[PATH_CAP + 64];
    snprintf(stage, sizeof stage, "%s/.import-%d", files_root, id);
    snprintf(save, sizeof save, "%s/save", stage);
    snprintf(acct, sizeof acct, "%s/%s", save, uid_dir);
    steps_reset();
    if (!make_dir_ok(files_root) || files_mkdir(stage) || files_mkdir(save) || crashed() ||
        move_dir(imp, acct, MOVE_TRIES)) {
        if (!tx_dead) {
            rt_log("host save import: cannot move %s into %s; it waits for the next start", imp, stage);
            remove_tree(stage);
        }
        return;
    }
    if (!crashed()) import_finish(stage, id);
}

// Deleted files beyond the newest DELETED_KEEP that are older than DELETED_DAYS go for good.
static void deleted_prune(void) {
    char del[PATH_CAP];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    Listing l;
    if (list_dir(del, &l)) return;
    qsort(l.e, (size_t)l.n, sizeof *l.e, listed_cmp);   // oldest first: the names start with their time
    int64_t now = (int64_t)(plat_unix_time_ns() / 1000000000u);
    int dated = 0;
    for (int i = 0; i < l.n; i++) dated += l.e[i].kind == PLAT_FS_DIR && stamp_seconds(l.e[i].name) >= 0;
    for (int i = 0; i < l.n && dated > DELETED_KEEP; i++) {
        int64_t t = l.e[i].kind == PLAT_FS_DIR ? stamp_seconds(l.e[i].name) : -1;
        if (t < 0) continue;
        dated--;
        if (now - t < (int64_t)DELETED_DAYS * 86400) break;
        char *p = join(del, l.e[i].name);
        if (!remove_tree(p)) rt_log("fs: save files: deleted file %s was kept %d days; gone now", l.e[i].name, DELETED_DAYS);
        free(p);
    }
    listing_free(l.e, l.n);
}

// Start: a file operation a crash cut short is finished (an import) or undone (New, Copy, the delete of file 1), and
// what a Delete of file 1 that failed could not put back goes back now.
static void files_recover(void) {
    if (!dir_there(files_root)) return;   // never used: nothing to finish
    Listing l;
    list_dir(files_root, &l);
    for (int i = 0; i < l.n; i++) {
        const char *s = l.e[i].name;
        char *p = join(files_root, s);
        if (!strncmp(s, ".new-", 5)) {
            rt_log("fs: save files: dropped %s, a file New or Copy did not finish", s);
            remove_tree(p);
        } else if (!strncmp(s, ".import-", 8) && file_number(s + 8)) {
            import_finish(p, file_number(s + 8));
        }
        free(p);
    }
    listing_free(l.e, l.n);
    char del[PATH_CAP];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    list_dir(del, &l);
    for (int i = 0; i < l.n; i++) {
        const char *s = l.e[i].name;
        char *p = join(del, s);
        if (one_left_in(s, p))   // never finished here: the menu said the delete did not happen, or said nothing
            rt_log("fs: save files: %s file 1 from %s, whose delete did not finish",
                   one_back(p) ? "cannot put back all of" : "put back", s);
        else if (!strncmp(s, ".part-", 6))   // nothing of file 1 in it: a Delete that moved nothing left it
            one_back(p);
        free(p);
    }
    listing_free(l.e, l.n);
    deleted_prune();
}

// The .import-<n> folders in save-files: how many there are, or with drop how many could not be deleted.
static int import_folders(int drop) {
    Listing l;
    if (list_dir(files_root, &l)) return 0;
    int n = 0;
    for (int i = 0; i < l.n; i++) {
        if (strncmp(l.e[i].name, ".import-", 8) || !file_number(l.e[i].name + 8)) continue;
        char *p = join(files_root, l.e[i].name);
        n += !drop || remove_tree_retry(p);
        free(p);
    }
    listing_free(l.e, l.n);
    return n;
}

// The menu's Cancel: an import a start could not finish goes too.
static int import_drop_left(void) {
    plat_mutex_lock(&files_lock);
    int left = import_folders(1);
    plat_xchg32(&import_left, left > 0);
    plat_mutex_unlock(&files_lock);
    return left ? -1 : 0;
}

int fs_save_files(FsSaveFile *out, int max) {
    ready();
    static int ids[FILES_MAX];
    plat_mutex_lock(&files_lock);
    int n = file_ids(ids, max < FILES_MAX ? max : FILES_MAX), next = file_next(), cur = file_current(),
        named = file_named();
    for (int i = 0; i < n; i++) {
        FsSaveFile *f = &out[i];
        memset(f, 0, sizeof *f);
        f->id = ids[i];
        file_name(f->id, f->name);
        f->running = f->id == running_file;
        f->next = f->id == next;
        f->current = f->id == cur;
        f->named = f->id == named;
        char src[PATH_CAP + 32];
        if (f->running) {
            plat_mutex_lock(&tx_lock);
            save_summary(newest_save(), &f->sum);
            plat_mutex_unlock(&tx_lock);
        } else if (file_newest(f->id, src, sizeof src)) {
            save_summary(src, &f->sum);
        } else {
            save_summary("", &f->sum);
        }
    }
    plat_mutex_unlock(&files_lock);
    return n;
}

int fs_save_file_running(void) {
    ready();
    return running_file;
}

// The import this start made a file, when this start mounts it: save_file (or --save-file) may pick another file, and
// the menu must not say the game loaded the import then.
int fs_save_file_import(void) {
    return plat_cas32(&storage_ready, 0, 0) && import_file == running_file ? import_file : 0;
}

int fs_save_file_next_id(void) {
    ready();
    plat_mutex_lock(&files_lock);
    int id = next_id();
    plat_mutex_unlock(&files_lock);
    return id;
}

int fs_save_file_new(const char *name, int from) {
    ready();
    char clean[FS_SAVE_NAME_MAX];
    if (!name_clean(name, clean)) return FS_FILE_FAILED;
    plat_mutex_lock(&files_lock);
    if (from && !file_exists(from) && from != running_file) {
        plat_mutex_unlock(&files_lock);
        rt_log("fs: save files: no copy of file %d: it is not there", from);
        return FS_FILE_GONE;
    }
    steps_reset();
    int id = next_id(), ok = 0;
    char tmp[PATH_CAP], save[PATH_CAP + 8], acct[PATH_CAP + 64], dst[PATH_CAP];
    snprintf(tmp, sizeof tmp, "%s/.new-%d", files_root, id);
    snprintf(save, sizeof save, "%s/save", tmp);
    snprintf(acct, sizeof acct, "%s/%s", save, uid_dir);
    file_root(id, dst, sizeof dst);
    if (make_dir_ok(files_root) && !files_mkdir(tmp) && !crashed() && !files_mkdir(save)) {
        ok = !(from ? copy_newest(from, acct) : files_mkdir(acct)) && !crashed() && !name_write(tmp, clean) &&
             !crashed();
        if (ok) plat_fs_sync_dir(tmp);
        ok = ok && !move_dir(tmp, dst, MOVE_TRIES) && !crashed();
    }
    if (!ok && !tx_dead) remove_tree(tmp);
    if (ok) rt_log("fs: save files: made file %d \"%s\"%s", id, clean, from ? ", a copy of the newest save of another" : "");
    else if (!tx_dead) rt_log("fs: save files: cannot make a file in %s", files_root);
    plat_mutex_unlock(&files_lock);
    return ok ? id : FS_FILE_FAILED;
}

int fs_save_file_rename(int id, const char *name) {
    ready();
    char clean[FS_SAVE_NAME_MAX], root[PATH_CAP];
    if (!name_clean(name, clean)) return FS_FILE_FAILED;
    plat_mutex_lock(&files_lock);
    steps_reset();
    // save_file picks the lowest file of its name: another file given that name could take its place at the next start
    const char *set = fs_save_file_setting();
    int r = !file_exists(id) && id != running_file ? FS_FILE_GONE
            : id == file_named()                    ? FS_FILE_IN_USE
            : set && strcmp(set, "1") && !file_number(set) && ascii_same(clean, set) ? FS_FILE_NAME_SET
                                                                                      : 0;
    if (r)   // the menu says "the log says why", or for FS_FILE_NAME_SET why itself
        rt_log("fs: save files: file %d not renamed: %s", id,
               r == FS_FILE_GONE     ? "it is not there"
               : r == FS_FILE_IN_USE ? "save_file names it by its name, and the next start would stop"
                                     : "save_file picks the file of that name, and this one could take its place");
    file_root(id, root, sizeof root);
    if (!r && name_write(root, clean)) {
        r = FS_FILE_FAILED;
        rt_log("fs: save files: cannot write the name of file %d into %s", id, root);
    }
    plat_mutex_unlock(&files_lock);
    return r;
}

int fs_save_file_choose(int id) {
    ready();
    plat_mutex_lock(&files_lock);
    steps_reset();
    int r = !file_exists(id) && id != running_file ? FS_FILE_GONE : current_write(id) ? FS_FILE_FAILED : 0;
    if (!r) rt_log("fs: save files: the next start mounts file %d", id);
    else rt_log("fs: save files: file %d not picked: %s", id, r == FS_FILE_GONE ? "it is not there" : "save-files/current "
                "cannot be written");
    plat_mutex_unlock(&files_lock);
    return r;
}

int fs_save_file_delete(int id) {
    ready();
    plat_mutex_lock(&files_lock);
    steps_reset();
    int r = 0;
    char stamp[24], del[PATH_CAP], to[PATH_CAP + 48];
    if (!file_exists(id)) r = FS_FILE_GONE;
    else if (id == running_file || id == file_next() || id == file_current()) r = FS_FILE_IN_USE;
    if (r)   // the menu says "the log says why"
        rt_log("fs: save files: file %d not deleted: %s", id,
               r == FS_FILE_GONE              ? "it is not there"
               : id == running_file           ? "this run plays it"
               : id == file_next()            ? "the next start loads it"
                                              : "save-files/current names it (save_file picks another file now)");
    utc_stamp(stamp, sizeof stamp);
    snprintf(del, sizeof del, "%s/deleted", files_root);
    if (!r && !make_dir_ok(del)) r = FS_FILE_FAILED;
    if (!r && id >= 2) {
        char root[PATH_CAP];
        file_root(id, root, sizeof root);
        snprintf(to, sizeof to, "%s/%s-%d", del, stamp, id);
        if (move_dir(root, to, MOVE_TRIES) || crashed()) r = FS_FILE_FAILED;
    } else if (!r) {
        // Until the rename of .part that is the delete, what moved goes back, now or at the next start: the menu says
        // the delete did not happen.
        snprintf(to, sizeof to, "%s/.part-%s-1", del, stamp);
        if (files_mkdir(to) || crashed()) {
            r = FS_FILE_FAILED;
        } else if (one_finish(to)) {
            r = FS_FILE_FAILED;
            if (!tx_dead && one_back(to) && !tx_dead)
                rt_log("fs: save files: the delete of file 1 failed and not all of it went back; %s holds the rest, "
                       "which the next start puts back", to);
        }
    }
    if (!r) rt_log("fs: save files: file %d moved to %s", id, del);
    else if (r == FS_FILE_FAILED && !tx_dead) rt_log("fs: save files: cannot move file %d to %s", id, del);
    plat_mutex_unlock(&files_lock);
    return r;
}

// Start: the file this start mounts and its folders, its journal recovered (section "save transactions").
static void storage_files(void) {
    import_file = 0;
    files_recover();
    import_pending();
    plat_xchg32(&import_left, import_folders(0) > 0);
    running_file = file_pick();
    if (import_file)
        rt_log("host save import: this start mounts %s", import_file == running_file ? "it" : "another file, as save_file asks");
    char base[PATH_CAP], p[PATH_CAP + 8];
    file_root(running_file, base, sizeof base);
    need(snprintf(save_root, sizeof save_root, "%s/save/%s", base, uid_dir) < (int)sizeof save_root, "save path");
    need(snprintf(tx_root, sizeof tx_root, "%s/save-tx", base) < (int)sizeof tx_root, "save path");
    snprintf(work_root, sizeof work_root, "%s/work", tx_root);
    snprintf(new_root, sizeof new_root, "%s/new", tx_root);
    snprintf(old_root, sizeof old_root, "%s/old", tx_root);
    snprintf(backup_root, sizeof backup_root, "%s/save-backup", base);
    snprintf(p, sizeof p, "%s/save", base);
    make_dirs(p);
    make_dirs(tx_root);
    make_dirs(backup_root);
    save_recover();   // before save_root is created: an absent save_root means a commit to complete
    make_dirs(save_root);
    char name[FS_SAVE_NAME_MAX];
    file_name(running_file, name);
    rt_log("host save file: %d%s%s%s (%s)%s", running_file, *name ? " \"" : "", name, *name ? "\"" : "", base,
           fs_save_file_setting() ? ", as save_file asks" : "");
}

// ---- storage roots
// One game per profile: a second one's start would take the first one's commit in progress for an interrupted one
// and delete it, and both would write into one working copy (save-tx/work), losing or mixing their saves. rt_boot
// takes the lock before the shader prewarm, so a second copy ends at once instead of after the prewarm's seconds of
// work on every core and the gigabytes of memory it commits; storage_init takes it again (plat_lock_file keeps one
// handle per path, so the second call holds it already).
void fs_host_lock_profile(void) {
    if (!*profile)
        need(snprintf(profile, sizeof profile, "%s", rt_config()->profile) < (int)sizeof profile, "profile path");
    char p[PATH_CAP];
    make_dirs(profile);
    snprintf(p, sizeof p, "%s/instance.lock", profile);
    int held = plat_lock_file(p);
    if (!held) rt_fatal("fs: another copy of the game is running with the profile %s; close it first", profile);
    if (held < 0) rt_log("fs: cannot make %s; a second copy of the game would not be stopped", p);
}

static void storage_init(void *param) {
    (void)param;
    uint64_t uid[2];
    char p[PATH_CAP];
    fs_host_lock_profile();
    account_uid(uid);
    snprintf(uid_dir, sizeof uid_dir, "%016llx%016llx", (unsigned long long)uid[1], (unsigned long long)uid[0]);
    need(snprintf(files_root, sizeof files_root, "%s/save-files", profile) < (int)sizeof files_root, "save path");
    storage_files();
    snprintf(p, sizeof p, "%s/temp", profile);
    remove_tree(p);
    make_dirs(p);
    snprintf(p, sizeof p, "%s/cache", profile);
    make_dirs(p);
    plat_xchg32(&storage_ready, 1);
    rt_log("host save storage: %s", save_root);
    return;
}

static void ready(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, storage_init, NULL);
}

void fs_host_set_profile(const char *dir) {
    if (plat_cas32(&storage_ready, 0, 0)) rt_fatal("fs_host_set_profile after the first storage use");
    need(dir && snprintf(profile, sizeof profile, "%s", dir) < (int)sizeof profile, "profile path");
}

// ---- mounts and paths
static uint32_t mount_add(const char *name, int kind, const char *root) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return FS_R_INVALID_ARGUMENT;
    char as_path[40];
    snprintf(as_path, sizeof as_path, "%s:", name);
    if (romfs_owns(as_path)) return FS_R_ALREADY_EXISTS;   // the RomFS mount's name
    plat_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < NMOUNTS; i++) {
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) {
            plat_mutex_unlock(&lock);
            return FS_R_ALREADY_EXISTS;
        }
        if (!mounts[i].kind && slot < 0) slot = i;
    }
    if (slot < 0) rt_fatal("fs mount table full");
    mounts[slot].kind = kind;
    strcpy(mounts[slot].name, name);
    snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> %s", name, root);
    return 0;
}

// A path component every host stores under exactly this name: not empty, ".", "..", no reserved character, no trailing
// dot or space (Windows strips those, which would alias two names).
static int component_ok(const char *c, size_t n) {
    if (!n || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) return 0;
    if (c[n - 1] == '.' || c[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)c[i] < 32 || strchr("\\:*?\"<>|", c[i])) return 0;
    return 1;
}

// "name:/a/b" -> host path; 0 on success; *kind = the mount's kind (M_SAVE ...) when kind is given.
static uint32_t host_path_kind(const char *path, char *out, size_t cap, int *kind) {
    if (!path) return FS_R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return FS_R_INVALID_PATH;
    size_t n = (size_t)(colon - path);
    char root[1024];
    int k = M_FREE;
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) {
            strcpy(root, mounts[i].root);
            k = mounts[i].kind;
        }
    plat_mutex_unlock_shared(&lock);
    if (k == M_FREE) return FS_R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    const char *rest = colon + 1;
    for (const char *q = rest + 1; *q;) {
        const char *e = strchr(q, '/');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        if (!e && !len) break;
        if (!component_ok(q, len)) return FS_R_INVALID_PATH;
        q = e ? e + 1 : q + len;
    }
    if (snprintf(out, cap, "%s%s", root, rest) >= (int)cap) return FS_R_INVALID_PATH;
    size_t L = strlen(out);
    while (L > strlen(root) && out[L - 1] == '/') out[--L] = 0;
    return 0;
}

static uint32_t host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }

// A change to save data through a path (create, delete, rename, directories) marks the save as in progress.
static uint32_t save_change_path(const char *path, char *out, size_t cap) {
    int kind = M_FREE;
    uint32_t r = host_path_kind(path, out, cap, &kind);
    if (!r && kind == M_SAVE_RO) return FS_R_UNSUPPORTED_OPEN;
    if (!r && kind == M_SAVE) save_touched();
    return r;
}

static uint32_t fs_result(int e) {
    switch (e) {
    case PLAT_FS_OK: return 0;
    case PLAT_FS_NOT_FOUND: return FS_R_PATH_NOT_FOUND;
    case PLAT_FS_EXISTS: return FS_R_ALREADY_EXISTS;
    case PLAT_FS_NOT_EMPTY: return FS_R_NOT_EMPTY;
    default: return FS_R_INVALID_ARGUMENT;
    }
}

// ---- storage mounts
static int my_uid(const uint64_t *uid) {
    uint64_t me[2];
    account_uid(me);
    return uid && uid[0] == me[0] && uid[1] == me[1];
}

static uint32_t mount_save(const char *name, const uint64_t *uid) {
    ready();
    if (!my_uid(uid)) return FS_R_TARGET_NOT_FOUND;
    plat_mutex_lock(&tx_lock);
    if (!save_mounts) work_create();
    uint32_t r = mount_add(name, M_SAVE, work_root);
    if (!r) save_mounts++;
    else if (!save_mounts) remove_tree(work_root);
    plat_mutex_unlock(&tx_lock);
    return r;
}

static uint8_t save_exists(uint64_t application, const uint64_t *uid) {
    ready();
    return application == NX2NAVI_APPLICATION_ID && my_uid(uid);
}

// Other titles' save data (data-link bonuses) is not present on this host.
static uint32_t mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return FS_R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE_RO, save_root);
}

static uint32_t mount_temp(const char *name) {
    ready();
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/temp", profile);
    return mount_add(name, M_TEMP, p);
}

static void cache_dir(int index, char *out, size_t cap) { snprintf(out, cap, "%s/cache/%d", profile, index); }

static uint32_t create_cache(int index, int64_t size, int64_t journal) {
    ready();
    if (index < 0 || size < 0 || journal < 0) return FS_R_INVALID_ARGUMENT;
    char p[PATH_CAP];
    cache_dir(index, p, sizeof p);
    return fs_result(plat_fs_mkdir(p));
}

static uint32_t delete_cache(int index) {
    ready();
    char p[PATH_CAP];
    cache_dir(index, p, sizeof p);
    return remove_tree(p) ? FS_R_TARGET_NOT_FOUND : 0;
}

static uint32_t mount_cache(const char *name, int index) {
    ready();
    char p[PATH_CAP];
    cache_dir(index, p, sizeof p);
    if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_TARGET_NOT_FOUND;
    return mount_add(name, M_CACHE, p);
}

typedef struct { int32_t index; uint8_t reserved[0x1c]; } CacheStorageInfo;
_Static_assert(sizeof(CacheStorageInfo) == 0x20, "nn::fs::CacheStorageInfo");
typedef struct { int list[64]; int count, next; } CacheList;

static uint32_t open_cache_list(uint64_t *out) {
    ready();
    if (!out) return FS_R_INVALID_ARGUMENT;
    CacheList *l = calloc(1, sizeof *l);
    need(l != NULL, "cache list allocation");
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/cache", profile);
    Listing ls;
    list_dir(p, &ls);
    for (int i = 0; i < ls.n; i++) {
        char *end;
        long v = strtol(ls.e[i].name, &end, 10);
        if (ls.e[i].kind == PLAT_FS_DIR && *ls.e[i].name && !*end && v >= 0 && l->count < 64) l->list[l->count++] = (int)v;
    }
    listing_free(ls.e, ls.n);
    *out = (uint64_t)(uintptr_t)l;
    return 0;
}

static uint32_t read_cache_list(int *count, CacheStorageInfo *out, uint64_t h, int max) {
    CacheList *l = (CacheList *)(uintptr_t)h;
    if (!l || !count || max < 0 || (!out && max)) return FS_R_INVALID_ARGUMENT;
    int n = 0;
    while (n < max && l->next < l->count) {
        memset(&out[n], 0, sizeof *out);
        out[n++].index = l->list[l->next++];
    }
    *count = n;
    return 0;
}

static void close_cache_list(uint64_t h) { free((void *)(uintptr_t)h); }

static void unmount(const char *name) {
    save_wait_commit();   // else the working copy could go before a Commit queued just now has copied it
    plat_mutex_lock(&tx_lock);
    plat_mutex_lock(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (name && mounts[i].kind && !strcmp(mounts[i].name, name)) {
            int save = mounts[i].kind == M_SAVE;
            mounts[i].kind = M_FREE;
            plat_mutex_unlock(&lock);
            if (save && !--save_mounts) work_discard();
            plat_mutex_unlock(&tx_lock);
            rt_log("fs unmount %s", name);
            return;
        }
    plat_mutex_unlock(&lock);
    plat_mutex_unlock(&tx_lock);
    size_t n = name ? strlen(name) : 0;
    char probe[40];
    if (!n || n >= 32 || (snprintf(probe, sizeof probe, "%s:", name), !romfs_owns(probe)))
        rt_fatal("fs: unmount of unknown mount %s", name ? name : "(null)");
}

static uint32_t commit(const char *name) {
    int found = 0, save = 0;
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (name && mounts[i].kind && !strcmp(mounts[i].name, name)) found = 1, save = mounts[i].kind == M_SAVE;
    plat_mutex_unlock_shared(&lock);
    if (!found) return FS_R_PATH_NOT_FOUND;
    if (save) return save_commit();   // durability comes from the staging copy, not the working copy's handles
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NFILES; i++)
        if (files[i].h && (files[i].mode & FS_OPEN_WRITE)) plat_file_fsync(files[i].h);
    plat_mutex_unlock_shared(&lock);
    return 0;
}

// ---- files
static File *file_of(uint64_t h, PlatFile **fh, int *mode) {
    if (h < FILE_BASE || h >= FILE_BASE + NFILES) return NULL;
    plat_mutex_lock_shared(&lock);
    File *f = &files[h - FILE_BASE];
    PlatFile *v = f->h;
    if (fh) *fh = f->h;
    if (mode) *mode = f->mode;
    plat_mutex_unlock_shared(&lock);
    return v ? f : NULL;
}

// Positional reads: bytes read at off, 0 at the end, -1 on an error.
static int64_t host_read_at(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    mem_thaw((uintptr_t)buf, n);   // the kernel's write would fail on a frozen page instead of faulting
    int64_t got = plat_file_pread(f, buf, n, off);
    if (got == -2)   // a read-only page in the buffer: a frozen region missed its thaw
        rt_log("fs: read of %u bytes into %p refused: the buffer is not writable", n, buf);
    return got < 0 ? -1 : got;
}

static uint32_t open_file(uint64_t *out, const char *path, int mode) {
    if (romfs_owns(path)) return romfs_open(out, path, mode);
    fs_trace(mode & FS_OPEN_WRITE ? "write" : "read", path);
    if (!out || !(mode & (FS_OPEN_READ | FS_OPEN_WRITE)) || (mode & ~7)) return FS_R_INVALID_ARGUMENT;
    char p[PATH_CAP];
    int kind = M_FREE;
    uint32_t r = host_path_kind(path, p, sizeof p, &kind);
    if (r) return r;
    if (plat_fs_kind(p) != PLAT_FS_FILE) return FS_R_PATH_NOT_FOUND;
    if (kind == M_SAVE_RO && (mode & FS_OPEN_WRITE)) return FS_R_UNSUPPORTED_OPEN;
    int save_writer = kind == M_SAVE && (mode & FS_OPEN_WRITE);
    int err = 0;
    PlatFile *f = plat_fs_open(p, (mode & FS_OPEN_WRITE) != 0, &err);
    if (!f) return fs_result(err);
    plat_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++)
        if (!files[i].h) {
            files[i] = (File){f, mode, save_writer};
            *out = FILE_BASE + (uint64_t)i;
            plat_mutex_unlock(&lock);
            if (save_writer) {
                plat_inc32(&save_writers);
                save_touched();
            }
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("fs: host file handle table full (%d)", NFILES);
}

static uint32_t read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (romfs_handle(h)) return romfs_read(out, h, offset, buffer, size);
    PlatFile *f;
    int mode;
    if (!out || offset < 0 || (!buffer && size) || !file_of(h, &f, &mode)) return FS_R_INVALID_ARGUMENT;
    if (!(mode & FS_OPEN_READ)) return FS_R_OPEN_MODE;
    uint64_t done = 0;
    while (done < size) {
        uint32_t chunk = size - done > 0x40000000 ? 0x40000000 : (uint32_t)(size - done);
        int64_t n = host_read_at(f, (uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (n < 0) rt_fatal("fs: host save read failure");
        if (!n) break;
        done += (uint64_t)n;
    }
    *out = done;
    return 0;
}

static uint32_t read_exact(uint64_t h, int64_t off, void *buf, uint64_t size) {
    uint64_t n = 0;
    uint32_t r = read_file(&n, h, off, buf, size);
    return r ? r : n == size ? 0 : FS_R_INVALID_ARGUMENT;
}

static uint32_t write_file(uint64_t h, int64_t offset, const void *buffer, uint64_t size, const int32_t *option) {
    PlatFile *f;
    int mode;
    File *file = offset < 0 || (!buffer && size) ? NULL : file_of(h, &f, &mode);
    if (!file) return FS_R_INVALID_ARGUMENT;
    if (!(mode & FS_OPEN_WRITE)) return FS_R_OPEN_MODE;
    PlatFileId id;
    if (!plat_file_id(f, &id)) rt_fatal("fs: host save size");
    // offset + size without the sum: a size near 2^64 would wrap it below the file's size
    if ((size > id.size || (uint64_t)offset > id.size - size) && !(mode & FS_OPEN_APPEND)) return FS_R_APPEND_REQUIRED;
    if (file->save) save_touched();
    for (uint64_t done = 0; done < size;) {
        uint32_t chunk = size - done > 0x40000000 ? 0x40000000 : (uint32_t)(size - done);
        // NX2NAVI_WB_PROBE: the kernel reads the buffer, and a probed page would fail the write instead of faulting
        if (mem_probing()) mem_thaw((uintptr_t)buffer + done, chunk);
        int64_t put = plat_file_pwrite(f, (const uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (put <= 0) rt_fatal("fs: host save write failure");
        done += (uint64_t)put;
    }
    if (option && (*option & 1)) plat_file_fsync(f);   // WriteOptionFlag_Flush
    return 0;
}

static uint32_t file_size(int64_t *out, uint64_t h) {
    if (romfs_handle(h)) return romfs_size(out, h);
    PlatFile *f;
    PlatFileId id;
    if (!out || !file_of(h, &f, NULL) || !plat_file_id(f, &id)) return FS_R_INVALID_ARGUMENT;
    *out = (int64_t)id.size;
    return 0;
}

static uint32_t set_file_size(uint64_t h, int64_t size) {
    PlatFile *f;
    int mode;
    File *file = size < 0 ? NULL : file_of(h, &f, &mode);
    if (!file) return FS_R_INVALID_ARGUMENT;
    if (!(mode & FS_OPEN_WRITE)) return FS_R_OPEN_MODE;
    if (file->save) save_touched();
    return fs_result(plat_file_set_size(f, (uint64_t)size));
}

static uint32_t flush_file(uint64_t h) {
    PlatFile *f;
    int mode;
    if (!file_of(h, &f, &mode)) return FS_R_INVALID_ARGUMENT;
    if (mode & FS_OPEN_WRITE) plat_file_fsync(f);
    return 0;
}

static void close_file(uint64_t h) {
    if (romfs_handle(h)) {
        romfs_close(h);
        return;
    }
    if (!file_of(h, NULL, NULL)) rt_fatal("fs: close of invalid file handle %#llx", (unsigned long long)h);
    plat_mutex_lock(&lock);
    PlatFile *f = files[h - FILE_BASE].h;
    int save = files[h - FILE_BASE].save;
    files[h - FILE_BASE] = (File){0};
    plat_mutex_unlock(&lock);
    plat_file_close(f);
    if (save) {
        save_touched();
        plat_dec32(&save_writers);
    }
}

static uint32_t create_file(const char *path, int64_t size) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (size < 0) return FS_R_INVALID_ARGUMENT;
    int err = 0;
    PlatFile *f = plat_fs_create(p, &err);
    if (!f) return fs_result(err);
    err = size ? plat_file_set_size(f, (uint64_t)size) : 0;
    plat_file_close(f);
    return fs_result(err);
}

static uint32_t delete_file(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (plat_fs_kind(p) == PLAT_FS_DIR) return FS_R_INVALID_ARGUMENT;   // DeleteFile on a directory
    return fs_result(plat_fs_unlink(p));
}

static uint32_t rename_file(const char *from, const char *to) {
    char a[PATH_CAP], b[PATH_CAP];
    uint32_t r = save_change_path(from, a, sizeof a);
    if (!r) r = host_path(to, b, sizeof b);
    if (r) return r;
    size_t n = strcspn(from, ":");
    if (strcspn(to, ":") != n || strncmp(from, to, n)) return FS_R_INVALID_PATH;   // across mounts
    if (exists(b)) return FS_R_ALREADY_EXISTS;
    return fs_result(plat_fs_rename(a, b, 0));
}

static uint32_t entry_type(int *out, const char *path) {
    if (romfs_owns(path)) return romfs_entry_type(out, path);
    char p[PATH_CAP];
    uint32_t r = host_path(path, p, sizeof p);
    if (r) return r;
    if (!out) return FS_R_INVALID_ARGUMENT;
    int kind = plat_fs_kind(p);
    if (kind == PLAT_FS_MISSING) return FS_R_PATH_NOT_FOUND;
    *out = kind == PLAT_FS_DIR ? FS_ENTRY_DIRECTORY : FS_ENTRY_FILE;
    return 0;
}

// ---- directories
typedef struct { char name[769]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");

static uint32_t create_directory(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    return fs_result(plat_fs_mkdir(p));
}

static uint32_t delete_recursive(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_PATH_NOT_FOUND;
    return remove_tree(p) ? FS_R_INVALID_ARGUMENT : 0;
}

static int romfs_listing_add(const char *name, int is_dir, uint64_t size, void *ctx) {
    return listing_add(name, is_dir ? PLAT_FS_DIR : PLAT_FS_FILE, size, ctx);
}

static uint32_t open_directory(uint64_t *out, const char *path, int mode) {
    Listing l = {0};
    if (romfs_owns(path)) {   // Switch 2 titles list RomFS folders (content:/Foliage/...); NX2NAVI never did
        if (!out || mode < 1 || mode > 3) return FS_R_INVALID_ARGUMENT;
        uint32_t r = romfs_list(path, romfs_listing_add, &l);
        if (r) {
            listing_free(l.e, l.n);
            return r;
        }
    } else {
        char p[PATH_CAP];
        uint32_t r = host_path(path, p, sizeof p);
        if (r) return r;
        if (!out || mode < 1 || mode > 3) return FS_R_INVALID_ARGUMENT;
        if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_PATH_NOT_FOUND;
        int e = list_dir(p, &l);
        if (e) return fs_result(e);
    }
    plat_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++)
        if (!dirs[i].used) {
            dirs[i] = (Dir){l.e, l.n, 0, mode, 1};
            *out = DIR_BASE + (uint64_t)i;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("fs: host directory handle table full (%d)", NDIRS);
}

static uint32_t read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return FS_R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE];
    if (!d->used) return FS_R_INVALID_ARGUMENT;
    int64_t n = 0;
    while (n < max && d->next < d->n) {
        const Listed *e = &d->e[d->next++];
        size_t k = strlen(e->name) + 1;
        if (k > 769) continue;   // over 768 bytes: nn::fs cannot name it
        int is_dir = e->kind == PLAT_FS_DIR;
        if (!(d->mode & (is_dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out);
        memcpy(out[n].name, e->name, k);
        out[n].type = is_dir ? FS_ENTRY_DIRECTORY : FS_ENTRY_FILE;
        out[n].size = is_dir ? 0 : (int64_t)e->size;
        n++;
    }
    *count = n;
    return 0;
}

// Result GetDirectoryEntryCount(int64_t *out, DirectoryHandle h): the entries ReadDirectory would still return
static uint32_t directory_entry_count(int64_t *count, uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count) return FS_R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE];
    if (!d->used) return FS_R_INVALID_ARGUMENT;
    int64_t n = 0;
    for (int i = d->next; i < d->n; i++)
        if (strlen(d->e[i].name) < 769 && (d->mode & (d->e[i].kind == PLAT_FS_DIR ? 1 : 2))) n++;
    *count = n;
    return 0;
}

static void close_directory(uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].used)
        rt_fatal("fs: close of invalid directory handle %#llx", (unsigned long long)h);
    plat_mutex_lock(&lock);
    Dir d = dirs[h - DIR_BASE];
    dirs[h - DIR_BASE] = (Dir){0};
    plat_mutex_unlock(&lock);
    listing_free(d.e, d.n);
}

// ---- overrides: AAPCS64 thunks. FileHandle, DirectoryHandle, CacheStorageListHandle
// and ApplicationId are 8-byte structs passed in one register; Uid, ReadOption and WriteOption by reference.
#define I32(i) ((int32_t)arg_x(c, i))
#define I64(i) ((int64_t)arg_x(c, i))
static void ov_open_file(Cpu *c) { ret_x(c, open_file(arg_p(c, 0), arg_p(c, 1), I32(2))); }
static void ov_file_size(Cpu *c) { ret_x(c, file_size(arg_p(c, 0), arg_x(c, 1))); }
static void ov_close_file(Cpu *c) { close_file(arg_x(c, 0)); }
static void ov_entry_type(Cpu *c) { ret_x(c, entry_type(arg_p(c, 0), arg_p(c, 1))); }
static void ov_read_file(Cpu *c) { ret_x(c, read_file(arg_p(c, 0), arg_x(c, 1), I64(2), arg_p(c, 3), arg_x(c, 4))); }
static void ov_read_options(Cpu *c) { ret_x(c, read_file(arg_p(c, 0), arg_x(c, 1), I64(2), arg_p(c, 3), arg_x(c, 4))); }
static void ov_read_exact(Cpu *c) { ret_x(c, read_exact(arg_x(c, 0), I64(1), arg_p(c, 2), arg_x(c, 3))); }
static void ov_write_file(Cpu *c) { ret_x(c, write_file(arg_x(c, 0), I64(1), arg_p(c, 2), arg_x(c, 3), arg_p(c, 4))); }
static void ov_set_file_size(Cpu *c) { ret_x(c, set_file_size(arg_x(c, 0), I64(1))); }
static void ov_flush_file(Cpu *c) { ret_x(c, flush_file(arg_x(c, 0))); }
static void ov_create_file(Cpu *c) { ret_x(c, create_file(arg_p(c, 0), I64(1))); }
static void ov_delete_file(Cpu *c) { ret_x(c, delete_file(arg_p(c, 0))); }
static void ov_rename_file(Cpu *c) { ret_x(c, rename_file(arg_p(c, 0), arg_p(c, 1))); }
static void ov_create_directory(Cpu *c) { ret_x(c, create_directory(arg_p(c, 0))); }
static void ov_delete_recursive(Cpu *c) { ret_x(c, delete_recursive(arg_p(c, 0))); }
static void ov_open_directory(Cpu *c) { ret_x(c, open_directory(arg_p(c, 0), arg_p(c, 1), I32(2))); }
static void ov_read_directory(Cpu *c) { ret_x(c, read_directory(arg_p(c, 0), arg_p(c, 1), arg_x(c, 2), I64(3))); }
static void ov_close_directory(Cpu *c) { close_directory(arg_x(c, 0)); }
static void ov_directory_entry_count(Cpu *c) { ret_x(c, directory_entry_count(arg_p(c, 0), arg_x(c, 1))); }
static void ov_mount_save(Cpu *c) { ret_x(c, mount_save(arg_p(c, 0), arg_p(c, 1))); }
static void ov_mount_save_ro(Cpu *c) { ret_x(c, mount_save_ro(arg_p(c, 0), arg_x(c, 1), arg_p(c, 2))); }
static void ov_save_exists(Cpu *c) { ret_x(c, save_exists(arg_x(c, 0), arg_p(c, 1))); }
static void ov_mount_temp(Cpu *c) { ret_x(c, mount_temp(arg_p(c, 0))); }
static void ov_unmount(Cpu *c) { unmount(arg_p(c, 0)); }
static void ov_commit(Cpu *c) { ret_x(c, commit(arg_p(c, 0))); }
static void ov_create_cache(Cpu *c) { ret_x(c, create_cache(I32(0), I64(1), I64(2))); }
static void ov_delete_cache(Cpu *c) { ret_x(c, delete_cache(I32(0))); }
static void ov_mount_cache(Cpu *c) { ret_x(c, mount_cache(arg_p(c, 0), I32(1))); }
static void ov_open_cache_list(Cpu *c) { ret_x(c, open_cache_list(arg_p(c, 0))); }
static void ov_read_cache_list(Cpu *c) { ret_x(c, read_cache_list(arg_p(c, 0), arg_p(c, 1), arg_x(c, 2), I32(3))); }
static void ov_close_cache_list(Cpu *c) { close_cache_list(arg_x(c, 0)); }
// The fs priority only orders I/O inside the system fs process; host I/O is unordered.
static void ov_set_priority(Cpu *c) { (void)c; }
#undef I32
#undef I64

#define S_OPEN_FILE "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
#define S_FILE_SIZE "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"
#define S_CLOSE_FILE "_ZN2nn2fs9CloseFileENS0_10FileHandleE"
#define S_ENTRY_TYPE "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"
#define S_READ_FILE "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm"
#define S_READ_OPTIONS "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE"
#define S_READ_EXACT "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm"
#define S_WRITE_FILE "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"
#define S_SET_FILE_SIZE "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl"
#define S_FLUSH_FILE "_ZN2nn2fs9FlushFileENS0_10FileHandleE"
#define S_CREATE_FILE "_ZN2nn2fs10CreateFileEPKcl"
#define S_DELETE_FILE "_ZN2nn2fs10DeleteFileEPKc"
#define S_RENAME_FILE "_ZN2nn2fs10RenameFileEPKcS2_"
#define S_CREATE_DIRECTORY "_ZN2nn2fs15CreateDirectoryEPKc"
#define S_DELETE_RECURSIVE "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc"
#define S_OPEN_DIRECTORY "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci"
#define S_READ_DIRECTORY "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"
#define S_CLOSE_DIRECTORY "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE"
#define S_DIRECTORY_ENTRY_COUNT "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE"
#define S_MOUNT_SAVE "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE"
#define S_MOUNT_SAVE_RO "_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE"
#define S_SAVE_EXISTS "_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE"
#define S_MOUNT_TEMP "_ZN2nn2fs21MountTemporaryStorageEPKc"
#define S_UNMOUNT "_ZN2nn2fs7UnmountEPKc"
#define S_COMMIT "_ZN2nn2fs6CommitEPKc"
#define S_CREATE_CACHE "_ZN2nn2fs18CreateCacheStorageEill"
#define S_DELETE_CACHE "_ZN2nn2fs18DeleteCacheStorageEi"
#define S_MOUNT_CACHE "_ZN2nn2fs17MountCacheStorageEPKci"
#define S_OPEN_CACHE_LIST "_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE"
#define S_READ_CACHE_LIST "_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi"
#define S_CLOSE_CACHE_LIST "_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE"
#define S_SET_PRIORITY "_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE"
// romfs.c
#define S_MOUNT_ROM "_ZN2nn2fs8MountRomEPKcPvm"
#define S_QUERY_CACHE "_ZN2nn2fs22QueryMountRomCacheSizeEPm"
#define S_INITIALIZE "_ZN2nn2fs24InitializeForApplicationEv"

HOST_OVERRIDE(S_OPEN_FILE, ov_open_file);
HOST_OVERRIDE(S_FILE_SIZE, ov_file_size);
HOST_OVERRIDE(S_CLOSE_FILE, ov_close_file);
HOST_OVERRIDE(S_ENTRY_TYPE, ov_entry_type);
HOST_OVERRIDE(S_READ_FILE, ov_read_file);
HOST_OVERRIDE(S_READ_OPTIONS, ov_read_options);
HOST_OVERRIDE(S_READ_EXACT, ov_read_exact);
HOST_OVERRIDE(S_WRITE_FILE, ov_write_file);
HOST_OVERRIDE(S_SET_FILE_SIZE, ov_set_file_size);
HOST_OVERRIDE(S_FLUSH_FILE, ov_flush_file);
HOST_OVERRIDE(S_CREATE_FILE, ov_create_file);
HOST_OVERRIDE(S_DELETE_FILE, ov_delete_file);
HOST_OVERRIDE(S_RENAME_FILE, ov_rename_file);
HOST_OVERRIDE(S_CREATE_DIRECTORY, ov_create_directory);
HOST_OVERRIDE(S_DELETE_RECURSIVE, ov_delete_recursive);
HOST_OVERRIDE(S_OPEN_DIRECTORY, ov_open_directory);
HOST_OVERRIDE(S_READ_DIRECTORY, ov_read_directory);
HOST_OVERRIDE(S_CLOSE_DIRECTORY, ov_close_directory);
HOST_OVERRIDE(S_DIRECTORY_ENTRY_COUNT, ov_directory_entry_count);
HOST_OVERRIDE(S_MOUNT_SAVE, ov_mount_save);
HOST_OVERRIDE(S_MOUNT_SAVE_RO, ov_mount_save_ro);
HOST_OVERRIDE(S_SAVE_EXISTS, ov_save_exists);
HOST_OVERRIDE(S_MOUNT_TEMP, ov_mount_temp);
HOST_OVERRIDE(S_UNMOUNT, ov_unmount);
HOST_OVERRIDE(S_COMMIT, ov_commit);
HOST_OVERRIDE(S_CREATE_CACHE, ov_create_cache);
HOST_OVERRIDE(S_DELETE_CACHE, ov_delete_cache);
HOST_OVERRIDE(S_MOUNT_CACHE, ov_mount_cache);
HOST_OVERRIDE(S_OPEN_CACHE_LIST, ov_open_cache_list);
HOST_OVERRIDE(S_READ_CACHE_LIST, ov_read_cache_list);
HOST_OVERRIDE(S_CLOSE_CACHE_LIST, ov_close_cache_list);
HOST_OVERRIDE(S_SET_PRIORITY, ov_set_priority);

// ---- --probe-fs
// Every check calls the overrides the way guest code does: by synthetic address through rt_call, with AAPCS64
// arguments in a Cpu, on this (probe) thread. Prints counts only, never RomFS paths.
static const char *const probe_symbols[] = {
    S_OPEN_FILE, S_FILE_SIZE, S_CLOSE_FILE, S_ENTRY_TYPE, S_READ_FILE, S_READ_OPTIONS, S_READ_EXACT, S_WRITE_FILE,
    S_SET_FILE_SIZE, S_FLUSH_FILE, S_CREATE_FILE, S_DELETE_FILE, S_RENAME_FILE, S_CREATE_DIRECTORY, S_DELETE_RECURSIVE,
    S_OPEN_DIRECTORY, S_READ_DIRECTORY, S_CLOSE_DIRECTORY, S_MOUNT_SAVE, S_MOUNT_SAVE_RO, S_SAVE_EXISTS, S_MOUNT_TEMP,
    S_UNMOUNT, S_COMMIT, S_CREATE_CACHE, S_DELETE_CACHE, S_MOUNT_CACHE, S_OPEN_CACHE_LIST, S_READ_CACHE_LIST,
    S_CLOSE_CACHE_LIST, S_SET_PRIORITY, S_MOUNT_ROM, S_QUERY_CACHE, S_INITIALIZE};
#define NPROBE ((int)(sizeof probe_symbols / sizeof *probe_symbols))
#define SAMPLES 64
#define HEAD_BYTES (8u << 20)      // a sampled file is compared whole up to this size, else head and tail
#define TAIL_BYTES (64u << 10)

static uint64_t probe_addr[NPROBE];
static Cpu *pcpu;
static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static uint64_t U(const void *p) { return (uint64_t)(uintptr_t)p; }

// The override registered for sym, called through its synthetic address with up to 6 arguments. Returns x0.
static uint64_t fs_call(const char *sym, const uint64_t a[6]) {
    for (int i = 0; i < NPROBE; i++)
        if (!strcmp(probe_symbols[i], sym)) {
            for (int r = 0; r < 19; r++) pcpu->x[r] = 0xdead0000 + r;   // caller-saved garbage
            return rt_call(pcpu, probe_addr[i], 6, a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    rt_fatal("probe-fs: %s is not a probed symbol", sym);
}
#define CALL(sym, ...) fs_call(sym, (const uint64_t[6]){__VA_ARGS__})
#define RES(sym, ...) ((uint32_t)CALL(sym, __VA_ARGS__))

static int64_t independent_read(PlatFile *f, uint8_t *buf, uint64_t n, uint64_t off) {
    uint64_t done = 0;
    while (done < n) {
        int64_t r = host_read_at(f, buf + done, (uint32_t)(n - done), off + done);
        if (r <= 0) return -1;
        done += (uint64_t)r;
    }
    return (int64_t)done;
}

// Read [pos, pos+n) of the open RomFS handle h through one of the three ReadFile overloads, and compare it with an
// independent read of romfs.bin at the index offset.
static int compare_range(PlatFile *image, uint64_t h, uint64_t offset, uint64_t pos, uint64_t n, int variant,
                         uint8_t *a, uint8_t *b) {
    uint64_t got = ~0ull;
    uint32_t r;
    int32_t option = 0;
    if (variant == 0) r = RES(S_READ_FILE, U(&got), h, pos, U(a), n);
    else if (variant == 1) r = RES(S_READ_OPTIONS, U(&got), h, pos, U(a), n, U(&option));
    else r = RES(S_READ_EXACT, h, pos, U(a), n), got = r ? ~0ull : n;
    if (r || got != n) return 0;
    if (independent_read(image, b, n, offset + pos) != (int64_t)n) return 0;
    return !memcmp(a, b, n);
}

static void probe_paths(void) {
    static const struct { const char *path, *want; } cases[] = {
        {"rom:/a/b.bin", "a/b.bin"}, {"rom://a/b.bin", "a/b.bin"}, {"rom:///a/b.bin", "a/b.bin"},
        {"rom:/a", "a"},             {"rom:/../main", NULL},       {"rom://../a", NULL},
        {"rom:/a/../b", NULL},       {"rom:/a/./b", NULL},         {"rom:/a//b", "a/b"},
        {"rom:/a\\b", NULL},         {"rom:/", NULL},              {"rom://", NULL},
        {"rom:a", NULL},             {"ro:/a", NULL},              {"romx:/a", NULL},
        {"rom:/.", NULL},            {"rom:/..", NULL},            {"ROM:/a", NULL},
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char folded[256];
        const char *in = romfs_path_collapse(cases[i].path, folded, sizeof folded) ? folded : cases[i].path;
        const char *got = romfs_path_relative("rom", in);
        if (cases[i].want ? !got || strcmp(got, cases[i].want) : got != NULL) {
            bad++;
            printf("  FAIL: romfs_path_relative(\"rom\", \"%s\") = %s%s%s\n", cases[i].path, got ? "\"" : "",
                   got ? got : "NULL", got ? "\"" : "");
        }
    }
    checks++;
    failures += bad > 0;
    CHECK(!romfs_path_relative(NULL, "rom:/a") && !romfs_path_relative("", ":/a") && !romfs_path_relative("rom", NULL),
          "romfs_path_relative accepts an empty mount or path");
    printf("paths: %u romfs_path.h normalisation cases: %s\n", (unsigned)(sizeof cases / sizeof *cases),
           bad ? "FAIL" : "ok");
}

static uint64_t probe_romfs(void) {
    uint64_t count = romfs_file_count(), cache = ~0ull;
    CHECK(RES(S_QUERY_CACHE, U(&cache)) == 0 && cache == 0, "QueryMountRomCacheSize");
    CHECK(RES(S_MOUNT_ROM, U("rom"), 0, 0) == 0, "MountRom(\"rom\")");
    CHECK(RES(S_MOUNT_ROM, U("rom"), 0, 0) == 0, "MountRom(\"rom\") again");
    CHECK(RES(S_MOUNT_ROM, U("../x"), 0, 0) == FS_R_INVALID, "MountRom with a bad name");
    probe_paths();

    PlatFile *image = plat_file_open(romfs_image_path());
    if (!image) rt_fatal("probe-fs: cannot open the RomFS image");
    uint8_t *a = malloc(HEAD_BYTES), *b = malloc(HEAD_BYTES);
    need(a && b, "probe buffers");
    uint64_t step = count / SAMPLES, bytes = 0, sampled = 0, deep = 0, high = 0, largest = 0, lsize = 0;
    char path[4200], sample[4200] = "";
    for (uint64_t i = 0, size; i < count; i++)
        if (romfs_entry(i, NULL, &size) && size > lsize) largest = i, lsize = size;
    for (uint64_t k = 0; k <= SAMPLES && step; k++) {   // k == SAMPLES: the largest file, beyond the 64
        uint64_t i = k < SAMPLES ? k * step : largest, off, size, h = 0;
        int64_t fsize = -1;
        int type = -1;
        const char *name = romfs_entry(i, &off, &size);
        snprintf(path, sizeof path, "rom:/%s", name);
        if (!*sample && strchr(name, '/') && size >= 256) snprintf(sample, sizeof sample, "%s", name);
        int ok = RES(S_ENTRY_TYPE, U(&type), U(path)) == 0 && type == FS_ENTRY_FILE;
        ok = ok && RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ) == 0 && romfs_handle(h);
        ok = ok && RES(S_FILE_SIZE, U(&fsize), h) == 0 && (uint64_t)fsize == size;
        uint64_t head = size < HEAD_BYTES ? size : HEAD_BYTES;
        ok = ok && compare_range(image, h, off, 0, head, (int)(k % 3), a, b);
        bytes += head;
        if (size > HEAD_BYTES) {
            uint64_t tail = size - TAIL_BYTES;
            ok = ok && compare_range(image, h, off, tail, TAIL_BYTES, (int)((k + 1) % 3), a, b);
            bytes += TAIL_BYTES;
            deep += k < SAMPLES;
        }
        high += k < SAMPLES && off >= (1ull << 32);
        uint64_t got = ~0ull;
        ok = ok && RES(S_READ_FILE, U(&got), h, size, U(a), 1) == 0 && got == 0;          // at EOF: 0 bytes
        ok = ok && RES(S_READ_FILE, U(&got), h, size + 1, U(a), 1) == FS_R_INVALID;      // past EOF
        if (h) CALL(S_CLOSE_FILE, h);
        CHECK(ok, "sampled RomFS file %llu (index row %llu)", (unsigned long long)k, (unsigned long long)i);
        sampled += ok && k < SAMPLES;
    }
    printf("romfs: %llu of %d sampled files equal romfs.bin at their index offset (%llu at offsets past 4 GiB, %llu "
           "over 8 MiB compared by head and tail, %.1f MiB compared), EOF reads ok\n", (unsigned long long)sampled,
           SAMPLES, (unsigned long long)high, (unsigned long long)deep, bytes / 1048576.0);
    printf("romfs: largest file (%.1f MiB): head and tail equal romfs.bin\n", lsize / 1048576.0);

    // The same file under "rom:/", "rom://" and "rom:///"; its folder is a directory; traversal and writes refused.
    uint64_t h = 0, off, size, got = 0;
    int type = -1;
    need(*sample, "no sampled file in a folder");
    uint64_t si = 0;
    while (strcmp(romfs_entry(si, &off, &size), sample)) si++;
    int same = 1;
    for (int slashes = 1; slashes <= 3; slashes++) {
        snprintf(path, sizeof path, "rom:%.*s%s", slashes, "///", sample);
        same &= RES(S_ENTRY_TYPE, U(&type), U(path)) == 0 && type == FS_ENTRY_FILE;
        if (RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ)) {
            same = 0;
            continue;
        }
        same &= RES(S_READ_FILE, U(&got), h, 0, U(a), 256) == 0 && got == 256;
        same &= independent_read(image, b, 256, off) == 256 && !memcmp(a, b, 256);
        CALL(S_CLOSE_FILE, h);
    }
    CHECK(same, "rom:/, rom:// and rom:/// do not open the same file");
    snprintf(path, sizeof path, "rom:/%.*s", (int)(strrchr(sample, '/') - sample), sample);
    CHECK(RES(S_ENTRY_TYPE, U(&type), U(path)) == 0 && type == FS_ENTRY_DIRECTORY, "a sampled file's folder");
    snprintf(path, sizeof path, "rom://../%s", sample);
    CHECK(RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ) == FS_R_PATH_NOT_FOUND, "rom://../<file> opens");
    snprintf(path, sizeof path, "rom:/%s.absent", sample);
    CHECK(RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ) == FS_R_PATH_NOT_FOUND, "an absent file opens");
    uint32_t traversal = RES(S_OPEN_FILE, U(&h), U("rom:/../main"), FS_OPEN_READ);
    uint32_t write_open = RES(S_OPEN_FILE, U(&h), U("rom:/absent"), FS_OPEN_WRITE);
    snprintf(path, sizeof path, "rom:/%s", sample);
    uint32_t write_existing = RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ | FS_OPEN_WRITE);
    printf("romfs: rom:/, rom://, rom:/// same file: %s; rom:/../main -> %#x; write open -> %#x, %#x\n",
           same ? "ok" : "FAIL", traversal, write_open, write_existing);
    CHECK(traversal == FS_R_PATH_NOT_FOUND, "rom:/../main -> %#x, expected 0x202", traversal);
    CHECK(write_open == FS_R_UNSUPPORTED_OPEN && write_existing == FS_R_UNSUPPORTED_OPEN,
          "write open -> %#x/%#x, expected 0x6402", write_open, write_existing);

    // Overlays (NX2NAVI_ROMFS_PATCHES, NX2NAVI_ROMFS_MODS): served in place of the original, added files and their folders
    // exist, bytes equal the overlay file.
    size_t nov = romfs_overlay_count(), ovok = 0;
    for (size_t k = 0; k < nov; k++) {
        const char *file;
        uint64_t osize;
        int is_added;
        const char *name = romfs_overlay(k, &file, &osize, &is_added);
        snprintf(path, sizeof path, "rom:/%s", name);
        int ok = RES(S_ENTRY_TYPE, U(&type), U(path)) == 0 && type == FS_ENTRY_FILE;
        char *slash = strrchr(path, '/');
        if (is_added && slash > path + 5) {
            *slash = 0;
            ok = ok && RES(S_ENTRY_TYPE, U(&type), U(path)) == 0 && type == FS_ENTRY_DIRECTORY;
            *slash = '/';
        }
        int64_t fsize = -1;
        uint64_t n = osize < HEAD_BYTES ? osize : HEAD_BYTES;
        ok = ok && RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ) == 0;
        if (ok) {
            ok = RES(S_FILE_SIZE, U(&fsize), h) == 0 && (uint64_t)fsize == osize &&
                 RES(S_READ_FILE, U(&got), h, 0, U(a), n) == 0 && got == n;
            CALL(S_CLOSE_FILE, h);
        }
        PlatFile *of = plat_file_open(file);
        ok = ok && of && independent_read(of, b, n, 0) == (int64_t)n && !memcmp(a, b, n);
        if (of) plat_file_close(of);
        CHECK(ok, "overlay %zu (%s)", k, is_added ? "added" : "replacement");
        ovok += ok;
    }
    if (nov) printf("romfs: %zu of %zu overlay files served from their overlay\n", ovok, nov);
    plat_file_close(image);
    free(a);
    free(b);
    return count;
}

// ---- save transactions: restarts, uncommitted and committed changes, a crash at every step of a Commit
// The process dies here: the save mount goes without Unmount, and the next start (storage_files: the save files'
// recovery, the file pick, save_recover) mounts again.
static void probe_restart(const uint64_t *uid) {
    plat_mutex_lock(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (mounts[i].kind == M_SAVE) mounts[i].kind = M_FREE;
    plat_mutex_unlock(&lock);
    save_mounts = tx_crash_at = tx_dead = 0;
    plat_xchg32(&save_dirty, 0);
    storage_files();
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0, "MountSaveData after a restart");
}

// Commit through the override, then wait for its background thread, as the checks read the disk next. Not with
// fs_save_flush: that also marks the process as exiting, and every later Commit of the probe would be refused.
static uint32_t tx_commit(void) {
    uint32_t r = RES(S_COMMIT, U("save"));
    save_wait_commit();
    return r;
}

#define TX_SIZE 4096
static uint8_t tx_buf[TX_SIZE];

// Fill a save file, created when absent, with byte g through the overrides.
static int tx_write(const char *path, int g) {
    uint64_t h = 0;
    int32_t no_flush = 0;
    memset(tx_buf, g, sizeof tx_buf);
    uint32_t r = RES(S_CREATE_FILE, U(path), TX_SIZE);
    if ((r && r != FS_R_ALREADY_EXISTS) || RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_WRITE)) return 0;
    int ok = RES(S_WRITE_FILE, h, 0, U(tx_buf), TX_SIZE, U(&no_flush)) == 0;
    CALL(S_CLOSE_FILE, h);
    return ok;
}

static int tx_byte(int ok) {
    for (int i = 1; ok && i < TX_SIZE; i++) ok = tx_buf[i] == tx_buf[0];
    return ok ? tx_buf[0] : -2;
}

// The byte a save file is filled with, read through the overrides; -1 absent, -2 short or mixed.
static int tx_read(const char *path) {
    uint64_t h = 0, got = 0;
    if (RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ)) return -1;
    int ok = RES(S_READ_FILE, U(&got), h, 0, U(tx_buf), TX_SIZE) == 0 && got == TX_SIZE;
    CALL(S_CLOSE_FILE, h);
    return tx_byte(ok);
}

// The same, read from the committed save on disk.
static int tx_read_committed(const char *name) {
    char *p = join(save_root, name);
    PlatFile *f = plat_file_open(p);
    free(p);
    if (!f) return -1;
    int ok = independent_read(f, tx_buf, TX_SIZE, 0) == TX_SIZE;
    plat_file_close(f);
    return tx_byte(ok);
}

// Hard links of a file of the committed save on disk; 0 when absent.
static int tx_links_committed(const char *name) {
    char *p = join(save_root, name);
    int links = plat_fs_links(p);
    free(p);
    return links;
}

static int count_dirs(const char *dir) {
    Listing l;
    list_dir(dir, &l);
    int n = 0;
    for (int i = 0; i < l.n; i++) n += l.e[i].kind == PLAT_FS_DIR && l.e[i].name[0] != '.';
    listing_free(l.e, l.n);
    return n;
}

static void probe_save_tx(const uint64_t *uid) {
    const char *a = "save:/tx_a.sav", *b = "save:/tx_b.sav";
    uint64_t h = 0;
    CHECK(tx_write(a, 1) && tx_write(b, 1) && tx_commit() == 0 && tx_read_committed("tx_a.sav") == 1,
          "Commit of generation 1");

    // uncommitted changes: the game reads them; the committed save and the next start do not see them
    CHECK(tx_write(a, 2) && tx_write("save:/tx_c.sav", 2), "uncommitted writes");
    CHECK(tx_read(a) == 2 && tx_read("save:/tx_c.sav") == 2, "reads do not see the game's uncommitted writes");
    CHECK(tx_read_committed("tx_a.sav") == 1 && tx_read_committed("tx_c.sav") == -1,
          "an uncommitted write reached the committed save");
    probe_restart(uid);
    CHECK(tx_read(a) == 1 && tx_read("save:/tx_c.sav") == -1, "an uncommitted write survived a restart");

    // committed changes survive a restart; an Unmount without Commit drops changes
    CHECK(tx_write(a, 3) && tx_write(b, 3) && tx_commit() == 0, "Commit of generation 3");
    probe_restart(uid);
    CHECK(tx_read(a) == 3 && tx_read(b) == 3, "a commit lost in a restart");
    CHECK(tx_write(a, 4), "write before Unmount");
    CALL(S_UNMOUNT, U("save"));
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0 && tx_read(a) == 3, "Unmount without Commit kept the change");

    // a crash at step k of a Commit, k = 1, 2, ... until a Commit completes: the next start sees all of the old
    // generation or all of the new one
    int committed = 3, gen = 4, steps = 0, olds = 0, news = 0, mixed = 0;
    for (int k = 1; k < 64; k++) {
        int next = ++gen, ok = tx_write(a, next) && tx_write(b, next);
        tx_crash_at = k;
        uint32_t r = tx_commit();
        int died = tx_dead;
        tx_crash_at = 0;
        if (!died) {
            steps = k - 1;
            CHECK(ok && r == 0 && tx_read_committed("tx_a.sav") == next && tx_read_committed("tx_b.sav") == next,
                  "Commit after the crash points");
            committed = next;
            break;
        }
        probe_restart(uid);
        int va = tx_read(a), vb = tx_read(b);
        if (va == committed && vb == committed) olds++;
        else if (va == next && vb == next) news++, committed = next;
        else mixed++;
        CHECK(ok && (va == vb) && (va == committed), "crash at commit step %d: tx_a %d, tx_b %d (old %d, new %d)", k,
              va, vb, committed, next);
        CHECK(!exists(new_root) && !exists(old_root) && !*ready_root, "crash at commit step %d: staging left after "
              "recovery", k);
    }
    CHECK(steps >= 4 && olds && news && !mixed, "crash points: %d (%d old, %d new, %d mixed)", steps, olds, news,
          mixed);
    int backups = count_dirs(backup_root);
    CHECK(backups == SAVE_BACKUPS, "save-backup holds %d saves", backups);

    // a Commit writes the changed file and links the unchanged one to the committed save it replaces
    int kept = committed;
    CHECK(tx_write(a, ++gen) && tx_commit() == 0 && tx_read_committed("tx_a.sav") == gen &&
          tx_read_committed("tx_b.sav") == kept && tx_links_committed("tx_b.sav") >= 2 &&
          tx_links_committed("tx_a.sav") == 1, "Commit of one changed file: tx_a %d links, tx_b %d links",
          tx_links_committed("tx_a.sav"), tx_links_committed("tx_b.sav"));
    committed = gen;

    // a Commit that changed no byte (the game rewrites its files at start) makes no new save and no backup
    static char backups_before[64][48], backups_after[64][48];
    int nbefore = backup_names(backups_before);
    CHECK(tx_write(a, committed) && tx_commit() == 0 && tx_read_committed("tx_a.sav") == committed &&
          backup_names(backups_after) == nbefore && !memcmp(backups_before, backups_after, sizeof backups_before),
          "a Commit of unchanged bytes made a backup");

    // the background commit: a write right after Commit waits for it and stays out of it; an Unmount right after
    // Commit waits for it and keeps it
    int queued = ++gen;
    CHECK(tx_write(a, queued) && RES(S_COMMIT, U("save")) == 0 && tx_write(a, ++gen) && tx_read(a) == gen &&
          tx_read_committed("tx_a.sav") == queued, "a write right after Commit: committed tx_a %d, expected %d",
          tx_read_committed("tx_a.sav"), queued);
    CHECK(RES(S_COMMIT, U("save")) == 0, "Commit before Unmount");
    CALL(S_UNMOUNT, U("save"));
    CHECK(tx_read_committed("tx_a.sav") == gen && RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0 && tx_read(a) == gen,
          "an Unmount right after Commit lost it");
    committed = gen;

    // a file of the committed save held open by another program (a virus scanner, Explorer's preview, a sync tool):
    // Windows refuses to rename a directory with an open file under it, so the committed save cannot be swapped out.
    // The game and a restart still see every Commit, and the first start after the file is closed swaps them in.
    char *held_path = join(save_root, "tx_b.sav");
    PlatFile *held = plat_fs_open(held_path, 0, NULL);
    free(held_path);
    int pending = ++gen;
    CHECK(held && tx_write(a, pending) && tx_commit() == 0, "Commit with a committed file held open");
    probe_restart(uid);
    CHECK(tx_read(a) == pending, "a Commit made while a committed file was held open, after a restart: tx_a %d, "
          "expected %d", tx_read(a), pending);
    pending = ++gen;
    CHECK(tx_write(a, pending) && tx_commit() == 0 && tx_read(a) == pending, "a second Commit while held open");
    if (held) plat_file_close(held);
    probe_restart(uid);
    CHECK(tx_read(a) == pending && tx_read_committed("tx_a.sav") == pending && tx_read_committed("tx_b.sav") == kept,
          "the held-open Commits after the file was closed and a restart: tx_a %d, on disk %d, expected %d",
          tx_read(a), tx_read_committed("tx_a.sav"), pending);
    committed = pending;

    // the read-only mount shows the committed save and refuses changes
    CHECK(tx_write(a, 99) && RES(S_MOUNT_SAVE_RO, U("ro"), NX2NAVI_APPLICATION_ID, U(uid)) == 0 &&
          tx_read("ro:/tx_a.sav") == committed && RES(S_OPEN_FILE, U(&h), U("ro:/tx_a.sav"), FS_OPEN_WRITE) ==
          FS_R_UNSUPPORTED_OPEN && RES(S_CREATE_FILE, U("ro:/x"), 0) == FS_R_UNSUPPORTED_OPEN &&
          RES(S_RENAME_FILE, U("save:/tx_a.sav"), U("ro:/y")) == FS_R_INVALID_PATH, "MountSaveDataReadOnly");
    CALL(S_UNMOUNT, U("ro"));
    CHECK(RES(S_DELETE_FILE, U(a)) == 0 && RES(S_DELETE_FILE, U(b)) == 0 && tx_commit() == 0 &&
          tx_read_committed("tx_a.sav") == -1, "Commit of deletions");
    // a run that left read-only folders and files in the working copy (attributes another emulator's save carried):
    // the next start deletes them and names the failing path if it cannot
    char *ro_dir = join(work_root, "ro_dir"), *ro_file = join(ro_dir, "ro.sav");
    CHECK(!plat_fs_mkdir(ro_dir) && plat_file_write(ro_file, "x", 1) && plat_file_set_readonly(ro_file, 1) &&
          plat_file_set_readonly(ro_dir, 1), "read-only leftovers in the working copy");
    probe_restart(uid);
    CHECK(!exists(ro_dir), "the next start deleted a working copy with read-only folders");
    free(ro_dir);
    free(ro_file);
    printf("save tx: uncommitted writes visible to the game and dropped by a restart and by Unmount, commits kept; a "
           "crash at each of %d commit steps recovers to the old save %d times, the new one %d times, mixed %d "
           "times; %d backups\n", steps, olds, news, mixed, backups);
}

// Puts a deleted file back where it was, for the next crash point of a Delete.
static void probe_undelete(int id, const char *del) {
    char root[PATH_CAP];
    if (id >= 2) {
        file_root(id, root, sizeof root);
        CHECK(!move_dir(del, root, 1), "file %d back from %s", id, del);
        return;
    }
    for (int i = 0; i < 4; i++) {
        char from[PATH_CAP + 128], to[PATH_CAP + 64], sub[PATH_CAP + 128];
        one_entry(del, i, from, sizeof from, sub, sizeof sub);
        one_entry(profile, i, to, sizeof to, sub, sizeof sub);
        CHECK(!exists(from) || !move_dir(from, to, 1), "file 1's %s back", from);
    }
    remove_tree(del);   // an empty save folder is left
}

// ---- save files: New, Copy, Rename, the choice of the file, Delete and an import, each with a crash at every step
// Whether trees a and b hold the same names, kinds and bytes.
static int tree_equal(const char *a, const char *b) {
    Listing la, lb;
    int ea = list_dir(a, &la), eb = list_dir(b, &lb);
    int same = !ea && !eb && la.n == lb.n;
    if (same) {
        qsort(la.e, (size_t)la.n, sizeof *la.e, listed_cmp);
        qsort(lb.e, (size_t)lb.n, sizeof *lb.e, listed_cmp);
    }
    for (int i = 0; same && i < la.n; i++) {
        same = !strcmp(la.e[i].name, lb.e[i].name) && la.e[i].kind == lb.e[i].kind;
        if (!same) break;
        char *pa = join(a, la.e[i].name), *pb = join(b, lb.e[i].name);
        same = la.e[i].kind == PLAT_FS_DIR ? tree_equal(pa, pb) : same_file(pa, pb);
        free(pa);
        free(pb);
    }
    if (!ea) listing_free(la.e, la.n);
    if (!eb) listing_free(lb.e, lb.n);
    return same;
}

// A copy of dir as it is now, in <profile>/probe-ref/<tag> (out), to compare with later.
static void probe_snap(const char *dir, const char *tag, char *out, size_t cap) {
    int n = 0, linked = 0;
    uint64_t bytes = 0;
    snprintf(out, cap, "%s/probe-ref", profile);
    plat_file_mkdir_p(out);
    snprintf(out, cap, "%s/probe-ref/%s", profile, tag);
    remove_tree(out);
    CHECK(!copy_tree(dir, out, NULL, 0, &n, &linked, &bytes), "a reference copy of %s", dir);
}

static int probe_file_count(void) {
    static int ids[FILES_MAX];
    return file_ids(ids, FILES_MAX);
}

// The deleted file whose folder name ends in -<id>; 0 when none.
static int probe_deleted(int id, char *out, size_t cap) {
    char del[PATH_CAP], tail[16];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    snprintf(tail, sizeof tail, "-%d", id);
    Listing l;
    list_dir(del, &l);
    int found = 0;
    for (int i = 0; i < l.n && !found; i++) {
        size_t n = strlen(l.e[i].name), k = strlen(tail);
        if (l.e[i].name[0] != '.' && n > k && !strcmp(l.e[i].name + n - k, tail)) {
            snprintf(out, cap, "%s/%s", del, l.e[i].name);
            found = 1;
        }
    }
    listing_free(l.e, l.n);
    return found;
}

static void probe_name(int id, const char *want, const char *what) {
    char name[FS_SAVE_NAME_MAX];
    file_name(id, name);
    CHECK(!strcmp(name, want), "%s: file %d is named \"%s\", expected \"%s\"", what, id, name, want);
}

// The operations the probe cuts short or makes fail, on the files probe_save_files made: file 1 "Hero" (not running),
// file 2 "Second", file 3 (running, a copy of file 1). References of how they were: pref.
enum { OP_NEW, OP_COPY_RUNNING, OP_COPY_OTHER, OP_DELETE_N, OP_DELETE_1, OP_RENAME, OP_CHOOSE, OP_COUNT };
static const char *const probe_ops[] = {"New", "Copy of the running file", "Copy of another file", "Delete of file 2",
                                        "Delete of file 1", "Rename", "the choice of the next file"};
static struct {
    char one[PATH_CAP + 64], bak[PATH_CAP + 64], tx[PATH_CAP + 64], two[PATH_CAP + 64], three[PATH_CAP + 64];
} pref;

static int probe_run(int op, const char *name) {
    switch (op) {
    case OP_NEW: return fs_save_file_new(name, 0);
    case OP_COPY_RUNNING: return fs_save_file_new(name, 3);
    case OP_COPY_OTHER: return fs_save_file_new(name, 1);
    case OP_DELETE_N: return fs_save_file_delete(2);
    case OP_DELETE_1: return fs_save_file_delete(1);
    case OP_RENAME: return fs_save_file_rename(2, name);
    default: return fs_save_file_choose(1);
    }
}

// Where op left the files: 0 as it was, 1 wholly in its new place, -1 neither. before: the number of files before it;
// expect: the number New and Copy give; del: the deleted file found, for probe_restore.
static int probe_where(int op, int before, int expect, const char *name, char *del, size_t cap) {
    char p[PATH_CAP + 128], q[PATH_CAP + 128], got[FS_SAVE_NAME_MAX];
    if (op <= OP_COPY_OTHER) {
        int now = probe_file_count();
        if (now == before && !file_exists(expect)) return 0;
        snprintf(p, sizeof p, "%s/%d/save/%s", files_root, expect, uid_dir);
        file_name(expect, got);
        const char *src = op == OP_COPY_RUNNING ? pref.three : pref.one;
        return now == before + 1 && !strcmp(got, name) &&
                       (op != OP_NEW ? tree_equal(p, src) : plat_fs_kind(p) == PLAT_FS_DIR)
                   ? 1
                   : -1;
    }
    if (op == OP_DELETE_N) {
        snprintf(p, sizeof p, "%s/2", files_root);
        int gone = probe_deleted(2, del, cap);
        if (file_exists(2) && tree_equal(p, pref.two) && !gone) return 0;
        return !file_exists(2) && gone && tree_equal(del, pref.two) ? 1 : -1;
    }
    if (op == OP_RENAME) {
        file_name(2, got);
        return !strcmp(got, "Second") ? 0 : !strcmp(got, name) ? 1 : -1;
    }
    if (op == OP_CHOOSE) return file_current() == 3 ? 0 : file_current() == 1 ? 1 : -1;
    // file 1: its name, committed save, journal and backups all in the profile, or all in one deleted folder
    int gone = probe_deleted(1, del, cap);
    const char *base = gone ? del : profile;
    if (gone == file_exists(1)) return -1;
    snprintf(p, sizeof p, "%s/save/%s", base, uid_dir);
    snprintf(q, sizeof q, "%s/save-backup", base);
    int whole = tree_equal(p, pref.one) && tree_equal(q, pref.bak);
    snprintf(p, sizeof p, "%s/save-tx", base);
    snprintf(q, sizeof q, "%s/%s", base, NAME_FILE);
    uint64_t n = 0;
    char *nm = (char *)plat_file_read(q, &n);
    whole = whole && tree_equal(p, pref.tx) && nm && n == 4 && !memcmp(nm, "Hero", 4);
    free(nm);
    return whole ? gone : -1;
}

// Unfinished folders (.new-, .import-, .part-) in save-files and in deleted.
static int probe_leftovers(void) {
    char del[PATH_CAP];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    const char *dirs[] = {files_root, del};
    int n = 0;
    for (int d = 0; d < 2; d++) {
        Listing l;
        if (list_dir(dirs[d], &l)) continue;
        for (int i = 0; i < l.n; i++) n += l.e[i].name[0] == '.';
        listing_free(l.e, l.n);
    }
    return n;
}

// After op went through: the files as they were before it, for its next step.
static void probe_restore(const uint64_t *uid, int op, const char *del) {
    if (op == OP_DELETE_N || op == OP_DELETE_1) probe_undelete(op == OP_DELETE_N ? 2 : 1, del);
    if (op == OP_RENAME) CHECK(fs_save_file_rename(2, "Second") == 0, "Rename back");
    if (op == OP_CHOOSE) {   // that start mounted file 1: file 3 runs again
        CHECK(fs_save_file_choose(3) == 0, "the choice back");
        probe_restart(uid);
    }
}

// op with, at each of its steps k in turn, mode 0 a crash, 1 a step that fails, 2 a full disk from that step on.
static void probe_inject(const uint64_t *uid, int mode, int op) {
    static const char *const modes[] = {"a crash", "a failing step", "a full disk"};
    char two[PATH_CAP + 16];
    snprintf(two, sizeof two, "%s/2", files_root);
    probe_snap(save_root, "three", pref.three, sizeof pref.three);   // file 3's newest save: ready_root is empty now
    probe_snap(two, "two", pref.two, sizeof pref.two);
    int olds = 0, news = 0, bad = 0, k, before = probe_file_count();
    for (k = 1; k < 400; k++) {
        int expect = next_id();
        char name[32], del[PATH_CAP + 64] = "";
        snprintf(name, sizeof name, "op%d-%d-%d", mode, op, k);
        tx_crash_at = mode == 0 ? k : 0, fail_at = mode == 1 ? k : 0, fail_space_from = mode == 2 ? k : 0;
        tx_step = tx_dead = fail_step = 0;
        int r = probe_run(op, name);
        int died = tx_dead, reached = mode == 0 ? died : fail_step >= k, said = r >= 0;
        tx_crash_at = fail_at = fail_space_from = 0;
        int now = died ? said : probe_where(op, before, expect, name, del, sizeof del), left = died ? 0 : probe_leftovers();
        CHECK(now == said && !left, "%s at step %d of %s: the menu says %s, the files are %s, %d unfinished folders",
              modes[mode], k, probe_ops[op], said ? "done" : "it did not work",
              now == 0 ? "as they were" : now == 1 ? "changed" : "mixed", left);
        probe_restart(uid);
        int after = probe_where(op, before, expect, name, del, sizeof del);
        left = probe_leftovers();
        CHECK(after >= 0 && !left && (died || after == said),
              "%s at step %d of %s, after the next start: the menu said %s, the files are %s, %d unfinished folders",
              modes[mode], k, probe_ops[op], died ? "nothing" : said ? "done" : "it did not work",
              after == 0 ? "as they were" : after == 1 ? "changed" : "mixed", left);
        olds += after == 0, news += after == 1, bad += after < 0 || now != said || left;
        if (op <= OP_COPY_OTHER) before = probe_file_count();
        if (!reached) {
            CHECK(r >= 0 && after == 1, "%s past its last step: %d", probe_ops[op], r);
            if (after == 1) probe_restore(uid, op, del);
            break;
        }
        if (after == 1) probe_restore(uid, op, del);
    }
    // Rename and the choice are one atomic write without a crash point; a full disk cannot stop Delete of file 2 (one
    // rename): those have no step that leaves the files as they were
    CHECK((k > 1 || op >= OP_RENAME) && news && !bad && (mode != 1 || olds),
          "%s at each of %d steps of %s (%d as it was, %d new, %d bad)",
          modes[mode], k - 1, probe_ops[op], olds, news, bad);
    printf("save files: %s at each of %d steps of %s: as it was %d times, in its new place %d times, mixed or not as "
           "the menu said %d times\n", modes[mode], k - 1, probe_ops[op], olds, news, bad);
}

// A file that cannot be moved (another program holds it); a Delete of file 1 whose every move out of .part is refused
// too, at once, for the whole retry time, or with a crash while putting back; and the empty folder of a Delete of file
// 1 that moved nothing: the menu's word and the files agree, at once and after the next start, and nothing is mixed.
static void probe_refusals(const uint64_t *uid) {
    char del[PATH_CAP + 64], p[PATH_CAP + 128], sub[PATH_CAP + 128];
    int bad = 0, runs = 0;
    for (int i = 0; i < 4; i++) {   // each of file 1's entries held in turn
        one_entry(profile, i, p, sizeof p, sub, sizeof sub);
        fail_move = p;
        int r = fs_save_file_delete(1), now = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
        probe_restart(uid);   // still held at the next start
        int after = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
        fail_move = NULL;
        CHECK(r == FS_FILE_FAILED && now == 0 && after == 0 && !probe_leftovers(),
              "Delete of file 1 with %s held: %d, files %d, after the next start %d", p, r, now, after);
        bad += r != FS_FILE_FAILED || now || after, runs++;
    }
    snprintf(p, sizeof p, "%s/2", files_root);
    fail_move = p;
    int r = fs_save_file_delete(2), now = probe_where(OP_DELETE_N, 0, 0, "", del, sizeof del);
    fail_move = NULL;
    CHECK(r == FS_FILE_FAILED && now == 0, "Delete of file 2 held: %d, files %d", r, now);
    bad += r != FS_FILE_FAILED || now, runs++;
    printf("save files: a Delete of a file another program holds (file 1, each of its 4 parts, and file 2): failed "
           "and as it was %d of %d times\n", runs - bad, runs);

    // the step fails, and every move out of .part is refused too: the rename that would finish the delete and every
    // move back. The menu says it failed; file 1 is not listed while some of it waits in .part, and the next start
    // puts it back. Then the same failure with a crash at each step after it: never mixed.
    int steps = 0, back_fail = 0, crashes = 0, as_said = 0, tries = 0;
    for (int k = 1; k < 40; k++) {
        fail_at = k, fail_move = "/deleted/.part-";
        tx_step = tx_dead = fail_step = 0;
        r = fs_save_file_delete(1);
        int reached = fail_step >= k, listed = file_exists(1);
        fail_at = 0, fail_move = NULL;
        now = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
        probe_restart(uid);
        int after = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
        int ok = after == (r < 0 ? 0 : 1) && !probe_leftovers() && (r < 0 ? now == 0 || !listed : now == 1);
        CHECK(ok, "Delete of file 1, step %d failing and every move out of .part refused: the menu says %s, listed %d, "
              "after the next start %s", k, r < 0 ? "it did not work" : "done", listed,
              after == 0 ? "as it was" : after == 1 ? "deleted" : "mixed");
        as_said += ok, tries++;
        back_fail += r < 0 && !listed;
        if (after == 1) probe_undelete(1, del);
        if (!reached) break;
        steps = k;
        for (int j = 1; j < 40; j++) {
            fail_at = k, tx_crash_at = j;
            tx_step = tx_dead = fail_step = 0;
            fs_save_file_delete(1);
            int died = tx_dead;
            fail_at = tx_crash_at = 0;
            probe_restart(uid);
            after = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
            CHECK(after >= 0 && !probe_leftovers(), "Delete of file 1, step %d failing, crash at %d: files %d", k, j, after);
            if (after == 1) probe_undelete(1, del);
            if (!died) break;
            crashes++;
        }
    }
    printf("save files: a Delete of file 1 failing at each of %d steps with every move out of .part refused: as the "
           "menu said %d of %d times (%d times file 1 waited for the next start); with a crash at each later step (%d "
           "crashes): never mixed\n", steps, as_said, tries, back_fail, crashes);

    // one of file 1's parts kept open in .part for the whole retry time, as a sync client may: Windows then refuses
    // the rename of .part and the moves out of it, each tried for 2 s. The menu says it failed, and the next start,
    // with the part let go, puts file 1 back.
    busy_move = "/deleted/.part-";
    uint64_t t0 = plat_now_ns();
    r = fs_save_file_delete(1);
    double held = (plat_now_ns() - t0) / 1e9;
    int listed = file_exists(1);
    busy_move = NULL;
    probe_restart(uid);
    int after = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
    CHECK(r == FS_FILE_FAILED && after == 0 && !probe_leftovers(), "Delete of file 1 with a part held in .part for the "
          "whole retry time: the menu says %s, after the next start %s", r < 0 ? "it did not work" : "done",
          after == 0 ? "as it was" : after == 1 ? "deleted" : "mixed");
    printf("save files: a Delete of file 1 with a part held in .part for the whole retry time (%.1f s): the menu says "
           "%s, file 1 %s listed meanwhile, after the next start %s\n", held, r < 0 ? "it did not work" : "done",
           listed ? "still" : "not", after == 0 ? "as it was" : after == 1 ? "deleted" : "mixed");
    if (after == 1) probe_undelete(1, del);

    // a Delete of file 1 whose first move is refused while another program holds its fresh .part folder, which then
    // stays, empty (Windows does not remove a folder another program holds): the next start leaves file 1 as it was
    one_entry(profile, 0, p, sizeof p, sub, sizeof sub);
    fail_move = p;
    r = fs_save_file_delete(1);
    fail_move = NULL;
    char stamp[24];
    utc_stamp(stamp, sizeof stamp);
    snprintf(p, sizeof p, "%s/deleted/.part-%s-1", files_root, stamp);
    plat_file_mkdir_p(p);
    probe_restart(uid);
    after = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
    CHECK(r == FS_FILE_FAILED && after == 0 && file_exists(1) && !probe_leftovers(), "Delete of file 1 that left an "
          "empty .part: the menu says %s, after the next start %s", r < 0 ? "it did not work" : "done",
          after == 0 ? "as it was" : after == 1 ? "deleted" : "mixed");
    printf("save files: a Delete of file 1 that moved nothing and left its .part folder: the menu says %s, after the "
           "next start file 1 is %s\n", r < 0 ? "it did not work" : "done",
           after == 0 ? "as it was" : after == 1 ? "deleted" : "mixed");
    if (after == 1) probe_undelete(1, del);

    // a crash cut a Delete of file 1 short after its name and save moved into .part, and an older build's start then
    // made and saved a new file 1: nothing goes back over it, so no file holds parts of two saves; .part waits and file
    // 1 is not listed. Once the new save is gone (the player's choice), the next start puts file 1 back.
    char one[PATH_CAP + 64], moved[PATH_CAP + 128], f[PATH_CAP + 96];
    utc_stamp(stamp, sizeof stamp);
    snprintf(p, sizeof p, "%s/deleted/.part-%s-1", files_root, stamp);
    for (int i = 0; i < 2; i++) {   // the name, then the committed save, as Delete moves them
        one_entry(profile, i, one, sizeof one, sub, sizeof sub);
        one_entry(p, i, moved, sizeof moved, sub, sizeof sub);
        plat_file_mkdir_p(sub);
        CHECK(!move_dir(one, moved, 1), "a Delete of file 1 cut short: %s moved", one);
    }
    one_entry(profile, 1, one, sizeof one, sub, sizeof sub);
    snprintf(f, sizeof f, "%s/slot_00", one);
    plat_file_mkdir_p(f);
    snprintf(f, sizeof f, "%s/slot_00/progress.sav", one);
    CHECK(plat_file_write(f, "older build", 11), "an older build's new save of file 1");
    probe_restart(uid);
    uint64_t n = 0;
    char *b = (char *)plat_file_read(f, &n);
    int newer = b && n == 11 && !memcmp(b, "older build", 11);
    free(b);
    one_entry(p, 0, moved, sizeof moved, sub, sizeof sub);
    int waits = exists(moved);
    one_entry(p, 1, moved, sizeof moved, sub, sizeof sub);
    waits = waits && exists(moved);
    one_entry(profile, 0, moved, sizeof moved, sub, sizeof sub);
    CHECK(newer && waits && !exists(moved) && !file_exists(1), "a Delete of file 1 cut short, then an older build's "
          "new file 1: its save %s, .part %s, the old name %s", newer ? "kept" : "changed", waits ? "waits" : "emptied",
          exists(moved) ? "put back beside it" : "left in .part");
    printf("save files: a Delete of file 1 cut short, then an older build's new file 1: %s; file 1 %slisted\n",
           newer && waits && !exists(moved) ? "nothing put back over it" : "mixed", file_exists(1) ? "" : "not ");
    remove_tree(one);
    probe_restart(uid);
    after = probe_where(OP_DELETE_1, 0, 0, "", del, sizeof del);
    CHECK(after == 0 && !probe_leftovers(), "file 1 put back once the older build's save went: %s",
          after == 0 ? "as it was" : after == 1 ? "deleted" : "mixed");
}

// The game's side while the menu copies the running file: commits of tx_a.sav and tx_b.sav filled with one byte each,
// a new generation each time, until told to stop.
static volatile int32_t committer_stop, committer_commits;
static void probe_committer(void *arg) {
    (void)arg;
    for (int g = 101; !plat_cas32(&committer_stop, 0, 0); g = g == 250 ? 101 : g + 1) {
        if (tx_write("save:/tx_a.sav", g) && tx_write("save:/tx_b.sav", g) && tx_commit() == 0)
            plat_xchg32(&committer_commits, committer_commits + 1);
    }
}

// One byte all through a file of TX_SIZE bytes, else -1.
static int probe_fill(const char *path) {
    uint64_t n = 0;
    uint8_t *b = plat_file_read(path, &n);
    int g = b && n == TX_SIZE ? b[0] : -1;
    for (uint64_t i = 1; g >= 0 && i < n; i++) g = b[i] == b[0] ? g : -1;
    free(b);
    return g;
}

// Copy of the running file while the game commits on another thread: every copy holds one whole commit (tx_a and
// tx_b of one generation), and the commit lock is held for the copy only, not for its flushes.
static void probe_copy_while_saving(void) {
    plat_xchg32(&committer_stop, 0);
    plat_xchg32(&committer_commits, 0);
    PlatThread *t = plat_thread_new(probe_committer, NULL, "probe commits");
    CHECK(t != NULL, "the probe's commit thread");
    for (int i = 0; t && i < 5000 && !plat_cas32(&committer_commits, 0, 0); i++) plat_sleep_ms(1);   // tx_a = tx_b
    int copies = 0, whole = 0;
    uint64_t held = 0;
    for (int i = 0; t && i < 12; i++) {
        int id = fs_save_file_new("while saving", running_file);
        char a[PATH_CAP + 96], b[PATH_CAP + 96];
        snprintf(a, sizeof a, "%s/%d/save/%s/tx_a.sav", files_root, id, uid_dir);
        snprintf(b, sizeof b, "%s/%d/save/%s/tx_b.sav", files_root, id, uid_dir);
        int ga = probe_fill(a), gb = probe_fill(b);
        copies += id > 0, whole += id > 0 && ga >= 0 && ga == gb;
        held = copy_lock_ns > held ? copy_lock_ns : held;
        CHECK(id > 0 && ga >= 0 && ga == gb, "a copy made while the game commits: file %d holds generations %d and %d",
              id, ga, gb);
        if (id > 0) fs_save_file_delete(id);
    }
    plat_xchg32(&committer_stop, 1);
    if (t) plat_thread_join(t);
    printf("save files: %d copies of the running file while the game made %d commits: %d whole; the commit lock held "
           "at most %.1f ms per copy\n", copies, committer_commits, whole, held / 1e6);
}

static void probe_save_files(const uint64_t *uid) {
    char one[PATH_CAP + 64], one_bak[PATH_CAP + 16], one_ref[PATH_CAP + 64], bak_ref[PATH_CAP + 64], p[PATH_CAP + 64];
    cfg_setenv("NX2NAVI_SAVE_FILE", "");   // empty counts as unset, and wins over a save_file in nx2navi.ini

    // a start of a profile that never used save files mounts file 1 and makes nothing new
    probe_restart(uid);
    CHECK(running_file == 1 && !exists(files_root), "a start without save files: file %d, save-files %s", running_file,
          exists(files_root) ? "made" : "absent");
    snprintf(one, sizeof one, "%s/save/%s", profile, uid_dir);
    snprintf(one_bak, sizeof one_bak, "%s/save-backup", profile);
    probe_snap(one, "one", one_ref, sizeof one_ref);
    probe_snap(one_bak, "one-backup", bak_ref, sizeof bak_ref);

    // New (empty), Copy (of the running file), Rename
    int empty = fs_save_file_new("Second", 0), copy = fs_save_file_new("  Copy\tof one \n", 1);
    snprintf(p, sizeof p, "%s/2/save/%s", files_root, uid_dir);
    Listing l;
    int listed = !list_dir(p, &l);
    CHECK(empty == 2 && listed && l.n == 0, "New: file %d, its save %s", empty, listed ? "listed" : "missing");
    if (listed) listing_free(l.e, l.n);
    snprintf(p, sizeof p, "%s/3/save/%s", files_root, uid_dir);
    CHECK(copy == 3 && tree_equal(p, one_ref), "Copy of file 1: file %d", copy);
    probe_name(2, "Second", "New");
    probe_name(3, "Copyof one", "Copy");
    CHECK(fs_save_file_rename(1, "Hero") == 0 && fs_save_file_rename(1, " \t\n") == FS_FILE_FAILED &&
          fs_save_file_rename(99, "x") == FS_FILE_GONE, "Rename");
    probe_name(1, "Hero", "Rename");
    FsSaveFile list[8];
    int n = fs_save_files(list, 8);
    CHECK(n == 3 && list[0].id == 1 && list[0].running && list[0].next && !list[1].running && !list[2].next &&
          !strcmp(list[0].name, "Hero") && !strcmp(list[1].name, "Second"), "the list of files: %d", n);
    CHECK(tree_equal(one, one_ref) && tree_equal(one_bak, bak_ref), "New, Copy or Rename changed file 1's save");

    // the choice of the next file; the start mounts it, and its commits, crashes and backups stay its own
    CHECK(fs_save_file_choose(3) == 0 && file_current() == 3 && fs_save_file_choose(99) == FS_FILE_GONE, "choose");
    probe_restart(uid);
    snprintf(p, sizeof p, "%s/3/save/%s", files_root, uid_dir);
    CHECK(running_file == 3 && !strcmp(save_root, p), "the start after the choice mounts file %d (%s)", running_file,
          save_root);
    printf("save files: file 3, a copy of file 1: ");
    probe_save_tx(uid);
    CHECK(tree_equal(one, one_ref) && tree_equal(one_bak, bak_ref), "file 3's commits changed file 1 or its backups");

    // save_file picks the file for a start, by name in any case or by number, and leaves the choice as it was
    cfg_setenv("NX2NAVI_SAVE_FILE", "hERO");
    probe_restart(uid);
    CHECK(running_file == 1, "save_file = hERO: file %d", running_file);
    cfg_setenv("NX2NAVI_SAVE_FILE", "2");
    probe_restart(uid);
    CHECK(running_file == 2 && file_current() == 3, "save_file = 2: file %d, current %d", running_file, file_current());
    cfg_setenv("NX2NAVI_SAVE_FILE", "nothing like it");
    CHECK(file_next() == 0, "save_file naming no file");
    cfg_setenv("NX2NAVI_SAVE_FILE", "");
    probe_restart(uid);
    CHECK(running_file == 3, "save_file unset: file %d", running_file);

    // refusals: the running file, the next start's, one that is not there
    CHECK(fs_save_file_delete(3) == FS_FILE_IN_USE && fs_save_file_delete(99) == FS_FILE_GONE, "Delete refusals");
    CHECK(fs_save_file_choose(2) == 0 && fs_save_file_delete(2) == FS_FILE_IN_USE && fs_save_file_choose(3) == 0,
          "Delete of the next start's file");

    // a crash, a failing step and a full disk at every step of every operation: what the menu says (done, or did not
    // work) is what the files are, at once and after the next start, and each file is as it was or wholly in its new
    // place
    snprintf(pref.one, sizeof pref.one, "%s", one_ref);
    snprintf(pref.bak, sizeof pref.bak, "%s", bak_ref);
    snprintf(p, sizeof p, "%s/save-tx", profile);
    probe_snap(p, "one-tx", pref.tx, sizeof pref.tx);
    for (int mode = 0; mode < 3; mode++)
        for (int op = 0; op < OP_COUNT; op++) probe_inject(uid, mode, op);
    probe_refusals(uid);
    char three_ref[PATH_CAP + 64];
    snprintf(three_ref, sizeof three_ref, "%s", pref.three);
    int extra = fs_save_file_new("extra", 0);
    char extra_del[PATH_CAP + 64];
    CHECK(extra > 0 && fs_save_file_delete(extra) == 0 && !file_exists(extra) &&
          probe_deleted(extra, extra_del, sizeof extra_del), "Delete of a file without a crash");
    // the number the menu's New names its default after is the number New then gives, past the deleted file's
    int said = fs_save_file_next_id(), made = fs_save_file_new("by its number", 0);
    CHECK(said == extra + 1 && made == said && fs_save_file_delete(made) == 0,
          "the number New gives: %d said, %d made, after a delete of file %d", said, made, extra);

    CHECK(tree_equal(save_root, three_ref), "the operations changed the running file's save");
    probe_copy_while_saving();

    // an import becomes one new file whatever step a crash stops it at, and that start mounts it
    for (int k = 1;; k++) {
        char imp[PATH_CAP + 16], f[PATH_CAP + 48];
        snprintf(imp, sizeof imp, "%s/save-import", profile);
        snprintf(f, sizeof f, "%s/slot_00", imp);
        plat_file_mkdir_p(f);
        snprintf(f, sizeof f, "%s/slot_00/progress.sav", imp);
        uint8_t b[64];
        memset(b, k, sizeof b);
        CHECK(plat_file_write(f, b, sizeof b), "an import to make");
        int before = probe_file_count(), expect = next_id();
        tx_crash_at = k;
        import_pending();
        int died = tx_dead, made = import_file;
        tx_crash_at = tx_dead = 0;
        probe_restart(uid);   // finishes a cut-short import, or makes the import a file now
        snprintf(f, sizeof f, "%s/%d/save/%s/slot_00/progress.sav", files_root, expect, uid_dir);
        uint64_t sz = 0;
        uint8_t *got = plat_file_read(f, &sz);
        // the start that finished the import says so; after a crash past its last rename none does, as it was done
        CHECK(probe_file_count() == before + 1 && got && sz == sizeof b && got[0] == k && running_file == expect &&
              file_current() == expect && (died ? import_file == expect || !import_file : made == expect) &&
              !exists(imp),
              "import, crash at step %d: %d files (was %d), running %d, expected %d", k, probe_file_count(), before,
              running_file, expect);
        free(got);
        if (!died) {
            printf("save files: an import cut short at each of %d steps became one file every time\n", k - 1);
            break;
        }
        if (k == 20) {
            CHECK(0, "import: no end of crash points");
            break;
        }
    }

    // with save_file set, an import still becomes a file, but the start mounts the setting's file and the menu does
    // not say the game loaded the import
    char imp[PATH_CAP + 32];
    snprintf(imp, sizeof imp, "%s/save-import/slot_00", profile);
    plat_file_mkdir_p(imp);
    cfg_setenv("NX2NAVI_SAVE_FILE", "2");
    int expect = next_id(), was = probe_file_count();
    probe_restart(uid);
    CHECK(running_file == 2 && import_file == expect && fs_save_file_import() == 0 && probe_file_count() == was + 1,
          "an import with save_file = 2: running %d, the import is file %d (%d expected), the menu's import %d",
          running_file, import_file, expect, fs_save_file_import());
    cfg_setenv("NX2NAVI_SAVE_FILE", "");
    probe_restart(uid);
    printf("save files: an import with save_file set became file %d; the start mounted file 2, and the menu does not "
           "say it loaded the import\n", expect);

    // a menu import that a start cannot finish (the move of its .import-<n> folder into place refused): it still
    // waits, so the menu keeps Play this file's Restart greyed and offers to cancel it. The next start that can makes
    // it a file and mounts it, as the menu said; a Cancel drops it, and the next start mounts what it would have.
    for (int cancel = 0; cancel < 2; cancel++) {
        plat_file_mkdir_p(imp);
        int before = running_file, made = next_id();
        fail_move = "/.import-";
        probe_restart(uid);
        fail_move = NULL;
        snprintf(p, sizeof p, "%s/.import-%d", files_root, made);
        int waits = fs_save_import_waiting(), staged = exists(p), dropped = cancel && !fs_save_import_cancel();
        int waits_after = fs_save_import_waiting(), mid = running_file;
        probe_restart(uid);
        CHECK(waits && staged && mid != made &&
                  (cancel ? dropped && !waits_after && !exists(p) && !file_exists(made) && running_file != made
                          : running_file == made && !fs_save_import_waiting()),
              "an import a start could not finish%s: waiting %d, staged %d, after the next start file %d (the import "
              "is %d)", cancel ? ", then cancelled" : "", waits, staged, running_file, made);
        printf("save files: an import a start could not finish: the menu %s it waits; %s, the next start mounts file %d "
               "(the import was to be %d)\n", waits ? "says" : "does not say", cancel ? "cancelled" : "not cancelled",
               running_file, made);
        CHECK(fs_save_file_choose(before) == 0, "choose file %d again", before);
        probe_restart(uid);
    }

    // deleted files: those past the newest three and older than 30 days go at the next start
    char del[PATH_CAP], keep[PATH_CAP + 8];
    snprintf(del, sizeof del, "%s/deleted", files_root);
    snprintf(keep, sizeof keep, "%s.keep", del);
    CHECK(!move_dir(del, keep, 1) && !plat_fs_mkdir(del), "deleted files aside");
    static const char *const old[] = {"20200101-000000-901", "20200102-000000-902", "20200103-000000-903",
                                      "20200104-000000-904", "20200105-000000-905"};
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", del, old[i]);
        plat_file_mkdir_p(p);
    }
    files_recover();
    int kept = count_dirs(del);
    snprintf(p, sizeof p, "%s/%s", del, old[2]);
    CHECK(kept == DELETED_KEEP && exists(p), "old deleted files: %d kept", kept);
    remove_tree(del);
    CHECK(!move_dir(keep, del, 1), "deleted files back");
    char stamp[24];
    utc_stamp(stamp, sizeof stamp);
    for (int i = 906; i <= 908; i++) {   // three new ones: the old one is past the newest three
        snprintf(p, sizeof p, "%s/%s-%d", del, stamp, i);
        plat_file_mkdir_p(p);
    }
    snprintf(p, sizeof p, "%s/%s", del, old[0]);
    plat_file_mkdir_p(p);
    int before = count_dirs(del);
    files_recover();
    snprintf(p, sizeof p, "%s/%s-906", del, stamp);
    CHECK(count_dirs(del) == before - 1 && exists(p), "an old deleted file beside newer ones: %d of %d kept",
          count_dirs(del), before);

    // save_file = 1 once file 1 is deleted (and other files exist) names no file, as any number no file has: the start
    // stops instead of making an empty file 1
    char one_del[PATH_CAP + 64];
    CHECK(fs_save_file_delete(1) == 0 && probe_deleted(1, one_del, sizeof one_del), "Delete of file 1");
    cfg_setenv("NX2NAVI_SAVE_FILE", "1");
    CHECK(file_next() == 0, "save_file = 1 after file 1 was deleted: file %d", file_next());
    cfg_setenv("NX2NAVI_SAVE_FILE", "");
    probe_undelete(1, one_del);
    cfg_setenv("NX2NAVI_SAVE_FILE", "1");
    CHECK(file_next() == 1, "save_file = 1 with file 1 back: file %d", file_next());
    cfg_setenv("NX2NAVI_SAVE_FILE", "");

    // save_file names file 2 by its name: Rename of file 2 is refused (the next start would stop), the list marks it;
    // the menu's choice (current) differs from the next start's file then, and Delete of it is refused and listed
    int r_file = running_file;
    CHECK(fs_save_file_choose(1) == 0, "choose file 1");
    cfg_setenv("NX2NAVI_SAVE_FILE", "sECOND");
    n = fs_save_files(list, 8);
    int named2 = 0, current1 = 0;
    for (int i = 0; i < n; i++) named2 += list[i].id == 2 && list[i].named, current1 += list[i].id == 1 && list[i].current;
    CHECK(fs_save_file_rename(2, "Other") == FS_FILE_IN_USE && fs_save_file_rename(3, "Copyof one") == 0 && named2 &&
          file_next() == 2 && fs_save_file_delete(1) == FS_FILE_IN_USE && current1,
          "save_file = sECOND: Rename of file 2 and Delete of file 1 (current) refused, %d named, %d current", named2,
          current1);
    probe_name(2, "Second", "Rename refused");
    // a Rename of another file to that name is refused too, with its own reason: the start takes the lowest file of a
    // name, so file 1 named "second" would start instead of file 2
    int r1 = fs_save_file_rename(1, "second"), r3 = fs_save_file_rename(3, "  SECOND ");
    CHECK(r1 == FS_FILE_NAME_SET && r3 == FS_FILE_NAME_SET && file_next() == 2,
          "save_file = sECOND: Rename of files 1 and 3 to that name: %d and %d, the next start's file %d", r1, r3,
          file_next());
    probe_name(1, "Hero", "Rename of file 1 to save_file's name");
    probe_name(3, "Copyof one", "Rename of file 3 to save_file's name");
    printf("save files: save_file = sECOND names file 2; a Rename of file 1 or 3 to that name: %s, the next start's "
           "file %d\n", r1 == FS_FILE_NAME_SET && r3 == FS_FILE_NAME_SET ? "refused" : "done", file_next());
    cfg_setenv("NX2NAVI_SAVE_FILE", "");
    CHECK(fs_save_file_choose(r_file) == 0, "choose the running file again");

    // more files than the menu lists (64): the lowest, sorted, with the running file and the next start's in the list
    static FsSaveFile many[64];
    for (int id = 900; id < 980; id++) {
        snprintf(p, sizeof p, "%s/%d/save/%s", files_root, id, uid_dir);
        plat_file_mkdir_p(p);
    }
    CHECK(fs_save_file_choose(979) == 0, "choose file 979");
    n = fs_save_files(many, 64);
    int sorted = 1, has_running = 0, has_next = 0;
    for (int i = 0; i < n; i++) {
        sorted &= !i || many[i - 1].id < many[i].id;
        has_running |= many[i].id == running_file && many[i].running, has_next |= many[i].id == 979 && many[i].next;
    }
    CHECK(n == 64 && sorted && has_running && has_next && many[0].id == 1,
          "a list cut at 64 of %d files: %d listed, sorted %d, running %d, next %d", probe_file_count(), n, sorted,
          has_running, has_next);
    printf("save files: a list of 64 out of %d files: the lowest in order, with the running file and the next start's\n",
           probe_file_count());
    CHECK(fs_save_file_choose(r_file) == 0, "choose the running file again");
    for (int id = 900; id < 980; id++) {
        snprintf(p, sizeof p, "%s/%d", files_root, id);
        remove_tree(p);
    }

    n = fs_save_files(list, 8);
    int running = 0, next = 0;
    for (int i = 0; i < n; i++) running += list[i].running, next += list[i].next;
    CHECK(n >= 6 && exists(one) && running == 1 && next == 1, "the list at the end: %d files, %d running, %d next", n,
          running, next);
    printf("save files: New, Copy, Rename, choose, save_file, per-file commits and backups, Delete, import: %s\n",
           failures ? "see FAIL lines" : "ok");
}

static void probe_storage(void) {
    char tmp[PLAT_PATH_MAX * 3], dir[PLAT_PATH_MAX * 3 + 64];
    need(plat_temp_dir(tmp, sizeof tmp), "temporary directory");
    snprintf(dir, sizeof dir, "%s/nx2navi-probe-fs-%u", tmp, plat_process_id());
    remove_tree(dir);
    fs_host_set_profile(dir);
    uint64_t uid[2], other[2], h = 0, h2 = 0, d = 0, l = 0;
    account_uid(uid);
    other[0] = uid[0] ^ 1, other[1] = uid[1];
    static uint8_t data[65536], back[65536];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + (i >> 8));
    int32_t no_flush = 0, flush = 1;
    int64_t size = -1, count = -1;
    uint64_t got = 0;
    int type = -1;

    CHECK(RES(S_MOUNT_SAVE, U("save"), U(other)) == FS_R_TARGET_NOT_FOUND, "MountSaveData for another user");
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0, "MountSaveData");
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == FS_R_ALREADY_EXISTS, "MountSaveData twice");
    CHECK((CALL(S_SAVE_EXISTS, NX2NAVI_APPLICATION_ID, U(uid)) & 0xff) == 1 &&
          (CALL(S_SAVE_EXISTS, NX2NAVI_APPLICATION_ID + 0x1000, U(uid)) & 0xff) == 0, "IsSaveDataExisting");
    CHECK(RES(S_MOUNT_SAVE_RO, U("other"), NX2NAVI_APPLICATION_ID + 0x1000, U(uid)) == FS_R_TARGET_NOT_FOUND,
          "MountSaveDataReadOnly of another title");
    CHECK(!fs_save_busy(), "idle at start");

    // create, write, commit
    CHECK(RES(S_CREATE_FILE, U("save:/probe.sav"), 0) == 0 && fs_save_busy(), "CreateFile");
    CHECK(RES(S_CREATE_FILE, U("save:/probe.sav"), 0) == FS_R_ALREADY_EXISTS, "CreateFile twice");
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_WRITE) == 0, "OpenFile for writing");
    CHECK(RES(S_WRITE_FILE, h, 0, U(data), 16, U(&no_flush)) == FS_R_APPEND_REQUIRED, "a growing write without append");
    CALL(S_CLOSE_FILE, h);
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_WRITE | FS_OPEN_APPEND) == 0, "OpenFile for append");
    CHECK(RES(S_WRITE_FILE, h, 0, U(data), sizeof data / 2, U(&no_flush)) == 0 &&
          RES(S_WRITE_FILE, h, sizeof data / 2, U(data + sizeof data / 2), sizeof data / 2, U(&flush)) == 0,
          "WriteFile");
    CHECK(RES(S_FILE_SIZE, U(&size), h) == 0 && size == sizeof data, "GetFileSize after writing");
    CHECK(RES(S_READ_FILE, U(&got), h, 0, U(back), 16) == FS_R_OPEN_MODE, "ReadFile on a write-only handle");
    CHECK(RES(S_FLUSH_FILE, h) == 0, "FlushFile");
    CHECK(fs_save_busy(), "a save file open for writing does not count as a save in progress");
    CALL(S_CLOSE_FILE, h);
    CHECK(fs_save_busy(), "closed but uncommitted save data does not count as a save in progress");
    CHECK(tx_commit() == 0 && !fs_save_busy(), "Commit");
    CHECK(RES(S_COMMIT, U("nomount")) == FS_R_PATH_NOT_FOUND, "Commit of an unknown mount");

    // paths
    CHECK(RES(S_CREATE_FILE, U("save:/../x"), 0) == FS_R_INVALID_PATH, "save:/../x");
    CHECK(RES(S_CREATE_FILE, U("save:/a\\b"), 0) == FS_R_INVALID_PATH, "save:/a\\b");
    CHECK(RES(S_CREATE_FILE, U("save:/a:b"), 0) == FS_R_INVALID_PATH, "save:/a:b");
    CHECK(RES(S_CREATE_FILE, U("save:/a."), 0) == FS_R_INVALID_PATH, "save:/a.");
    CHECK(RES(S_CREATE_FILE, U("nomount:/x"), 0) == FS_R_PATH_NOT_FOUND, "a path on no mount");

    // directories
    CHECK(RES(S_CREATE_DIRECTORY, U("save:/dir")) == 0 && RES(S_CREATE_FILE, U("save:/dir/inner.bin"), 8) == 0,
          "CreateDirectory");
    CHECK(RES(S_ENTRY_TYPE, U(&type), U("save:/dir")) == 0 && type == FS_ENTRY_DIRECTORY, "GetEntryType (directory)");
    static DirectoryEntry ents[8];
    CHECK(RES(S_OPEN_DIRECTORY, U(&d), U("save:/"), 3) == 0, "OpenDirectory");
    CHECK(RES(S_READ_DIRECTORY, U(&count), U(ents), d, 8) == 0 && count == 2, "ReadDirectory count %lld",
          (long long)count);
    int seen = 0;
    for (int i = 0; i < count && i < 8; i++) {
        if (!strcmp(ents[i].name, "dir") && ents[i].type == FS_ENTRY_DIRECTORY) seen |= 1;
        if (!strcmp(ents[i].name, "probe.sav") && ents[i].type == FS_ENTRY_FILE && ents[i].size == sizeof data)
            seen |= 2;
    }
    CHECK(seen == 3, "ReadDirectory entries");
    CHECK(RES(S_READ_DIRECTORY, U(&count), U(ents), d, 8) == 0 && count == 0, "ReadDirectory at the end");
    CALL(S_CLOSE_DIRECTORY, d);
    CHECK(RES(S_OPEN_DIRECTORY, U(&d), U("save:/"), 2) == 0 && RES(S_READ_DIRECTORY, U(&count), U(ents), d, 8) == 0 &&
          count == 1 && ents[0].type == FS_ENTRY_FILE, "ReadDirectory, files only");
    CALL(S_CLOSE_DIRECTORY, d);

    CHECK(tx_commit() == 0, "Commit of the directories");

    // reopen after unmount: the bytes come back through the overrides and are on disk
    CALL(S_UNMOUNT, U("save"));
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_READ) == FS_R_PATH_NOT_FOUND, "open after Unmount");
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0, "MountSaveData again");
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_READ) == 0, "reopen");
    memset(back, 0, sizeof back);
    CHECK(RES(S_FILE_SIZE, U(&size), h) == 0 && size == sizeof data &&
          RES(S_READ_EXACT, h, 0, U(back), sizeof back) == 0 && !memcmp(back, data, sizeof data), "reopened bytes");
    CHECK(RES(S_READ_FILE, U(&got), h, sizeof data - 10, U(back), 100) == 0 && got == 10, "short read at the end");
    CHECK(RES(S_WRITE_FILE, h, 0, U(data), 1, U(&no_flush)) == FS_R_OPEN_MODE, "WriteFile on a read-only handle");
    CALL(S_CLOSE_FILE, h);
    char host[1200];
    snprintf(host, sizeof host, "%s/save/%016llx%016llx/probe.sav", dir, (unsigned long long)uid[1],
             (unsigned long long)uid[0]);
    PlatFile *f = plat_file_open(host);
    memset(back, 0, sizeof back);
    CHECK(f && independent_read(f, back, sizeof back, 0) == sizeof back && !memcmp(back, data, sizeof data),
          "the save file on disk");
    if (f) plat_file_close(f);

    // resize, rename, delete
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_READ | FS_OPEN_WRITE) == 0 &&
          RES(S_SET_FILE_SIZE, h, 100) == 0 && RES(S_FILE_SIZE, U(&size), h) == 0 && size == 100, "SetFileSize");
    CHECK(RES(S_OPEN_FILE, U(&h2), U("save:/probe.sav"), FS_OPEN_READ) == 0, "a second handle on one file");
    CALL(S_CLOSE_FILE, h2);
    CALL(S_CLOSE_FILE, h);
    CHECK(RES(S_RENAME_FILE, U("save:/probe.sav"), U("save:/dir/inner.bin")) == FS_R_ALREADY_EXISTS,
          "RenameFile over an existing file");
    CHECK(RES(S_RENAME_FILE, U("save:/probe.sav"), U("save:/renamed.sav")) == 0 &&
          RES(S_ENTRY_TYPE, U(&type), U("save:/renamed.sav")) == 0 && type == FS_ENTRY_FILE &&
          RES(S_ENTRY_TYPE, U(&type), U("save:/probe.sav")) == FS_R_PATH_NOT_FOUND, "RenameFile");
    CHECK(RES(S_DELETE_FILE, U("save:/renamed.sav")) == 0 &&
          RES(S_DELETE_FILE, U("save:/renamed.sav")) == FS_R_PATH_NOT_FOUND, "DeleteFile");
    CHECK(RES(S_DELETE_RECURSIVE, U("save:/dir")) == 0 &&
          RES(S_ENTRY_TYPE, U(&type), U("save:/dir")) == FS_R_PATH_NOT_FOUND, "DeleteDirectoryRecursively");
    CHECK(tx_commit() == 0 && !fs_save_busy(), "Commit after the changes");
    probe_save_tx(uid);
    probe_save_files(uid);

    // temporary and cache storage
    CHECK(RES(S_MOUNT_TEMP, U("temp")) == 0 && RES(S_CREATE_FILE, U("temp:/t.bin"), 4) == 0 && !fs_save_busy(),
          "temporary storage");
    CHECK(RES(S_CREATE_CACHE, 3, 1 << 20, 1 << 20) == 0 && RES(S_CREATE_CACHE, 3, 1 << 20, 1 << 20) ==
          FS_R_ALREADY_EXISTS, "CreateCacheStorage");
    CHECK(RES(S_MOUNT_CACHE, U("cache"), 3) == 0 && RES(S_MOUNT_CACHE, U("cache9"), 9) == FS_R_TARGET_NOT_FOUND,
          "MountCacheStorage");
    CacheStorageInfo infos[4];
    int ncache = -1;
    CHECK(RES(S_OPEN_CACHE_LIST, U(&l)) == 0 && RES(S_READ_CACHE_LIST, U(&ncache), U(infos), l, 4) == 0 &&
          ncache == 1 && infos[0].index == 3, "cache storage list");
    CALL(S_CLOSE_CACHE_LIST, l);
    CALL(S_SET_PRIORITY, 1);
    CALL(S_UNMOUNT, U("cache"));
    CALL(S_UNMOUNT, U("temp"));
    CHECK(RES(S_DELETE_CACHE, 3) == 0, "DeleteCacheStorage");
    CALL(S_UNMOUNT, U("save"));

    char lock[PLAT_PATH_MAX * 3 + 96];
    snprintf(lock, sizeof lock, "%s/instance.lock", dir);
    plat_unlock_file(lock);   // storage_init's one-game-per-profile lock, held open until given back
    int removed = !remove_tree(dir) && !exists(dir);
    CHECK(removed, "the temporary profile was not deleted");
    printf("storage: save create/write/commit/reopen, directories, rename/delete, temporary and cache storage in a "
           "temporary profile: %s; profile deleted: %s\n", failures ? "see FAIL lines" : "ok", removed ? "yes" : "no");
}

int probe_fs(void) {
    fs_init();
    for (int i = 0; i < NPROBE; i++) {
        GuestFn fn = hostfn_override(probe_symbols[i]);
        if (!fn) rt_fatal("probe-fs: no HOST_OVERRIDE for %s", probe_symbols[i]);
        probe_addr[i] = hostfn_register(probe_symbols[i], fn);
    }
    printf("overrides: %d nn::fs overrides registered\n", NPROBE);
    pcpu = plat_aligned_alloc(sizeof(Cpu), 64);
    need(pcpu != NULL, "probe Cpu");
    memset(pcpu, 0, sizeof *pcpu);
    uint8_t *stack = mem_kernel_alloc(0x10000, 16);
    pcpu->sp = (uint64_t)(uintptr_t)stack + 0x10000;
    pcpu->excl_addr = ~0ull;
    for (int r = 19; r < 30; r++) pcpu->x[r] = 0x1900 + r;
    CALL(S_INITIALIZE, 0);   // nn::fs::InitializeForApplication: fs_init again, idempotent
    uint64_t files = probe_romfs();
    probe_storage();
    int kept = 1;
    for (int r = 19; r < 30; r++) kept &= pcpu->x[r] == 0x1900u + r;
    CHECK(kept && pcpu->sp == (uint64_t)(uintptr_t)stack + 0x10000, "an override lost a callee-saved register");
    mem_kernel_free(stack, 0x10000);
    plat_aligned_free(pcpu);
    if (failures) {
        printf("probe-fs: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-fs: passed (%llu files, %d sampled)\n", (unsigned long long)files, SAMPLES);
    return 0;
}
