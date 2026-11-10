// Restorations (restore.h): the declared ones (runtime/restore_<name>.c), their switches, and what a start does with
// them. romfs.c overlay_init calls restore_apply after the mods' overlays are chosen: the edits of the switched-on
// restorations, grouped by file so that several of them on one file all apply to it, go through the engine
// (restore_engine.c) on the file as the game would read it, and romfs.c serves each edited file in its place and
// raises its resource size table entry when it grew (restbl_overlay). Every edit logs a line: what it did, or why it
// was skipped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "restore.h"

#ifdef _WIN32
// PE/COFF: the linker sorts .rst$a < .rst$m (the RESTORATION entries) < .rst$z by the text after '$'.
__attribute__((section(".rst$a"), used)) static const Restoration *const rst_start[1];
__attribute__((section(".rst$z"), used)) static const Restoration *const rst_stop[1];
#else
// ELF: the linker brackets the section with __start_ and __stop_; weak, as a build without restorations has none.
extern const Restoration *const __start_nx2navi_rst[] __attribute__((weak));
extern const Restoration *const __stop_nx2navi_rst[] __attribute__((weak));
#define rst_start __start_nx2navi_rst
#define rst_stop __stop_nx2navi_rst
#endif

static const Restoration **list;
static int nlist;
static int *started_applied, *started_skipped;
// 1 once run has counted the start's edits into started_*: the menu reads them from its own thread, maybe while the
// RomFS is still starting (romfs.c overlay_init on the game's first file access).
static volatile int32_t started_ready;
static int probing;   // --probe-restore: the start applies nothing, the probe applies every restoration itself

static int by_key(const void *a, const void *b) {
    return strcmp((*(const Restoration *const *)a)->key, (*(const Restoration *const *)b)->key);
}

static void init(void *param) {
    (void)param;
    const Restoration *const *p = rst_start, *const *e = rst_stop;
    __asm__("" : "+r"(p), "+r"(e));   // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const Restoration *const *q = p; q < e; q++) n += *q != NULL;   // PE pads between objects with zeros
    list = calloc(n ? n : 1, sizeof *list);
    started_applied = calloc(n ? n : 1, sizeof *started_applied);
    started_skipped = calloc(n ? n : 1, sizeof *started_skipped);
    if (!list || !started_applied || !started_skipped) rt_fatal("restore: out of memory");
    for (const Restoration *const *q = p; q < e; q++)
        if (*q) list[nlist++] = *q;
    qsort(list, (size_t)nlist, sizeof *list, by_key);
    for (int i = 1; i < nlist; i++)   // one setting each
        if (!strcmp(list[i]->key, list[i - 1]->key)) rt_fatal("restore: two restorations have the key %s", list[i]->key);
    // A copy's Hash names its object in the whole area (both placement files): no two copies may take the same one.
    for (int i = 0; i < nlist; i++)
        for (int a = 0; a < list[i]->nedits; a++)
            for (int j = i; j < nlist; j++)
                for (int b = j == i ? a + 1 : 0; b < list[j]->nedits; b++) {
                    const RestoreEdit *x = &list[i]->edits[a], *y = &list[j]->edits[b];
                    if (x->kind == RESTORE_KIND_ADD && y->kind == RESTORE_KIND_ADD && x->new_hash == y->new_hash)
                        rt_fatal("restore: %s and %s both add a copy as the Hash %llu", list[i]->key, list[j]->key,
                                 (unsigned long long)x->new_hash);
                }
}

static void ensure(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, init, NULL);
}

int restore_count(void) {
    ensure();
    return nlist;
}

const Restoration *restore_at(int i) {
    ensure();
    return i >= 0 && i < nlist ? list[i] : NULL;
}

static void var_of(const Restoration *r, char *out, size_t n) { snprintf(out, n, "NX2NAVI_RESTORE_%s", r->key); }

int restore_on(const Restoration *r) {
    char var[128];
    var_of(r, var, sizeof var);
    const char *v = cfg_get(var);
    return v && *v && strcmp(v, "0") != 0;
}

