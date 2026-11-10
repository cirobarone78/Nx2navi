// The restorations (runtime/restore.c, restore_engine.c, byml.c, sarc.c) on the fixtures tests/restore_check.py
// writes into the folder given: runs edits of every kind on them, checks which apply and which are skipped, writes the
// results for the script to decode, feeds the parser, the engine and the SARC reader and writer every cut length of
// those files and damaged copies, checks the parser refuses dicts out of order (and quickly), and runs restore.c's
// switches, start and probe with the settings, the log and the RomFS stubbed below.
// Usage: restore_check <folder>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>

#include "byml.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "restore.h"
#include "sarc.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static const char *dir;

static uint8_t *load(const char *name, size_t *n) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL: cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len ? (size_t)len : 1);
    *n = fread(p, 1, (size_t)len, f);
    fclose(f);
    return p;
}

static void save(const char *name, const void *p, size_t n) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) {
        printf("FAIL: cannot write %s\n", path);
        exit(1);
    }
    fclose(f);
}

static void note(void *ctx, int k, const char *text) { printf("  %s %d: %s\n", (const char *)ctx, k, text); }

static uint8_t *unzstd(const uint8_t *z, size_t n, size_t *out_n) {
    unsigned long long cs = ZSTD_getFrameContentSize(z, n);
    uint8_t *p = malloc((size_t)cs);
    *out_n = ZSTD_decompress(p, (size_t)cs, z, n);
    if (ZSTD_isError(*out_n) || *out_n != cs) {
        printf("FAIL: fixture does not unpack\n");
        exit(1);
    }
    return p;
}

// The fixtures other files' edits copy from: "test/<name>" is <folder>/<name>; NULL when it is not there.
static void *fetch(const char *rel, size_t *n) {
    char path[4096];
    if (strncmp(rel, "test/", 5)) return NULL;
    snprintf(path, sizeof path, "%s/%s", dir, rel + 5);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fclose(f);
    return load(rel + 5, n);
}

// The area's static compound for the test's placements: one file, test/compound.bphsc, whatever the prefix asks.
static const char *list(const char *prefix, int i) {
    return !strncmp(prefix, "Phive/StaticCompoundBody/", 25) && i == 0 ? "test/compound.bphsc" : NULL;
}

// Runs edits[0..n) on file; ok must come out as want. Returns the edited file (or NULL); sizes holds what each edit
// reported of a member's size.
static RestoreMemberSize sizes[32];
static void *run(const char *what, const RestoreEdit *edits, int n, const void *in, size_t size, const void *dic,
                 size_t dic_n, const int *want, size_t *out_n) {
    const RestoreEdit *ptr[32];
    int ok[32];
    for (int k = 0; k < n; k++) ptr[k] = &edits[k];
    printf("%s\n", what);
    void *out = restore_file(ptr, n, in, size, dic, dic_n, fetch, list, ok, sizes, note, (void *)what, out_n);
    int any = 0;
    for (int k = 0; k < n; k++) {
        CHECK(ok[k] == want[k], "%s: edit %d %s", what, k, want[k] ? "should apply" : "should be skipped");
        CHECK(!sizes[k].now || ok[k], "%s: edit %d reported a member's size but did not apply", what, k);
        CHECK(!ok[k] || edits[k].kind != RESTORE_KIND_ADD_MEMBER || (!sizes[k].was && sizes[k].now),
              "%s: edit %d added a member but did not report its size", what, k);
        any |= ok[k];
    }
    CHECK(!out == !any, "%s: a file came back although no edit applied, or none although one did", what);
    return out;
}

#define F "test/place.bcett.byml.zs"

// The engine on 1000 damaged copies of raw[0..raw_n) (an unpacked placement) with edits[0..n): never a crash, and a
// file comes back only when an edit applied and it reads as a BYML (on every failure nothing is served and the game
// reads its own file).
static void engine_damage(const char *what, const RestoreEdit *edits, int n, const uint8_t *raw, size_t raw_n,
                          uint32_t *seed) {
    const RestoreEdit *ptrs[32];
    int oks[32], served = 0;
    uint8_t *bad = malloc(raw_n);
    for (int k = 0; k < n; k++) ptrs[k] = &edits[k];
    for (int round = 0; round < 1000; round++) {
        memcpy(bad, raw, raw_n);
        for (int j = 0; j < 4; j++) {
            *seed = *seed * 1103515245u + 12345u;
            bad[(*seed >> 8) % raw_n] ^= (uint8_t)(1u << (*seed & 7));
        }
        size_t en;
        uint8_t *e = restore_file(ptrs, n, bad, raw_n, NULL, 0, fetch, list, oks, NULL, NULL, NULL, &en);
        int any = 0;
        for (int k = 0; k < n; k++) any |= oks[k];
        CHECK(!e == !any, "damaged %s %d: a file came back with no edit applied, or none with one", what, round);
        Byml b;
        const char *why;
        if (e && byml_parse(&b, e, en, &why)) byml_free(&b);
        else CHECK(!e, "damaged %s %d: the edited file does not parse (%s)", what, round, why);
        served += e != NULL;
        free(e);
    }
    free(bad);
    printf("engine: %d of 1000 damaged %s edited\n", served, what);
}

