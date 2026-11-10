// The edit engine of the restorations (restore.h restore_file): one game file unpacked (a zstd frame, with the
// dictionary of its id), its BYML parsed (or the BYML members a pack's edits name), the edits applied to the tree,
// written back, read back to check it holds what was meant, and packed again as the original was. An edit applies
// only when what it names is there as declared: the object by Hash, Gyaml and Translate, the number it sets at the
// value the game ships.
//
// Each document (the file, or each pack member some edit names) is checked on its own: one that does not read back as
// edited, or a member that cannot be put back into its pack, loses only its own edits, and the file is served with the
// others. When the file itself cannot be unpacked or packed again, none of its edits apply.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "byml.h"
#include "restore.h"
#include "sarc.h"

#define MAX_RAW (256u << 20)
#define LEVEL 3   // zstd level of the files served: the game unpacks any level, and start-up time is what counts

typedef struct {
    void (*fn)(void *ctx, int k, const char *text);
    void *ctx;
} Notes;

static void say(const Notes *n, int k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(const Notes *n, int k, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n->fn) n->fn(n->ctx, k, line);
}

// The dictionary with id among the members of dic (Pack/ZsDic.pack.zs unpacked); NULL when none has it.
static const uint8_t *dictionary(const void *dic, size_t dic_n, unsigned id, size_t *size) {
    size_t at, n;
    for (unsigned i = 0; dic && sarc_at(dic, dic_n, i, NULL, &at, &n); i++)
        if (ZSTD_getDictID_fromDict((const uint8_t *)dic + at, n) == id) {
            *size = n;
            return (const uint8_t *)dic + at;
        }
    return NULL;
}

// ---- one edit on a parsed document

static int find_actor(const BymlNode *actors, uint64_t hash, uint32_t *at) {
    for (uint32_t i = 0; i < actors->n; i++) {
        const BymlNode *h = byml_get(&actors->items[i], "Hash");
        if (h && (h->type == BYML_UINT64 || h->type == BYML_INT64) && h->bits == hash) {
            *at = i;
            return 1;
        }
    }
    return 0;
}

// The three numbers of the array at key (Translate); 0, 0, 0 when the object has none (the game's default).
static void vec_of(const BymlNode *obj, const char *key, double v[3]) {
    const BymlNode *a = byml_get(obj, key);
    for (int i = 0; i < 3; i++) v[i] = !a ? 0 : a->type == BYML_ARRAY && a->n == 3 ? byml_number(&a->items[i]) : NAN;
}

static int near(double a, double b, double tol) { return fabs(a - b) <= tol * (fabs(b) > 1 ? fabs(b) : 1); }

static int apply_one(Byml *b, const RestoreEdit *e, int k, const Notes *nt) {
    BymlNode *obj = &b->root;
    unsigned long long hash = (unsigned long long)e->hash;
    if (e->hash) {
        BymlNode *actors = byml_get(&b->root, "Actors");
        uint32_t idx;
        if (!actors || actors->type != BYML_ARRAY) {
            say(nt, k, "skipped: the file has no Actors list");
            return 0;
        }
        if (!find_actor(actors, e->hash, &idx)) {
            say(nt, k, "skipped: no object %s %llu (another game version, or a mod moved it)", e->gyaml, hash);
            return 0;
        }
        obj = &actors->items[idx];
        const BymlNode *g = byml_get(obj, "Gyaml");
        if (e->gyaml && (!g || g->type != BYML_STRING || strcmp(g->str, e->gyaml))) {
            say(nt, k, "skipped: object %llu is %s, not %s", hash, g && g->type == BYML_STRING ? g->str : "unnamed",
                e->gyaml);
            return 0;
        }
        double t[3];
        vec_of(obj, "Translate", t);
        if (!(fabs(t[0] - e->at[0]) <= 0.01 && fabs(t[1] - e->at[1]) <= 0.01 && fabs(t[2] - e->at[2]) <= 0.01)) {
            say(nt, k, "skipped: %s %llu is at (%.2f, %.2f, %.2f), not (%.2f, %.2f, %.2f)", e->gyaml, hash, t[0], t[1],
                t[2], e->at[0], e->at[1], e->at[2]);
            return 0;
        }
    }
    BymlNode *v = e->path ? byml_path(obj, e->path) : NULL;
    double cur = v ? byml_number(v) : NAN;
    if (!v || isnan(cur)) {
        say(nt, k, "skipped: %s %s", e->path ? e->path : "(no path)", v ? "is not a number" : "is not there");
        return 0;
    }
    if (!near(cur, e->was, 1e-5)) {
        if (near(cur, e->now, 1e-5)) say(nt, k, "skipped: %s is already %g", e->path, cur);
        else say(nt, k, "skipped: %s is %g, not %g", e->path, cur, e->was);
        return 0;
    }
    if (!byml_set_number(v, e->now)) {
        say(nt, k, "skipped: %g does not fit %s", e->now, e->path);
        return 0;
    }
    say(nt, k, "%s%s%s %g -> %g", e->hash ? e->gyaml : "", e->hash ? " " : "", e->path, cur, e->now);
    return 1;
}