void restore_set(const Restoration *r, int on) {
    char var[128];
    var_of(r, var, sizeof var);
    cfg_set(var, on ? "1" : "0");
}

static int index_of(const Restoration *r) {
    ensure();
    for (int i = 0; i < nlist; i++)
        if (list[i] == r) return i;
    return -1;
}

void restore_started(const Restoration *r, int *applied, int *skipped) {
    int i = plat_cas32(&started_ready, 0, 0) ? index_of(r) : -1;   // an atomic read of the flag
    *applied = i < 0 ? 0 : started_applied[i];
    *skipped = i < 0 ? 0 : started_skipped[i];
}

// ---- applying them

typedef struct { int r, e; } Pick;   // restoration and edit index

static int by_file(const void *a, const void *b) {
    const Pick *x = a, *y = b;
    int c = strcmp(list[x->r]->edits[x->e].file, list[y->r]->edits[y->e].file);
    return c ? c : x->r != y->r ? x->r - y->r : x->e - y->e;
}

typedef struct { const Pick *picks; const char *file; } Group;

static void note(void *ctx, int k, const char *text) {
    const Group *g = ctx;
    if (k < 0) {
        rt_log("restore: %s: %s", g->file, text);
        if (probing) printf("  %s: %s\n", g->file, text);
        return;
    }
    const Restoration *r = list[g->picks[k].r];
    rt_log("restore: %s: %s: %s", r->key, g->file, text);
    if (probing) printf("  %s #%d: %s\n", r->key, g->picks[k].e + 1, text);
}

// Pack/ZsDic.pack.zs unpacked (malloc'd), NULL when it cannot be read.
static void *dictionaries(void *(*read)(const char *rel, size_t *n), size_t *n) {
    size_t zn;
    void *z = read("Pack/ZsDic.pack.zs", &zn), *d = NULL;
    unsigned long long cs = z ? ZSTD_getFrameContentSize(z, zn) : ZSTD_CONTENTSIZE_ERROR;
    if (cs != ZSTD_CONTENTSIZE_UNKNOWN && cs != ZSTD_CONTENTSIZE_ERROR && cs < (64u << 20) && (d = malloc(cs ? cs : 1)))
        if (ZSTD_decompress(d, (size_t)cs, z, zn) != cs) {
            free(d);
            d = NULL;
        }
    free(z);
    *n = d ? (size_t)cs : 0;
    return d;
}

// The pack members the last run added or rewrote (restore_member): the member's path, its size before (0: added) and
// after, its bytes declared (else the game's, copied or edited).
typedef struct { const char *path; size_t was, now; int declared; } Member;
static Member *members;
static int nmembers;

int restore_member(int i, const char **path, size_t *was, size_t *now, int *declared) {
    if (i < 0 || i >= nmembers) return 0;
    *path = members[i].path, *was = members[i].was, *now = members[i].now, *declared = members[i].declared;
    return 1;
}

