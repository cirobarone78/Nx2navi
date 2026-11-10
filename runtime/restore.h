// Restorations (restore.c, docs/restorations.md): behaviours of the original game that this release changed, each a
// switch of its own, NX2NAVI_RESTORE_<KEY> = 1 (nx2navi.ini [restorations], the Restorations tab of the menu). One is
// a list of edits to the player's own game files, each setting a number: below a placed object of a placement file,
// or in a BYML member of an actor pack. Once at start, romfs.c applies the edits of the switched-on restorations to
// each file as the game would read it (a mod's version, else the original) and serves the result from
// <profile>/restore/; an edit that does not find what it names is skipped and logged, never applied elsewhere.
//
// A restoration is declared in a file of its own, runtime/restore_<name>.c:
//
//   #include "restore.h"
//   static const RestoreEdit edits[] = {
//       RESTORE_SET("Banc/X/X_Static.bcett.byml.zs", "Area", 1234567890123456789u, 1.0, 100.0, -2.5, "Scale/2", 7, 3),
//   };
//   RESTORATION(example, "EXAMPLE", "Menu label", "One-line description.", edits);
#pragma once
#include <stddef.h>
#include <stdint.h>

// Set the number at path from was to now, in file (RomFS path of a placement file or other BYML, ".zs" ones unpacked,
// or of a pack).
typedef struct {
    const char *file;
    const char *member;   // the BYML member of that pack, NULL for a file that is a BYML itself
    // The placed object the number is below, as the game ships it: its Gyaml, its Hash and its Translate (each within
    // 1 cm). hash 0: the number is in the document itself (a pack member's parameters).
    const char *gyaml;
    uint64_t hash;
    float at[3];
    // The number below the object (or the document) at path, keys and array indices separated by '/' ("Scale/2"), a
    // number or a bool. was: what the game ships (within 1e-5 of it, relative); now: ours.
    const char *path;
    double was, now;
} RestoreEdit;

typedef struct {
    const char *key;     // the setting is NX2NAVI_RESTORE_<key>: upper case, digits and '_'
    const char *name;    // the menu's label
    const char *desc;    // the menu's one-line description
    const RestoreEdit *edits;
    int nedits;
} Restoration;

// Set the value at path below the placed object gyaml / hash at (x, y, z) from was to now.
#define RESTORE_SET(file, gyaml, hash, x, y, z, path, was, now) {file, 0, gyaml, hash, {x, y, z}, path, was, now}
// Set the value at path of the pack's BYML member (NULL: of the file itself) from was to now.
#define RESTORE_SET_MEMBER(file, member, path, was, now) {file, member, 0, 0, {0}, path, was, now}

// The registry: every RESTORATION of the linked objects meets in one section (as host.h's HOST_OVERRIDE entries do),
// which restore.c walks: on PE/COFF ".rst$m", sorted between restore.c's ".rst$a" and ".rst$z" markers; on ELF
// "nx2navi_rst", between the linker's __start_nx2navi_rst and __stop_nx2navi_rst.
#ifdef _WIN32
#define RESTORE_SECTION section(".rst$m")
#else
#define RESTORE_SECTION section("nx2navi_rst")
#endif
#define RESTORATION(id, key, name, desc, edits)                                                                   \
    static const Restoration restore_##id = {key, name, desc, edits, (int)(sizeof edits / sizeof *edits)};         \
    __attribute__((used, RESTORE_SECTION)) static const Restoration *const restore_entry_##id = &restore_##id

// The declared restorations, sorted by key; NULL past the end.
int restore_count(void);
const Restoration *restore_at(int i);
// 1 when its setting is on (NX2NAVI_RESTORE_<key> set and not "0"); restore_set writes it (the menu).
int restore_on(const Restoration *r);
void restore_set(const Restoration *r, int on);
// The edits of r this start applied and skipped; both 0 when it was off at start, or before the start applied them.
// Any thread.
void restore_started(const Restoration *r, int *applied, int *skipped);

// romfs.c overlay_init: applies the switched-on restorations. read returns a RomFS file as the game would read it
// (malloc'd, NULL when missing); serve takes over out[0..n) and serves it in place of rel, the file read last, and
// returns 0 when it cannot.
void restore_apply(void *(*read)(const char *rel, size_t *n), int (*serve)(const char *rel, void *out, size_t n));
// --probe-restore (main.c): every declared restoration against the files as the game would read them, nothing served;
// prints a line per edit. 0 when every edit applied.
int restore_probe(void);
// romfs.c: RomFS file rel as the game reads it (the overlay that replaces it, else the original); malloc'd, NULL when
// missing or 16 MiB or more.
void *romfs_read_served(const char *rel, size_t *n);

// The edit engine on one file (restore.c; tests/restore_check.c runs it on files of its own): applies edits[0..n),
// all naming this file, to in[0..size), the file's bytes as stored. A zstd frame is unpacked and packed again with the
// dictionary of its id among the members of dic[0..dic_n), Pack/ZsDic.pack.zs unpacked (NULL: none). ok[k] gets 1 for
// each edit applied; note gets what each edit did or why it was skipped (k = -1: the file itself). Returns the edited
// file (malloc'd, packed as the original was) when an edit applied, else NULL.
void *restore_file(const RestoreEdit *const *edits, int n, const void *in, size_t size, const void *dic, size_t dic_n,
                   int *ok, void (*note)(void *ctx, int k, const char *text), void *ctx, size_t *out_n);