// The edits idx[0..cnt) of one document doc[0..n) (a BYML file, or a pack's BYML member `what`); the document
// rewritten (malloc'd) when one applied, else NULL.
static uint8_t *edit_doc(const RestoreEdit *const *edits, const int *idx, int cnt, const char *what,
                         const uint8_t *doc, size_t n, int *ok, const Notes *nt, size_t *out_n) {
    Byml b, back;
    const char *why;
    if (!byml_parse(&b, doc, n, &why)) {
        for (int j = 0; j < cnt; j++) say(nt, idx[j], "skipped: %s: %s", what, why);
        return NULL;
    }
    int any = 0;
    for (int j = 0; j < cnt; j++) any |= ok[idx[j]] = apply_one(&b, edits[idx[j]], idx[j], nt);
    uint8_t *out = any ? byml_write(&b, out_n) : NULL;
    int same = out && byml_parse(&back, out, *out_n, &why);
    if (same) {
        same = byml_equal(&back.root, &b.root);
        byml_free(&back);
    }
    if (any && !same) {
        for (int j = 0; j < cnt; j++)
            if (ok[idx[j]]) say(nt, idx[j], "undone: %s does not read back as edited", what);
        for (int j = 0; j < cnt; j++) ok[idx[j]] = 0;
        free(out);
        out = NULL;
    }
    byml_free(&b);
    return out;
}

