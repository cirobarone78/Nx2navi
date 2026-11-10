// The edit engine of the restorations (restore.h restore_file): one game file unpacked (a zstd frame, with the
// dictionary of its id), the members a pack gains added, its BYML parsed (or the BYML members a pack's edits name),
// the edits applied to the tree, written back, read back to check it holds what was meant, and packed again as the
// original was. An edit applies only when what it names is there as declared: the object by Hash, Gyaml and Translate
// (in the file that places it, for a copy from another file), the value it changes at the value the game ships, the
// key it adds missing, the member it adds missing.
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
#define LEVEL 3        // zstd level of the files served: the game unpacks any level, and start-up time is what counts
#define MAX_SOURCES 8  // other files one call copies from

typedef struct {
    char *rel;
    uint8_t *raw;      // unpacked
    size_t n;
    Byml doc;          // parsed when an ADD copies from it
    int parsed;
    const char *why;   // NULL: raw is there
} Source;

typedef struct {
    void (*fn)(void *ctx, int k, const char *text);
    void *ctx;
    const void *dic;
    size_t dic_n;
    void *(*fetch)(const char *rel, size_t *n);
    Source src[MAX_SOURCES];
    int nsrc;
    const char *(*list)(const char *prefix, int i);
    uint8_t *compound;   // the area's static compound files, unpacked and joined (compound_of)
    size_t compound_n;
    int compound_read;
} Ctx;