// The edits of the switched-on restorations (all: of every declared one); serve NULL applies them without serving.
// Returns the number of edits skipped.
static int run(int all, void *(*read)(const char *rel, size_t *n), int (*serve)(const char *rel, void *out, size_t n)) {
    ensure();
    // the counts are of this run alone: the game runs once, the tests and a probe after them run again
    memset(started_applied, 0, sizeof *started_applied * (size_t)(nlist ? nlist : 1));
    memset(started_skipped, 0, sizeof *started_skipped * (size_t)(nlist ? nlist : 1));
    free(members);
    members = NULL, nmembers = 0;
    int total = 0, on = 0, np = 0, skipped = 0;
    for (int i = 0; i < nlist; i++) total += list[i]->nedits;
    if (!total) return 0;
    Pick *picks = malloc(sizeof *picks * (size_t)total);
    if (!picks) rt_fatal("restore: out of memory");
    for (int i = 0; i < nlist; i++)   // each switch read once: the menu may switch one meanwhile
        if (all || restore_on(list[i])) {
            on++;
            for (int e = 0; e < list[i]->nedits; e++) picks[np++] = (Pick){i, e};
        }
    if (!np) {
        free(picks);
        return 0;
    }
    const RestoreEdit **edits = malloc(sizeof *edits * (size_t)np);
    int *ok = malloc(sizeof *ok * (size_t)np);
    RestoreMemberSize *sizes = malloc(sizeof *sizes * (size_t)np);
    members = malloc(sizeof *members * (size_t)np);
    if (!edits || !ok || !sizes || !members) rt_fatal("restore: out of memory");
    qsort(picks, (size_t)np, sizeof *picks, by_file);
    double t0 = (double)plat_now_ns() / 1e9;
    size_t dn = 0;
    void *dic = dictionaries(read, &dn);
    if (!dic) rt_log("restore: cannot read Pack/ZsDic.pack.zs; files packed with its dictionaries are skipped");
    int files = 0;
    for (int a = 0, b; a < np; a = b) {
        const char *file = list[picks[a].r]->edits[picks[a].e].file;
        for (b = a; b < np && !strcmp(list[picks[b].r]->edits[picks[b].e].file, file); b++)
            edits[b - a] = &list[picks[b].r]->edits[picks[b].e];
        Group g = {picks + a, file};
        size_t n, out_n = 0;
        void *in = read(file, &n), *out = NULL;
        if (probing) printf("%s\n", file);
        if (!in) {
            for (int k = 0; k < b - a; k++) {
                ok[k] = 0;
                note(&g, k, "skipped: the file is missing");
            }
        } else {
            out = restore_file(edits, b - a, in, n, dic, dn, read, romfs_with, ok, sizes, note, &g, &out_n);
            free(in);
        }
        if (out && serve && !serve(file, out, out_n)) {
            for (int k = 0; k < b - a; k++)
                if (ok[k]) note(&g, k, "undone: the edited file cannot be served");
            for (int k = 0; k < b - a; k++) ok[k] = 0;
        } else if (out) {
            files++;
            if (!serve) free(out);
        }
        for (int k = 0; k < b - a; k++) {
            int r = picks[a + k].r;
            if (ok[k]) started_applied[r]++;
            else started_skipped[r]++, skipped++;
            if (ok[k] && sizes[k].now)
                members[nmembers++] = (Member){edits[k]->member, sizes[k].was, sizes[k].now, edits[k]->bytes != NULL};
        }
    }
    plat_xchg32(&started_ready, 1);   // a full barrier: the counts above are written before the flag
    for (int i = 0; i < nlist; i++)
        if (started_applied[i] || started_skipped[i])
            rt_log("restore: %s: %d of %d edits applied", list[i]->key, started_applied[i], list[i]->nedits);
    rt_log("restore: %d restorations, %d edits (%d skipped), %d files %s, %.0f ms", on, np, skipped, files,
           serve ? "served" : "edited (not served: --probe-restore)", ((double)plat_now_ns() / 1e9 - t0) * 1000);
    free(dic);
    free(ok);
    free(sizes);
    free(edits);
    free(picks);
    return skipped;
}

void restore_apply(void *(*read)(const char *rel, size_t *n), int (*serve)(const char *rel, void *out, size_t n)) {
    if (!probing) run(0, read, serve);
}

int restore_probe(void) {
    probing = 1;
    fs_init();
    ensure();
    int edits = 0;
    for (int i = 0; i < nlist; i++) edits += list[i]->nedits;
    printf("probe-restore: %d restorations, %d edits, every one applied to the files as the game reads them\n", nlist,
           edits);
    int skipped = run(1, romfs_read_served, NULL);
    for (int i = 0; i < nlist; i++)
        printf("%s (%s): %d of %d edits applied\n", list[i]->key, list[i]->name, started_applied[i], list[i]->nedits);
    printf("probe-restore: %s (%d of %d edits skipped)\n", skipped ? "FAILED" : "passed", skipped, edits);
    return skipped ? 1 : 0;
}