static int same_member(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

void *restore_file(const RestoreEdit *const *edits, int n, const void *in, size_t size, const void *dic, size_t dic_n,
                   int *ok, void (*note)(void *ctx, int k, const char *text), void *ctx, size_t *out_n) {
    Notes nt = {note, ctx};
    const uint8_t *p = in;
    const uint8_t *dict = NULL;
    size_t rn = size, dict_n = 0;
    uint8_t *raw = NULL, *out = NULL;
    int *idx = malloc(sizeof *idx * (n ? n : 1)), *done = calloc(n ? n : 1, sizeof *done);
    int packed = size >= 4 && !memcmp(p, "\x28\xb5\x2f\xfd", 4), any = 0;
    size_t raw_was = 0;
    const char *why = NULL;
    for (int k = 0; k < n; k++) ok[k] = 0;
    if (!idx || !done) {
        why = "out of memory";
    } else if (packed) {
        unsigned long long cs = ZSTD_getFrameContentSize(p, size);
        unsigned id = ZSTD_getDictID_fromFrame(p, size);
        if (id && !(dict = dictionary(dic, dic_n, id, &dict_n))) why = "its zstd dictionary is not in Pack/ZsDic.pack.zs";
        else if (cs == ZSTD_CONTENTSIZE_UNKNOWN || cs == ZSTD_CONTENTSIZE_ERROR || cs > MAX_RAW)
            why = "not a zstd frame with a size";
        else if (!(raw = malloc(cs ? (size_t)cs : 1))) why = "out of memory";
        else {
            ZSTD_DCtx *d = ZSTD_createDCtx();
            size_t got = d ? ZSTD_decompress_usingDict(d, raw, (size_t)cs, p, size, dict, dict_n) : 0;
            ZSTD_freeDCtx(d);
            if (ZSTD_isError(got) || got != cs) why = "its zstd frame does not unpack";
            rn = (size_t)cs;
        }
    } else if (!(raw = malloc(size ? size : 1))) {
        why = "out of memory";
    } else {
        memcpy(raw, p, size);
    }
    if (why) {
        for (int k = 0; k < n; k++) say(&nt, k, "skipped: %s", why);
        goto done;
    }
    raw_was = rn;
    // One document at a time: the file itself, or each pack member some edit names, in the order the edits name them.
    for (int k = 0; k < n; k++) {
        if (done[k]) continue;
        const char *m = edits[k]->member;
        int cnt = 0;
        for (int j = k; j < n; j++)
            if (!done[j] && same_member(edits[j]->member, m)) idx[cnt++] = j, done[j] = 1;
        size_t at = 0, len = rn, doc_n;
        if (m && !sarc_find(raw, rn, m, &at, &len, &why)) {
            for (int j = 0; j < cnt; j++) say(&nt, idx[j], "skipped: %s: %s", m, why);
            continue;
        }
        uint8_t *doc = edit_doc(edits, idx, cnt, m ? m : "the file", raw + at, len, ok, &nt, &doc_n);
        if (!doc) continue;
        uint8_t *next = doc;
        size_t next_n = doc_n;
        if (m) {
            next = sarc_replace(raw, rn, m, doc, doc_n, &next_n, &why);
            free(doc);
            if (!next) {
                for (int j = 0; j < cnt; j++)
                    if (ok[idx[j]]) say(&nt, idx[j], "undone: %s", why);
                for (int j = 0; j < cnt; j++) ok[idx[j]] = 0;
                continue;
            }
        }
        free(raw);
        raw = next, rn = next_n;
    }
    for (int k = 0; k < n; k++) any |= ok[k];
    if (!any) goto done;
    if (!packed) {
        out = raw, *out_n = rn;
        raw = NULL;
    } else {
        size_t cap = ZSTD_compressBound(rn), zn = 0;
        ZSTD_CCtx *c = ZSTD_createCCtx();
        out = c ? malloc(cap) : NULL;
        if (out) zn = ZSTD_compress_usingDict(c, out, cap, raw, rn, dict, dict_n, LEVEL);
        ZSTD_freeCCtx(c);
        // the frame as the game will unpack it: the same dictionary id and size, the same bytes
        uint8_t *check = out && !ZSTD_isError(zn) ? malloc(rn ? rn : 1) : NULL;
        ZSTD_DCtx *d = check ? ZSTD_createDCtx() : NULL;
        size_t got = d ? ZSTD_decompress_usingDict(d, check, rn, out, zn, dict, dict_n) : 0;
        int good = d && !ZSTD_isError(got) && got == rn && !memcmp(check, raw, rn) &&
                   ZSTD_getDictID_fromFrame(out, zn) == ZSTD_getDictID_fromFrame(p, size) &&
                   ZSTD_getFrameContentSize(out, zn) == rn;
        ZSTD_freeDCtx(d);
        free(check);
        if (!good) {
            for (int k = 0; k < n; k++)
                if (ok[k]) say(&nt, k, "undone: the edited file does not pack again");
            for (int k = 0; k < n; k++) ok[k] = 0;
            free(out);
            out = NULL;
            goto done;
        }
        *out_n = zn;
    }
    say(&nt, -1, "%zu bytes unpacked, %zu after the edits", raw_was, rn);
done:
    free(raw);
    free(idx);
    free(done);
    return out;
}