static void say(const Ctx *c, int k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(const Ctx *c, int k, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (c->fn) c->fn(c->ctx, k, line);
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

// p[0..size) as the game unpacks it (malloc'd; a copy when it is not a zstd frame), with the dictionary its frame
// names (*dict, NULL: none); NULL with why.
static uint8_t *unpack(const Ctx *c, const uint8_t *p, size_t size, size_t *rn, const uint8_t **dict, size_t *dict_n,
                       const char **why) {
    uint8_t *raw = NULL;
    *dict = NULL, *dict_n = 0;
    if (size < 4 || memcmp(p, "\x28\xb5\x2f\xfd", 4)) {
        if ((raw = malloc(size ? size : 1))) memcpy(raw, p, size), *rn = size;
        else *why = "out of memory";
        return raw;
    }
    unsigned long long cs = ZSTD_getFrameContentSize(p, size);
    unsigned id = ZSTD_getDictID_fromFrame(p, size);
    if (id && !(*dict = dictionary(c->dic, c->dic_n, id, dict_n)))
        *why = "its zstd dictionary is not in Pack/ZsDic.pack.zs";
    else if (cs == ZSTD_CONTENTSIZE_UNKNOWN || cs == ZSTD_CONTENTSIZE_ERROR || cs > MAX_RAW)
        *why = "not a zstd frame with a size";
    else if (!(raw = malloc(cs ? (size_t)cs : 1))) *why = "out of memory";
    else {
        ZSTD_DCtx *d = ZSTD_createDCtx();
        size_t got = d ? ZSTD_decompress_usingDict(d, raw, (size_t)cs, p, size, *dict, *dict_n) : 0;
        ZSTD_freeDCtx(d);
        if (ZSTD_isError(got) || got != cs) {
            *why = "its zstd frame does not unpack";
            free(raw);
            return NULL;
        }
        *rn = (size_t)cs;
    }
    return raw;
}

// Another file, unpacked, kept until the call ends (strings of copied objects point into it); NULL with why.
static Source *source(Ctx *c, const char *rel, const char **why) {
    for (int i = 0; i < c->nsrc; i++)
        if (!strcmp(c->src[i].rel, rel)) return c->src[i].why ? (*why = c->src[i].why, NULL) : &c->src[i];
    if (c->nsrc == MAX_SOURCES) return *why = "more source files than one file's edits may copy from", NULL;
    Source *s = &c->src[c->nsrc];
    memset(s, 0, sizeof *s);
    if (!(s->rel = strdup(rel))) return *why = "out of memory", NULL;
    c->nsrc++;
    size_t n = 0, dn;
    const uint8_t *dict;
    void *in = c->fetch ? c->fetch(rel, &n) : NULL;
    if (!in) s->why = "the file is missing";
    else s->raw = unpack(c, in, n, &s->n, &dict, &dn, &s->why);
    free(in);
    if (s->why) return *why = s->why, NULL;
    return s;
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

// The three numbers of the array at key (Translate, Rotate); 0, 0, 0 when the object has none (the game's default).
static void vec_of(const BymlNode *obj, const char *key, double v[3]) {
    const BymlNode *a = byml_get(obj, key);
    for (int i = 0; i < 3; i++) v[i] = !a ? 0 : a->type == BYML_ARRAY && a->n == 3 ? byml_number(&a->items[i]) : NAN;
}

// key set to {v[0], v[1], v[2]}: in place when it is an array of three numbers, else as a new array of floats.
static int set_vec(BymlNode *obj, const char *key, const float v[3]) {
    BymlNode *a = byml_get(obj, key);
    if (a && a->type == BYML_ARRAY && a->n == 3 && !isnan(byml_number(&a->items[0])) &&
        !isnan(byml_number(&a->items[1])) && !isnan(byml_number(&a->items[2])))
        return byml_set_number(&a->items[0], v[0]) && byml_set_number(&a->items[1], v[1]) &&
               byml_set_number(&a->items[2], v[2]);
    BymlNode arr = {.type = BYML_ARRAY};
    for (int i = 0; i < 3; i++) {
        BymlNode f = {.type = BYML_FLOAT};
        byml_set_number(&f, v[i]);
        if (!byml_append(&arr, NULL, f)) {
            byml_node_free(&arr);
            return 0;
        }
    }
    if (byml_append(obj, key, arr)) return 1;
    byml_node_free(&arr);
    return 0;
}

static void drop_key(BymlNode *obj, const char *key) {
    for (uint32_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) {
            byml_remove(obj, i);
            return;
        }
}

static int near(double a, double b, double tol) { return fabs(a - b) <= tol * (fabs(b) > 1 ? fabs(b) : 1); }

// The area's other placement file: X_Static.bcett... for X_Dynamic.bcett... and the reverse; 0 for any other file.
static int other_placement(const char *file, char *out, size_t n) {
    static const char *const pair[2] = {"_Static.bcett.", "_Dynamic.bcett."};
    for (int i = 0; i < 2; i++) {
        const char *at = strstr(file, pair[i]);
        if (at && snprintf(out, n, "%.*s%s%s", (int)(at - file), file, pair[!i], at + strlen(pair[i])) < (int)n)
            return 1;
    }
    return 0;
}

// 1 when the area's other placement file places an object with this Hash (the game reads both files of an area into
// one set of objects, named by Hash): a copy must take a Hash neither file uses.
static int area_has(Ctx *c, const RestoreEdit *e, uint64_t hash) {
    char rel[512];
    const char *why;
    uint32_t at;
    Source *s = other_placement(e->file, rel, sizeof rel) ? source(c, rel, &why) : NULL;
    if (!s || (!s->parsed && !byml_parse(&s->doc, s->raw, s->n, &why))) return 0;   // no such file: nothing to clash
    s->parsed = 1;
    const BymlNode *actors = byml_get(&s->doc.root, "Actors");
    return actors && actors->type == BYML_ARRAY && find_actor(actors, hash, &at);
}

// The area's static compound collision, Phive/StaticCompoundBody/<folder>/<area>_X<x>_Z<z>...bphsc for
// Banc/<folder>/<area>_Static or _Dynamic, unpacked and joined (kept until the call ends); NULL when there is none.
static const uint8_t *compound_of(Ctx *c, const char *file, size_t *n) {
    if (!c->compound_read) {
        c->compound_read = 1;
        const char *slash = strrchr(file, '/'), *cut = strstr(file, "_Static.bcett.");
        if (!cut) cut = strstr(file, "_Dynamic.bcett.");
        if (c->list && c->fetch && slash && cut && cut > slash) {
            const char *folder = slash, *rel;
            while (folder > file && folder[-1] != '/') folder--;
            char prefix[512];
            snprintf(prefix, sizeof prefix, "Phive/StaticCompoundBody/%.*s/%.*s_X", (int)(slash - folder), folder,
                     (int)(cut - slash - 1), slash + 1);
            for (int i = 0; (rel = c->list(prefix, i)); i++) {
                size_t zn = 0, rn = 0, dn;
                const uint8_t *dict;
                const char *why = NULL;
                uint8_t *z = c->fetch(rel, &zn), *raw = z ? unpack(c, z, zn, &rn, &dict, &dn, &why) : NULL;
                uint8_t *grown = raw ? realloc(c->compound, c->compound_n + rn) : NULL;
                if (grown) memcpy(grown + c->compound_n, raw, rn), c->compound = grown, c->compound_n += rn;
                free(z);
                free(raw);
            }
        }
    }
    *n = c->compound_n;
    return c->compound;
}

// 1 when the area's static compound holds the object of this Hash: its collision is baked there and stays where it was
// when the placement no longer has it (seen: a crate removed from HyruleCastle_Static vanished, but Link still stopped
// at its face).
static int compound_holds(Ctx *c, const char *file, uint64_t hash) {
    size_t n;
    const uint8_t *p = compound_of(c, file, &n);
    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = (uint8_t)(hash >> 8 * i);   // stored little-endian
    for (size_t i = 0; p && i + 8 <= n; i++)
        if (p[i] == key[0] && !memcmp(p + i, key, 8)) return 1;
    return 0;
}

// The object e names in doc (where: "the file", or the file it copies from): its node, and the Actors list and its
// index there; NULL after saying why.
static BymlNode *find_obj(const Ctx *c, Byml *doc, const RestoreEdit *e, int k, const char *where, BymlNode **actors,
                          uint32_t *idx) {
    unsigned long long hash = (unsigned long long)e->hash;
    *actors = byml_get(&doc->root, "Actors");
    if (!*actors || (*actors)->type != BYML_ARRAY) {
        say(c, k, "skipped: %s has no Actors list", where);
        return NULL;
    }
    if (!find_actor(*actors, e->hash, idx)) {
        say(c, k, "skipped: %s has no object %s %llu (another game version, or a mod moved it)", where, e->gyaml, hash);
        return NULL;
    }
    BymlNode *obj = &(*actors)->items[*idx];
    const BymlNode *g = byml_get(obj, "Gyaml");
    if (e->gyaml && (!g || g->type != BYML_STRING || strcmp(g->str, e->gyaml))) {
        say(c, k, "skipped: object %llu is %s, not %s", hash, g && g->type == BYML_STRING ? g->str : "unnamed",
            e->gyaml);
        return NULL;
    }
    double t[3];
    vec_of(obj, "Translate", t);
    if (!(fabs(t[0] - e->at[0]) <= 0.01 && fabs(t[1] - e->at[1]) <= 0.01 && fabs(t[2] - e->at[2]) <= 0.01)) {
        say(c, k, "skipped: %s %llu is at (%.2f, %.2f, %.2f), not (%.2f, %.2f, %.2f)", e->gyaml, hash, t[0], t[1], t[2],
            e->at[0], e->at[1], e->at[2]);
        return NULL;
    }
    return obj;
}

// The dict path's last '/' names below obj and the key after it (a pointer into path); NULL when it is not a dict.
static BymlNode *parent_of(BymlNode *obj, const char *path, const char **key) {
    const char *slash = strrchr(path, '/');
    *key = slash ? slash + 1 : path;
    BymlNode *p = obj;
    if (slash) {
        char head[512];
        if ((size_t)(slash - path) >= sizeof head) return NULL;
        memcpy(head, path, (size_t)(slash - path));
        head[slash - path] = 0;
        p = byml_path(obj, head);
    }
    return p && p->type == BYML_DICT && **key ? p : NULL;
}

// A key the edit adds, missing so far: node goes into the dict above it. 0 after saying why.
static int add_key(const Ctx *c, int k, BymlNode *obj, const char *path, BymlNode node) {
    const char *key;
    BymlNode *parent = parent_of(obj, path, &key);
    if (!parent) {
        say(c, k, "skipped: %s: the dict to add the key to is not there", path);
        return 0;
    }
    if (!byml_append(parent, key, node)) {
        say(c, k, "skipped: out of memory");
        return 0;
    }
    return 1;
}

static int apply_one(Ctx *c, Byml *b, const RestoreEdit *e, int k) {
    BymlNode *obj = &b->root, *actors = NULL;
    uint32_t idx = 0;
    unsigned long long hash = (unsigned long long)e->hash;
    int elsewhere = e->kind == RESTORE_KIND_ADD && e->from;   // the object is found in the file it is copied from
    if (e->hash && !elsewhere && !(obj = find_obj(c, b, e, k, "the file", &actors, &idx))) return 0;
    switch (e->kind) {
    case RESTORE_KIND_REMOVE:
        if (!actors) break;
        if (compound_holds(c, e->file, e->hash)) {
            say(c, k, "skipped: the area's static compound (Phive/StaticCompoundBody) holds %s %llu: removed from the "
                      "placement it would vanish but its collision would stay",
                e->gyaml, hash);
            return 0;
        }
        byml_remove(actors, idx);
        say(c, k, "removed %s %llu at (%.2f, %.2f, %.2f)", e->gyaml, hash, e->at[0], e->at[1], e->at[2]);
        return 1;
    case RESTORE_KIND_ADD: {
        uint32_t there;
        BymlNode *mine = byml_get(&b->root, "Actors");
        if (!e->hash) break;
        if (e->from) {   // the object to copy is in another placement file
            const char *why;
            BymlNode *src_actors;
            Source *s = source(c, e->from, &why);
            if (!s) {
                say(c, k, "skipped: %s: %s", e->from, why);
                return 0;
            }
            if (!s->parsed && !byml_parse(&s->doc, s->raw, s->n, &why)) {
                say(c, k, "skipped: %s: %s", e->from, why);
                return 0;
            }
            s->parsed = 1;
            if (!(obj = find_obj(c, &s->doc, e, k, e->from, &src_actors, &idx))) return 0;
        }
        if (!mine || mine->type != BYML_ARRAY) {
            say(c, k, "skipped: the file has no Actors list");
            return 0;
        }
        if (find_actor(mine, e->new_hash, &there)) {
            say(c, k, "skipped: the file already has an object %llu", (unsigned long long)e->new_hash);
            return 0;
        }
        if (area_has(c, e, e->new_hash)) {
            say(c, k, "skipped: the area's other placement file has an object %llu (a copy needs a Hash the area does "
                      "not use)", (unsigned long long)e->new_hash);
            return 0;
        }
        BymlNode copy;
        if (!byml_copy(&copy, obj)) {
            say(c, k, "skipped: out of memory");
            return 0;
        }
        BymlNode *h = byml_get(&copy, "Hash"), *id = byml_path(&copy, "Phive/Placement/ID");
        h->bits = e->new_hash;
        if (id && (id->type == BYML_UINT64 || id->type == BYML_INT64) && id->bits == e->hash) id->bits = e->new_hash;
        drop_key(&copy, "Links");
        drop_key(&copy, "SRTHash");   // seen: a copy shows the same with or without it
        if (!set_vec(&copy, "Translate", e->pos) || !set_vec(&copy, "Rotate", e->rot) ||
            !byml_append(mine, NULL, copy)) {
            byml_node_free(&copy);
            say(c, k, "skipped: out of memory");
            return 0;
        }
        say(c, k, "added a copy of %s %llu%s%s as %llu at (%.2f, %.2f, %.2f) rotated (%.3f, %.3f, %.3f)", e->gyaml,
            hash, e->from ? " of " : "", e->from ? e->from : "", (unsigned long long)e->new_hash, e->pos[0], e->pos[1],
            e->pos[2], e->rot[0], e->rot[1], e->rot[2]);
        return 1;
    }
    case RESTORE_KIND_SET: {
        BymlNode *v = e->path ? byml_path(obj, e->path) : NULL;
        if (!e->path) break;
        if (e->new_type) {   // a key the game's file lacks
            BymlNode node = {.type = e->new_type};
            if (v) {
                double cur = byml_number(v);
                if (v->type == e->new_type && near(cur, e->now, 1e-5))
                    say(c, k, "skipped: %s is already %g", e->path, cur);
                else
                    say(c, k, "skipped: %s is there already, not missing", e->path);
                return 0;
            }
            if (e->new_type == BYML_STRING || isnan(byml_number(&node)) || !byml_set_number(&node, e->now)) {
                say(c, k, "skipped: %s: a new key takes a number or a bool", e->path);
                return 0;
            }
            if (!add_key(c, k, obj, e->path, node)) return 0;
            say(c, k, "%s%s%s added: %g", e->hash ? e->gyaml : "", e->hash ? " " : "", e->path, e->now);
            return 1;
        }
        double cur = v ? byml_number(v) : NAN;
        if (!v || isnan(cur)) {
            say(c, k, "skipped: %s %s", e->path, v ? "is not a number" : "is not there");
            return 0;
        }
        if (!near(cur, e->was, 1e-5)) {
            if (near(cur, e->now, 1e-5)) say(c, k, "skipped: %s is already %g", e->path, cur);
            else say(c, k, "skipped: %s is %g, not %g", e->path, cur, e->was);
            return 0;
        }
        if (!byml_set_number(v, e->now)) {
            say(c, k, "skipped: %g does not fit %s", e->now, e->path);
            return 0;
        }
        say(c, k, "%s%s%s %g -> %g", e->hash ? e->gyaml : "", e->hash ? " " : "", e->path, cur, e->now);
        return 1;
    }
    case RESTORE_KIND_SET_TEXT: {
        BymlNode *v = e->path ? byml_path(obj, e->path) : NULL;
        if (!e->path || !e->text_now) break;
        if (v && v->type != BYML_STRING) {
            say(c, k, "skipped: %s is not a text", e->path);
            return 0;
        }
        if (v && !strcmp(v->str, e->text_now)) {
            say(c, k, "skipped: %s is already \"%s\"", e->path, e->text_now);
            return 0;
        }
        if (!e->text_was) {   // a key the game's file lacks
            if (v) {
                say(c, k, "skipped: %s is there already (\"%s\"), not missing", e->path, v->str);
                return 0;
            }
            if (!add_key(c, k, obj, e->path, (BymlNode){.type = BYML_STRING, .str = e->text_now})) return 0;
            say(c, k, "%s%s%s added: \"%s\"", e->hash ? e->gyaml : "", e->hash ? " " : "", e->path, e->text_now);
            return 1;
        }
        if (!v) {
            say(c, k, "skipped: %s is not there", e->path);
            return 0;
        }
        if (strcmp(v->str, e->text_was)) {
            say(c, k, "skipped: %s is \"%s\", not \"%s\"", e->path, v->str, e->text_was);
            return 0;
        }
        v->str = e->text_now;
        say(c, k, "%s%s%s \"%s\" -> \"%s\"", e->hash ? e->gyaml : "", e->hash ? " " : "", e->path, e->text_was,
            e->text_now);
        return 1;
    }
    case RESTORE_KIND_ADD_MEMBER:
        break;
    }
    say(c, k, "skipped: an edit of this kind needs %s", e->kind == RESTORE_KIND_ADD_MEMBER ? "a pack" : "more fields");
    return 0;
}

// The edits idx[0..cnt) of one document doc[0..n) (a BYML file, or a pack's BYML member `what`); the document
// rewritten (malloc'd) when one applied, else NULL.
static uint8_t *edit_doc(Ctx *c, const RestoreEdit *const *edits, const int *idx, int cnt, const char *what,
                         const uint8_t *doc, size_t n, int *ok, size_t *out_n) {
    Byml b, back;
    const char *why;
    if (!byml_parse(&b, doc, n, &why)) {
        for (int j = 0; j < cnt; j++) say(c, idx[j], "skipped: %s: %s", what, why);
        return NULL;
    }
    int any = 0;
    for (int j = 0; j < cnt; j++) any |= ok[idx[j]] = apply_one(c, &b, edits[idx[j]], idx[j]);
    uint8_t *out = any ? byml_write(&b, out_n) : NULL;
    int same = out && byml_parse(&back, out, *out_n, &why);
    if (same) {
        same = byml_equal(&back.root, &b.root);
        byml_free(&back);
    }
    if (any && !same) {
        for (int j = 0; j < cnt; j++)
            if (ok[idx[j]]) say(c, idx[j], "undone: %s does not read back as edited", what);
        for (int j = 0; j < cnt; j++) ok[idx[j]] = 0;
        free(out);
        out = NULL;
    }
    byml_free(&b);
    return out;
}

// An ADD_MEMBER edit on the pack raw[0..*rn): the pack with the member (replacing *raw) and 1, else 0.
static int add_member(Ctx *c, const RestoreEdit *e, int k, uint8_t **raw, size_t *rn, size_t *added) {
    const char *why;
    size_t at, len;
    if (!e->member || (!e->bytes && !e->from_member)) {
        say(c, k, "skipped: an ADD_MEMBER names the member and its bytes or where to copy them from");
        return 0;
    }
    if (sarc_find(*raw, *rn, e->member, &at, &len, &why)) {
        say(c, k, "skipped: the pack has %s already", e->member);
        return 0;
    }
    const uint8_t *data = e->bytes;
    size_t size = e->nbytes, align = 8;
    if (!data) {
        const uint8_t *from = *raw;
        size_t from_n = *rn;
        if (e->from) {
            Source *s = source(c, e->from, &why);
            if (!s) {
                say(c, k, "skipped: %s: %s", e->from, why);
                return 0;
            }
            from = s->raw, from_n = s->n;
        }
        if (!sarc_find(from, from_n, e->from_member, &at, &size, &why)) {
            say(c, k, "skipped: %s: %s: %s", e->from ? e->from : "the pack", e->from_member, why);
            return 0;
        }
        data = from + at;
        align = sarc_alignment(from, from_n, e->from_member);
        if (align < 8) align = 8;
    }
    size_t out_n;
    uint8_t *out = sarc_add(*raw, *rn, e->member, data, size, align, &out_n, &why);
    if (!out) {
        say(c, k, "skipped: %s: %s", e->member, why);
        return 0;
    }
    free(*raw);
    *raw = out, *rn = out_n;
    if (added) *added = size;
    say(c, k, "added the member %s (%zu bytes%s%s%s%s)", e->member, size, e->bytes ? ", declared" : ", copied from ",
        e->bytes ? "" : e->from ? e->from : "the pack", e->bytes ? "" : ": ", e->bytes ? "" : e->from_member);
    return 1;
}

static int same_member(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

void *restore_file(const RestoreEdit *const *edits, int n, const void *in, size_t size, const void *dic, size_t dic_n,
                   void *(*fetch)(const char *rel, size_t *n), const char *(*list)(const char *prefix, int i), int *ok,
                   RestoreMemberSize *sizes, void (*note)(void *ctx, int k, const char *text), void *ctx,
                   size_t *out_n) {
    Ctx c = {note, ctx, dic, dic_n, fetch, {{0}}, 0, list, NULL, 0, 0};
    const uint8_t *p = in, *dict = NULL;
    size_t rn = 0, dict_n = 0, raw_was = 0;
    uint8_t *raw = NULL, *out = NULL;
    int *idx = malloc(sizeof *idx * (n ? n : 1)), *done = calloc(n ? n : 1, sizeof *done);
    int packed = size >= 4 && !memcmp(p, "\x28\xb5\x2f\xfd", 4), any = 0;
    const char *why = NULL;
    for (int k = 0; k < n; k++) ok[k] = 0;
    if (sizes)
        for (int k = 0; k < n; k++) sizes[k] = (RestoreMemberSize){0, 0};
    if (!idx || !done) why = "out of memory";
    else raw = unpack(&c, p, size, &rn, &dict, &dict_n, &why);
    if (!raw) {
        for (int k = 0; k < n; k++) say(&c, k, "skipped: %s", why);
        goto done;
    }
    raw_was = rn;
    // The members a pack gains, first: an edit of the same file may then set values in one.
    for (int k = 0; k < n; k++)
        if (edits[k]->kind == RESTORE_KIND_ADD_MEMBER) {
            done[k] = 1;
            ok[k] = add_member(&c, edits[k], k, &raw, &rn, sizes ? &sizes[k].now : NULL);
        }
    // One document at a time: the file itself, or each pack member some edit names, in the order the edits name them.
    for (int k = 0; k < n; k++) {
        if (done[k]) continue;
        const char *m = edits[k]->member;
        int cnt = 0;
        for (int j = k; j < n; j++)
            if (!done[j] && same_member(edits[j]->member, m)) idx[cnt++] = j, done[j] = 1;
        size_t at = 0, len = rn, doc_n;
        if (m && !sarc_find(raw, rn, m, &at, &len, &why)) {
            for (int j = 0; j < cnt; j++) say(&c, idx[j], "skipped: %s: %s", m, why);
            continue;
        }
        const char *what = m ? m : "the file";
        uint8_t *doc = edit_doc(&c, edits, idx, cnt, what, raw + at, len, ok, &doc_n);
        if (!doc) continue;
        uint8_t *next = doc;
        size_t next_n = doc_n;
        if (m) {
            next = sarc_replace(raw, rn, m, doc, doc_n, &next_n, &why);
            free(doc);
            if (!next) {
                for (int j = 0; j < cnt; j++)
                    if (ok[idx[j]]) say(&c, idx[j], "undone: %s", why);
                for (int j = 0; j < cnt; j++) ok[idx[j]] = 0;
                continue;
            }
            // the member's own entry in the resource size table sizes the game's buffer for it (restbl_overlay)
            for (int j = 0; sizes && j < cnt; j++)
                if (ok[idx[j]]) {
                    sizes[idx[j]] = (RestoreMemberSize){len, doc_n};
                    break;
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
        ZSTD_CCtx *cc = ZSTD_createCCtx();
        out = cc ? malloc(cap) : NULL;
        if (out) zn = ZSTD_compress_usingDict(cc, out, cap, raw, rn, dict, dict_n, LEVEL);
        ZSTD_freeCCtx(cc);
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
                if (ok[k]) say(&c, k, "undone: the edited file does not pack again");
            for (int k = 0; k < n; k++) ok[k] = 0;
            free(out);
            out = NULL;
            goto done;
        }
        *out_n = zn;
    }
    say(&c, -1, "%zu bytes unpacked, %zu after the edits", raw_was, rn);
done:
    for (int i = 0; i < c.nsrc; i++) {
        if (c.src[i].parsed) byml_free(&c.src[i].doc);
        free(c.src[i].raw);
        free(c.src[i].rel);
    }
    free(raw);
    free(c.compound);
    free(idx);
    free(done);
    return out;
}