static void placement(void) {
    size_t n, dn, out_n;
    uint8_t *in = load("place.byml.zs", &n), *dic = load("zsdic.sarc", &dn);
    static const RestoreEdit edits[] = {
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9, "Scale/1", 2, 5),        // 0 applies (a float)
        RESTORE_SET(F, "Obj_B", 1003, 7, 8, 9, "Scale/1", 2, 5),        // 1 another object under that hash
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9.5, "Scale/1", 2, 5),      // 2 elsewhere
        RESTORE_SET(F, "Obj_Z", 4242, 0, 0, 0, "Scale/1", 2, 5),        // 3 not there
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Count", 5, 9),  // 4 applies (an int)
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Speed", 0.75, 1),   // 5 not the value expected
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Nope", 1, 2),   // 6 no such key
        RESTORE_SET_MEMBER(F, NULL, "Misc/Double", 3.141592653589793, 2.5),   // 7 applies (a double, on the root)
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9, "Gyaml", 1, 2),          // 8 not a number
        RESTORE_SET(F, "Obj_C", 18446744073709551557ull, -1.5, 0.25, 1e6, "Translate/0", -1.5, -1.5),   // 9 applies
        RESTORE_SET(F, "Obj_A", 1003, 7, 8, 9, "Scale/7", 1, 2),        // 10 past the array's end
        RESTORE_SET(F, "Obj_B", 1002, 4, 5, 6, "Rotate/1", 1.5, 3),     // 11 applies (another object)
        RESTORE_SET(F, "Obj_B", 1002, 4, 5, 6, "Bakeable", 1, 0),       // 12 applies (a bool)
    };
    static const int want[] = {1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1};
    _Static_assert(sizeof want / sizeof *want == sizeof edits / sizeof *edits, "one expectation per edit");
    void *out = run("placement", edits, (int)(sizeof edits / sizeof *edits), in, n, dic, dn, want, &out_n);
    if (!out) return;
    save("place.out.zs", out, out_n);
    // The same edits again on what they made: each finds its work done and skips.
    const RestoreEdit again[] = {edits[0], edits[4], edits[7], edits[11]};
    static const int none[] = {0, 0, 0, 0};
    size_t n2;
    void *twice = run("placement again", again, 4, out, out_n, dic, dn, none, &n2);
    free(twice);
    // Without the dictionaries a packed file cannot be read, so nothing applies.
    void *nodic = run("placement without dictionaries", edits, 1, in, n, NULL, 0, none, &n2);
    free(nodic);
    // A cut frame does not unpack.
    void *cut = run("placement cut", edits, 1, in, n / 2, dic, dn, none, &n2);
    free(cut);
    // The parser on the unpacked placement cut short and with bytes changed: never a crash, and what it reads it
    // writes again and reads back the same.
    size_t raw_n;
    unsigned long long cs = ZSTD_getFrameContentSize(in, n);
    uint8_t *raw = malloc((size_t)cs);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    const char *name;
    size_t at, len;
    CHECK(sarc_at(dic, dn, 1, &name, &at, &len), "zsdic: no second member");
    raw_n = ZSTD_decompress_usingDict(d, raw, (size_t)cs, in, n, dic + at, len);
    ZSTD_freeDCtx(d);
    CHECK(!ZSTD_isError(raw_n) && raw_n == cs, "placement: the fixture does not unpack with zsdic member 1 (%s)",
          name);
    int parsed = 0, damaged = 0;
    uint32_t seed = 12345;
    for (size_t cut_n = 0; cut_n < raw_n; cut_n++) {
        Byml b;
        const char *why;
        if (byml_parse(&b, raw, cut_n, &why)) parsed++, byml_free(&b);
    }
    CHECK(parsed == 0, "placement: %d cut copies parsed", parsed);
    uint8_t *bad = malloc(raw_n);
    for (int round = 0; round < 3000; round++) {
        memcpy(bad, raw, raw_n);
        for (int j = 0; j < 4; j++) {
            seed = seed * 1103515245u + 12345u;
            bad[(seed >> 8) % raw_n] ^= (uint8_t)(1u << (seed & 7));
        }
        Byml b, back;
        const char *why;
        if (!byml_parse(&b, bad, raw_n, &why)) continue;
        damaged++;
        size_t wn;
        uint8_t *w = byml_write(&b, &wn);
        int read = w && byml_parse(&back, w, wn, &why);
        CHECK(read, "damaged placement %d: the rewrite does not parse", round);
        if (read) {
            CHECK(byml_equal(&b.root, &back.root), "damaged placement %d: what was read does not read back the same",
                  round);
            byml_free(&back);
        }
        free(w);
        byml_free(&b);
    }
    printf("parser: %zu cut lengths refused, %d of 3000 damaged copies read and written back the same\n", raw_n,
           damaged);
    engine_damage("placements", edits, (int)(sizeof edits / sizeof *edits), raw, raw_n, &seed);
    free(bad);
    free(raw);
    free(out);
    free(in);
    free(dic);
}

// The placement unpacked (malloc'd): the fixture is packed with the second dictionary of zsdic.sarc.
static uint8_t *placement_raw(const uint8_t *in, size_t n, const uint8_t *dic, size_t dn, size_t *raw_n) {
    const char *name;
    size_t at, len;
    unsigned long long cs = ZSTD_getFrameContentSize(in, n);
    uint8_t *raw = malloc((size_t)cs);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    CHECK(sarc_at(dic, dn, 1, &name, &at, &len), "zsdic: no second member");
    *raw_n = ZSTD_decompress_usingDict(d, raw, (size_t)cs, in, n, dic + at, len);
    ZSTD_freeDCtx(d);
    if (ZSTD_isError(*raw_n) || *raw_n != cs) {
        printf("FAIL: the placement fixture does not unpack\n");
        exit(1);
    }
    return raw;
}

// The kinds that remove, copy (from this file and from another), set text and add keys, on the placement; the script
// checks the tree.
static void placement_kinds(void) {
    size_t n, dn, out_n;
    uint8_t *in = load("place.byml.zs", &n), *dic = load("zsdic.sarc", &dn);
    static const RestoreEdit edits[] = {
        RESTORE_REMOVE(F, "Obj_A", 1001, 1, 2, 3),                               // 0 applies
        RESTORE_REMOVE(F, "Obj_B", 1003, 7, 8, 9),                               // 1 another object under that hash
        RESTORE_REMOVE(F, "Obj_A", 1003, 7, 8, 9.5),                             // 2 elsewhere
        RESTORE_REMOVE(F, "Obj_Z", 4242, 0, 0, 0),                               // 3 not there
        RESTORE_ADD(F, "Obj_B", 1002, 4, 5, 6, 2002, 10, 20, 30, 0, 3, 0),       // 4 applies
        RESTORE_ADD(F, "Obj_B", 1002, 4, 5, 6, 1003, 0, 0, 0, 0, 0, 0),          // 5 the new Hash is taken
        RESTORE_ADD_FROM(F, "test/src.byml", "Obj_S", 5001, 1, 1, 1, 6001, 9, 9, 9, 0, 0.5, 0),   // 6 applies
        RESTORE_SET(F, "Obj_S", 6001, 9, 9, 9, "Dynamic/Room", 24, 17),          // 7 applies, on the copy
        RESTORE_ADD_FROM(F, "test/src.byml", "Obj_S", 5001, 1, 1, 2, 6002, 0, 0, 0, 0, 0, 0),   // 8 elsewhere there
        RESTORE_ADD_FROM(F, "test/none.byml", "Obj_S", 5001, 1, 1, 1, 6003, 0, 0, 0, 0, 0, 0),  // 9 no such file
        RESTORE_ADD_FROM(F, "test/src.byml", "Obj_A", 1001, 1, 2, 3, 6004, 0, 0, 0, 0, 0, 0),   // 10 not placed there
        RESTORE_SET_TEXT(F, "Obj_A", 1003, 7, 8, 9, "Name", NULL, "Added"),      // 11 applies: a new key
        RESTORE_SET_MEMBER_TEXT(F, NULL, "FilePath", "Work/Banc/Scene/Test.bcett.json", "Work/Other.json"),   // 12
        RESTORE_SET_MEMBER_TEXT(F, NULL, "Misc/Unicode", "Ж", "x"),               // 13 not the text expected
        RESTORE_SET_TEXT(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Count", "5", "6"),  // 14 not a text
        RESTORE_SET_NEW(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Extra", BYML_FLOAT, 2.5),   // 15 applies
        RESTORE_SET_NEW(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Count", BYML_INT, 9),       // 16 there already
        RESTORE_SET_NEW(F, "Obj_A", 1003, 7, 8, 9, "Nope/Extra", BYML_INT, 9),          // 17 no dict to add to
        RESTORE_SET_NEW(F, "Obj_A", 1003, 7, 8, 9, "Translate/3", BYML_FLOAT, 1),      // 18 arrays do not grow
        RESTORE_SET_MEMBER_NEW(F, NULL, "Misc/Flag", BYML_BOOL, 1),               // 19 applies, on the root
        RESTORE_SET_TEXT(F, "Obj_A", 1003, 7, 8, 9, "Name", NULL, "Again"),      // 20 there now (11 added it)
        RESTORE_SET_MEMBER_NEW(F, NULL, "Misc/Aaa", BYML_INT, 3),                 // 21 applies: sorts first in Misc
        RESTORE_SET_NEW(F, "Obj_A", 1003, 7, 8, 9, "Dynamic/Text", BYML_STRING, 1),     // 22 a new key's text is SET_TEXT
    };
    static const int want[] = {1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0};
    _Static_assert(sizeof want / sizeof *want == sizeof edits / sizeof *edits, "one expectation per edit");
    void *out = run("placement, other kinds", edits, (int)(sizeof edits / sizeof *edits), in, n, dic, dn, want, &out_n);
    if (out) save("place2.out.zs", out, out_n);
    // The same edits again on what they made: each finds its work done and skips.
    if (out) {
        const RestoreEdit again[] = {edits[0], edits[4], edits[6], edits[11], edits[15], edits[19]};
        static const int none[] = {0, 0, 0, 0, 0, 0};
        size_t n2;
        free(run("placement, other kinds again", again, 6, out, out_n, dic, dn, none, &n2));
    }
    free(out);
    size_t raw_n;
    uint8_t *raw = placement_raw(in, n, dic, dn, &raw_n);
    uint32_t seed = 4321;
    engine_damage("placements (other kinds)", edits, (int)(sizeof edits / sizeof *edits), raw, raw_n, &seed);
    // byml_append takes items into arrays and keyed into dicts only (a hash dict's item would have no hash), and keeps
    // a dict's keys in order.
    Byml pb;
    const char *pwhy;
    if (byml_parse(&pb, raw, raw_n, &pwhy)) {
        BymlNode *hashes = byml_path(&pb.root, "Misc/Hashes"), *misc = byml_get(&pb.root, "Misc");
        BymlNode *flt = byml_path(&pb.root, "Misc/Float"), item = {.type = BYML_INT};
        CHECK(hashes && hashes->type == BYML_HASH32 && !byml_append(hashes, "k", item), "a hash dict took an item");
        CHECK(misc && !byml_append(misc, NULL, item), "a dict took an item without a key");
        CHECK(flt && !byml_append(flt, NULL, item), "a number took an item");
        static const char *const keys[] = {"Zz", "Added", "Int", "A"};   // after the last, inside, a key it has, first
        for (int i = 0; misc && i < 4; i++) CHECK(byml_append(misc, keys[i], item), "a dict refused the key %s", keys[i]);
        for (uint32_t i = 1; misc && i < misc->n; i++)
            CHECK(strcmp(misc->keys[i - 1], misc->keys[i]) < 0, "byml_append: %s before %s", misc->keys[i - 1],
                  misc->keys[i]);
        CHECK(misc && byml_get(misc, "Int") && byml_get(misc, "Int")->type == BYML_INT && byml_get(misc, "Int")->bits == 0,
              "byml_append did not replace the item of a key the dict has");
        byml_free(&pb);
    } else {
        CHECK(0, "placement: the unpacked fixture does not parse (%s)", pwhy);
    }
    free(raw);
    free(in);
    free(dic);
}

// A copy's Hash against the area's other placement file (test/area_Dynamic.bcett.byml.zs places 7001), and removals
// against the area's static compound (test/compound.bphsc holds 1003).
#define AREA "test/area_Static.bcett.byml.zs"
static void area_copies(void) {
    size_t n, dn, out_n;
    uint8_t *in = load("place.byml.zs", &n), *dic = load("zsdic.sarc", &dn);
    static const RestoreEdit edits[] = {
        RESTORE_ADD(AREA, "Obj_B", 1002, 4, 5, 6, 7001, 0, 0, 0, 0, 0, 0),   // 0 the Dynamic file has 7001
        RESTORE_ADD(AREA, "Obj_B", 1002, 4, 5, 6, 7002, 0, 0, 0, 0, 0, 0),   // 1 applies
        RESTORE_ADD(AREA, "Obj_B", 1002, 4, 5, 6, 1001, 0, 0, 0, 0, 0, 0),   // 2 this file has 1001
        RESTORE_REMOVE(AREA, "Obj_A", 1003, 7, 8, 9),                          // 3 the compound holds its collision
        RESTORE_REMOVE(AREA, "Obj_A", 1001, 1, 2, 3),                          // 4 applies
    };
    static const int want[] = {0, 1, 0, 0, 1};
    free(run("placement, copies and removals in an area", edits, 5, in, n, dic, dn, want, &out_n));
    free(in);
    free(dic);
}

#define P "test/pack.zs"
#define ONE "A/one.bgyml"

// sarc_at, sarc_find and sarc_replace on p[0..n), a copy of exactly n bytes (so the sanitizer sees a read past it):
// what they give lies inside, and a replace that succeeds holds the new bytes.
static int sarc_adds;   // copies sarc_add took a new member into

static int sarc_try(const uint8_t *p, size_t n) {
    static uint8_t small[16], big[600];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7), small[i % sizeof small] = (uint8_t)i;
    uint8_t *q = malloc(n ? n : 1);
    memcpy(q, p, n);
    const char *name, *why;
    size_t at, len, out_n;
    for (unsigned i = 0; sarc_at(q, n, i, &name, &at, &len); i++)
        CHECK(at <= n && len <= n - at && strlen(name) < n, "damaged pack: member %u runs outside", i);
    if (sarc_find(q, n, ONE, &at, &len, &why)) CHECK(at <= n && len <= n - at, "damaged pack: " ONE " runs outside");
    int replaced = 0;
    for (int s = 0; s < 2; s++) {
        const uint8_t *data = s ? big : small;
        size_t dn = s ? sizeof big : sizeof small;
        uint8_t *o = sarc_replace(q, n, ONE, data, dn, &out_n, &why);
        if (!o) continue;
        replaced++;
        CHECK(sarc_find(o, out_n, ONE, &at, &len, &why) && len == dn && !memcmp(o + at, data, dn),
              "damaged pack: a replace does not hold the new bytes");
        free(o);
    }
    uint8_t *o = sarc_add(q, n, "N/new.bin", small, sizeof small, 8, &out_n, &why);
    if (o) {
        sarc_adds++;
        CHECK(sarc_find(o, out_n, "N/new.bin", &at, &len, &why) && len == sizeof small && !memcmp(o + at, small, len) &&
                  at % 8 == 0,
              "damaged pack: an add does not hold the new bytes at an 8-aligned offset");
        free(o);
    }
    free(q);
    return replaced;
}

// The pack cut at every length and with bits flipped in its header and node and name tables.
static void sarc_damage(const uint8_t *raw, size_t raw_n) {
    int cut_ok = 0, flipped_ok = 0;
    for (size_t cut_n = 0; cut_n < raw_n; cut_n++) cut_ok += sarc_try(raw, cut_n) > 0;
    int cut_adds = sarc_adds;
    uint8_t *bad = malloc(raw_n);
    uint32_t seed = 777;
    size_t head = raw_n < 0x100 ? raw_n : 0x100;
    for (int round = 0; round < 3000; round++) {
        memcpy(bad, raw, raw_n);
        for (int j = 0; j < 2; j++) {
            seed = seed * 1103515245u + 12345u;
            bad[(seed >> 8) % head] ^= (uint8_t)(1u << (seed & 7));
        }
        flipped_ok += sarc_try(bad, raw_n) > 0;
    }
    free(bad);
    printf("sarc: %d of %zu cut lengths and %d of 3000 damaged copies replaced a member, %d and %d added one\n", cut_ok,
           raw_n, flipped_ok, cut_adds, sarc_adds - cut_adds);
}

static void pack(void) {
    size_t n, out_n;
    uint8_t *in = load("pack.zs", &n);
    static const RestoreEdit edits[] = {
        RESTORE_SET_MEMBER(P, ONE, "MoveSpeed", 0.15, 0.15 * 15.71 / 9.68),   // applies
        RESTORE_SET_MEMBER(P, "C/three.bgyml", "Params/Count", 4, 7),          // applies
        RESTORE_SET_MEMBER(P, "Z/none.bgyml", "X", 1, 2),                      // no such member
        RESTORE_SET_MEMBER(P, "B/two.bin", "X", 1, 2),                         // not a BYML
        RESTORE_SET_MEMBER(P, NULL, "X", 1, 2),                                // the pack itself is not a BYML
    };
    static const int want[] = {1, 1, 0, 0, 0};
    void *out = run("pack", edits, 5, in, n, NULL, 0, want, &out_n);
    if (out) save("pack.out.zs", out, out_n);
    free(out);
    // A member that grows past the next one: sarc_replace moves the ones after it.
    size_t raw_n, grown_n, at, len;
    uint8_t *raw = unzstd(in, n, &raw_n), big[768];
    for (int i = 0; i < 768; i++) big[i] = (uint8_t)i;
    const char *why;
    uint8_t *grown = sarc_replace(raw, raw_n, ONE, big, sizeof big, &grown_n, &why);
    CHECK(grown && sarc_find(grown, grown_n, ONE, &at, &len, &why) && len == sizeof big && !memcmp(grown + at, big, len),
          "grown pack: the member does not hold its new bytes");
    if (grown) save("grown.sarc", grown, grown_n);
    CHECK(!sarc_replace(raw, raw_n, "Z/none", big, 4, &grown_n, &why), "a member the pack lacks was replaced");
    CHECK(!sarc_replace(raw, 0x10, ONE, big, 4, &grown_n, &why), "a cut pack was edited");
    sarc_damage(raw, raw_n);
    free(grown);
    free(raw);
    free(in);
}

// Members added to a pack; the script checks the archive.
static void pack_more(void) {
    size_t n, out_n;
    uint8_t *in = load("pack.zs", &n);
    static const uint8_t declared[] = {'n', 'e', 'w', 0, 1, 2, 3};
    static const RestoreEdit edits[] = {
        RESTORE_ADD_MEMBER(P, "D/new.bin", declared),                            // 0 applies
        RESTORE_COPY_MEMBER(P, "E/copy.bgyml", NULL, "C/three.bgyml"),          // 1 applies, from this pack
        RESTORE_COPY_MEMBER(P, "F/other.bgyml", "test/pack2.zs", "X/x.bgyml"),  // 2 applies, from another
        RESTORE_ADD_MEMBER(P, "A/one.bgyml", declared),                          // 3 the pack has it
        RESTORE_COPY_MEMBER(P, "G/none.bgyml", NULL, "Z/none.bgyml"),           // 4 nothing to copy
        RESTORE_COPY_MEMBER(P, "H/none.bgyml", "test/none.zs", "X/x.bgyml"),    // 5 no such pack
        RESTORE_SET_MEMBER(P, "E/copy.bgyml", "Params/Count", 4, 8),            // 6 applies, on the member added
        RESTORE_SET_MEMBER_TEXT(P, "C/three.bgyml", "Params/Name", NULL, "liftable"),   // 7 applies: a new key
    };
    static const int want[] = {1, 1, 1, 0, 0, 0, 1, 1};
    void *out = run("pack, members added", edits, 8, in, n, NULL, 0, want, &out_n);
    if (out) save("pack2.out.zs", out, out_n);
    // the added members report their sizes; the first edit of each member rewritten reports it before and after (7
    // adds a key: the member grew, which romfs.c raises the member's own resource size entry for)
    CHECK(sizes[1].now && sizes[6].was == sizes[1].now && sizes[6].now >= sizes[6].was, "pack: the added member's sizes");
    CHECK(sizes[7].was && sizes[7].now > sizes[7].was, "pack: the member that grew: %zu -> %zu", sizes[7].was,
          sizes[7].now);
    free(out);
    // Names that outgrow the room before the data: the data moves, every member keeps its alignment.
    size_t raw_n, cur_n;
    uint8_t *raw = unzstd(in, n, &raw_n);
    const char *why;
    for (int i = 0; i < 6; i++) {
        char name[160];
        snprintf(name, sizeof name, "Long/%d/%0120d.bgyml", i, i);
        uint8_t *next = sarc_add(raw, raw_n, name, declared, sizeof declared, i % 2 ? 0x100 : 8, &cur_n, &why);
        CHECK(next != NULL, "long name %d: %s", i, next ? "" : why);
        if (!next) break;
        free(raw);
        raw = next, raw_n = cur_n;
    }
    save("grown2.sarc", raw, raw_n);
    CHECK(!sarc_add(raw, raw_n, "A/one.bgyml", declared, 1, 8, &cur_n, &why), "a member the pack has was added");
    free(raw);
    free(in);
}

static void others(void) {
    size_t n, out_n, dn;
    static const uint8_t be[16] = {'B', 'Y', 0, 7};
    static const RestoreEdit set[] = {RESTORE_SET_MEMBER("test/be.byml", NULL, "Root/Value", 1.25, -2)};
    static const int no[] = {0};
    free(run("big-endian", set, 1, be, sizeof be, NULL, 0, no, &out_n));
    static const RestoreEdit scale[] = {RESTORE_SET("test/x", "Obj_A", 1001, 1, 2, 3, "Scale/1", 2, 5)};
    uint8_t *junk = load("junk.bin", &n), *other = load("otherdic.zs", &n), *dic = load("zsdic.sarc", &dn);
    free(run("not a BYML", scale, 1, junk, 64, NULL, 0, no, &out_n));
    free(run("a dictionary Pack/ZsDic.pack.zs lacks", scale, 1, other, n, dic, dn, no, &out_n));
    free(run("an empty file", scale, 1, junk, 0, NULL, 0, no, &out_n));
    free(junk);
    free(other);
    free(dic);
}

static void put32le(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// A BYML file whose root is a dict (or a hash dict) of n floats 1.0, keyed "k000000", "k000001", ... (hashes 16 i + 7).
// mode 0: in order; 1: in reverse order; 2: item 1 has item 0's key (another entry of the key table with the same text)
// or hash. malloc'd.
static uint8_t *dict_file(uint8_t type, uint32_t n, int mode, size_t *size) {
    size_t keys = type == BYML_DICT ? 4 + 4 * ((size_t)n + 1) + 8 * (size_t)n : 0, root = 16 + keys;
    *size = root + 4 + (type == BYML_DICT ? 8 * (size_t)n : 8 * (size_t)n + ((size_t)n + 3) / 4 * 4);
    uint8_t *p = calloc(1, *size);
    memcpy(p, "YB\x07\x00", 4);
    put32le(p + 4, keys ? 16 : 0);
    put32le(p + 12, (uint32_t)root);
    if (keys) {
        put32le(p + 16, 0xc2 | n << 8);
        for (uint32_t i = 0; i <= n; i++) put32le(p + 20 + 4 * (size_t)i, 4 + 4 * (n + 1) + 8 * i);
        for (uint32_t i = 0; i < n; i++)
            snprintf((char *)p + 20 + 4 * ((size_t)n + 1) + 8 * (size_t)i, 8, "k%06u", mode == 2 && i == 1 ? 0 : i);
    }
    put32le(p + root, type | n << 8);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = mode == 1 ? n - 1 - i : i, h = 16 * (mode == 2 && i == 1 ? 0 : k) + 7;
        put32le(p + root + 4 + 8 * (size_t)i, type == BYML_DICT ? k | 0xd2u << 24 : h);
        put32le(p + root + 8 + 8 * (size_t)i, 0x3f800000);
        if (type == BYML_HASH32) p[root + 4 + 8 * (size_t)n + i] = BYML_FLOAT;
    }
    return p;
}

// Dicts out of order or with a key twice, as only a broken or hostile file has them (the game looks keys up sorted):
// the parser refuses them, so the engine's read-back check compares dicts item by item. A dict of 40,000 keys in
// reverse order is refused when it is parsed, before any edit; the same dict in order is edited.
static void order(void) {
    static const struct { uint8_t type; int mode; const char *why; } cases[] = {
        {BYML_DICT, 0, NULL}, {BYML_DICT, 1, "a dict's keys are not in order"},
        {BYML_DICT, 2, "a dict's keys are not in order"}, {BYML_HASH32, 0, NULL},
        {BYML_HASH32, 1, "a hash dict's hashes are not in order"}, {BYML_HASH32, 2, "a hash dict's hashes are not in order"},
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        size_t n;
        uint8_t *p = dict_file(cases[c].type, 8, cases[c].mode, &n);
        Byml b;
        const char *why = NULL;
        int ok = byml_parse(&b, p, n, &why);
        CHECK(cases[c].why ? !ok && !strcmp(why, cases[c].why) : ok, "order case %zu: %s", c, ok ? "parsed" : why);
        if (ok) byml_free(&b);
        free(p);
    }
    static const RestoreEdit set[] = {RESTORE_SET_MEMBER("test/dict.byml", NULL, "k000000", 1, 2)};
    const RestoreEdit *ptr[] = {&set[0]};
    for (int mode = 0; mode < 2; mode++) {
        size_t n, out_n;
        int ok;
        uint8_t *p = dict_file(BYML_DICT, 40000, mode, &n);
        clock_t t0 = clock();
        void *out = restore_file(ptr, 1, p, n, NULL, 0, NULL, NULL, &ok, NULL, note, mode ? "reversed dict" : "sorted dict",
                                 &out_n);
        double ms = (double)(clock() - t0) * 1000 / CLOCKS_PER_SEC;
        CHECK(mode ? !out && !ok : out && ok, "a dict of 40000 keys %s", mode ? "in reverse order was edited" : "was not edited");
        printf("order: a dict of 40000 keys %s in %.0f ms\n", mode ? "in reverse order refused" : "in order edited", ms);
        free(out);
        free(p);
    }
}

// ---- restore.c: the registry, the switches, a start's reads and serves and --probe-restore, with what it calls of the
// runtime stubbed here (the settings, the log, the RomFS)

static const RestoreEdit t_place[] = {
    RESTORE_REMOVE(F, "Obj_A", 1001, 1, 2, 3),
    RESTORE_SET(F, "Obj_B", 1002, 4, 5, 6, "Rotate/1", 1.5, 3),
};
static const RestoreEdit t_pack[] = {RESTORE_SET_MEMBER(P, ONE, "MoveSpeed", 0.15, 0.2)};
static const RestoreEdit t_missing[] = {RESTORE_SET("test/missing.bcett.byml", "Obj_A", 1001, 1, 2, 3, "Scale/1", 2, 5)};
RESTORATION(t_place, "TEST_PLACE", "Place", "Two edits of the placement.", t_place);
RESTORATION(t_pack, "TEST_PACK", "Pack", "An edit of a pack member.", t_pack);
RESTORATION(t_missing, "TEST_MISSING", "Missing", "An edit of a file the RomFS lacks.", t_missing);

static struct { char name[64], value[8]; } settings[8];
static int nsettings, logged, reads, serves, serve_fails;

const char *cfg_get(const char *name) {
    for (int i = 0; i < nsettings; i++)
        if (!strcmp(settings[i].name, name)) return settings[i].value;
    return NULL;
}

void cfg_set(const char *name, const char *value) {
    int i = 0;
    while (i < nsettings && strcmp(settings[i].name, name)) i++;
    if (i == (int)(sizeof settings / sizeof *settings)) return;
    if (i == nsettings) nsettings++;
    snprintf(settings[i].name, sizeof settings[i].name, "%s", name);
    snprintf(settings[i].value, sizeof settings[i].value, "%s", value);
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    logged++;
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL rt_fatal: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg) {
    if (o->state != 2) o->state = 2, fn(arg);
}

uint64_t plat_now_ns(void) { return 0; }

void fs_init(void) {}

// The RomFS: the fixtures at their test paths, and Pack/ZsDic.pack.zs packed as the game's is.
static void *t_read(const char *rel, size_t *n) {
    reads++;
    if (!strcmp(rel, "Pack/ZsDic.pack.zs")) {
        size_t dn;
        uint8_t *d = load("zsdic.sarc", &dn), *z = malloc(ZSTD_compressBound(dn));
        *n = ZSTD_compress(z, ZSTD_compressBound(dn), d, dn, 3);
        free(d);
        return z;
    }
    if (!strcmp(rel, F)) return load("place.byml.zs", n);
    if (!strcmp(rel, P)) return load("pack.zs", n);
    return NULL;
}

void *romfs_read_served(const char *rel, size_t *n) { return t_read(rel, n); }

const char *romfs_with(const char *prefix, int i) { return list(prefix, i); }

static int t_serve(const char *rel, void *out, size_t n) {
    (void)rel, (void)n;
    serves++;
    free(out);
    return !serve_fails;
}

static const Restoration *by_key(const char *key) {
    for (int i = 0; i < restore_count(); i++)
        if (!strcmp(restore_at(i)->key, key)) return restore_at(i);
    return NULL;
}

static void switches(void) {
    printf("switches\n");
    CHECK(restore_count() == 3 && !strcmp(restore_at(0)->key, "TEST_MISSING") &&
              !strcmp(restore_at(2)->key, "TEST_PLACE") && !restore_at(3),
          "the registry: %d restorations, not the three sorted by key", restore_count());
    const Restoration *place = by_key("TEST_PLACE"), *pk = by_key("TEST_PACK"), *missing = by_key("TEST_MISSING");
    if (!place || !pk || !missing) return;
    int a, s;
    // Every switch off (unset, "0" and ""): nothing read, served or logged.
    cfg_set("NX2NAVI_RESTORE_TEST_PACK", "0");
    cfg_set("NX2NAVI_RESTORE_TEST_MISSING", "");
    CHECK(!restore_on(place) && !restore_on(pk) && !restore_on(missing), "every switch off: one reads on");
    restore_apply(t_read, t_serve);
    CHECK(!reads && !serves && !logged, "every switch off: %d reads, %d serves, %d log lines", reads, serves, logged);
    restore_started(place, &a, &s);
    CHECK(!a && !s, "every switch off: TEST_PLACE counts %d applied, %d skipped", a, s);
    // Two on: the placement's edits apply and its file is served; the missing file's edit is skipped.
    restore_set(place, 1);
    restore_set(missing, 1);
    CHECK(restore_on(place) && restore_on(missing) && !restore_on(pk), "restore_set does not switch");
    restore_apply(t_read, t_serve);
    restore_started(place, &a, &s);
    CHECK(a == 2 && !s, "TEST_PLACE: %d applied, %d skipped", a, s);
    restore_started(missing, &a, &s);
    CHECK(!a && s == 1, "TEST_MISSING: %d applied, %d skipped", a, s);
    restore_started(pk, &a, &s);
    CHECK(!a && !s, "TEST_PACK (off): %d applied, %d skipped", a, s);
    CHECK(serves == 1 && logged > 0, "two on: %d files served, %d log lines", serves, logged);
    // The edited file cannot be served: its edits are undone and count as skipped (the counts are of the last run).
    serve_fails = 1;
    restore_apply(t_read, t_serve);
    restore_started(place, &a, &s);
    CHECK(!a && s == 2 && serves == 2, "TEST_PLACE not served: %d applied, %d skipped, %d serves", a, s, serves);
    // The pack members a start rewrote are of that start alone: listed with the pack's edit on, gone with it off.
    const char *mp;
    size_t mw, mn;
    int md;
    CHECK(!restore_member(0, &mp, &mw, &mn, &md), "a start without the pack's edit lists a member");
    serve_fails = 0;
    restore_set(pk, 1);
    restore_apply(t_read, t_serve);
    CHECK(serves == 4 && restore_member(0, &mp, &mw, &mn, &md) && !strcmp(mp, ONE) && mw && mn && !md &&
              !restore_member(1, &mp, &mw, &mn, &md),
          "TEST_PACK on: %d serves, the member it rewrote is not the one member listed", serves);
    restore_set(pk, 0);
    restore_apply(t_read, t_serve);
    CHECK(serves == 5 && !restore_member(0, &mp, &mw, &mn, &md), "TEST_PACK off again: the start still lists a member");
    // --probe-restore: every restoration, switched on or not, nothing served; 1 as the missing file's edit is skipped.
    CHECK(restore_probe() == 1 && serves == 5, "probe: did not fail on the skipped edit, or served a file");
    restore_apply(t_read, t_serve);
    CHECK(serves == 5, "a start after the probe served a file");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: restore_check <folder of tests/restore_check.py's fixtures>\n");
        return 2;
    }
    dir = argv[1];
    placement();
    placement_kinds();
    area_copies();
    pack();
    pack_more();
    others();
    order();
    switches();
    printf("restore_check.c: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails ? 1 : 0;
}
