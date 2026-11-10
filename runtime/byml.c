// BYML documents (byml.h). The format: a 16-byte header ("YB", u16 version, then u32 offsets of the key table, the
// string table and the root container), each table a node {u8 0xc2, u24 count} with count + 1 offsets from its start
// and the NUL-terminated strings; a container is {u8 type, u24 count} followed by its items. An array has count type
// bytes padded to 4 and then count 32-bit values; a dict has count entries {u24 key index, u8 type, u32 value} sorted
// by key; a hash dict (0x20) has count entries {u32 hash, u32 value} sorted by hash and then count type bytes. A value
// is the scalar itself (bool, int, float, uint), a string's index in the string table, or an offset: of a container,
// of an 8-byte value (int64, uint64, double) or of binary data {u32 size, bytes}. Every field is little-endian.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "byml.h"

#define MAX_DEPTH 64
// Items the containers of a parse hold in all, counted when each container's items are allocated (so at most 8 Mi
// nodes, 512 MiB, are ever allocated); equal containers a file shares are read once per use.
#define MAX_NODES (8u << 20)

typedef struct {
    const uint8_t *p;
    size_t n;
    const char **keys, **strs;
    uint32_t nkeys, nstrs, nodes;
    const char *why;
} Reader;

static uint32_t rd32(const Reader *r, size_t off) {
    const uint8_t *q = r->p + off;
    return (uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
}

static uint64_t rd64(const Reader *r, size_t off) { return (uint64_t)rd32(r, off + 4) << 32 | rd32(r, off); }

// {type, count} of the container at off; 0 when off is outside the file.
static int head(const Reader *r, size_t off, uint8_t *type, uint32_t *count) {
    if (off > r->n || r->n - off < 4 || off % 4) return 0;
    uint32_t w = rd32(r, off);
    *type = (uint8_t)w;
    *count = w >> 8;
    return 1;
}

static int fail(Reader *r, const char *why) {
    if (!r->why) r->why = why;
    return 0;
}

// The table of strings at off (0: none).
static int table(Reader *r, uint32_t off, const char ***out, uint32_t *count) {
    *out = NULL, *count = 0;
    if (!off) return 1;
    uint8_t type;
    uint32_t n;
    if (!head(r, off, &type, &n) || type != 0xc2) return fail(r, "a string table is not one");
    if ((r->n - off - 4) / 4 < (size_t)n + 1) return fail(r, "a string table runs past the end");
    const char **s = malloc(sizeof *s * (n ? n : 1));
    if (!s) return fail(r, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        size_t at = (size_t)off + rd32(r, off + 4 + 4 * (size_t)i);
        if (at >= r->n || !memchr(r->p + at, 0, r->n - at)) {
            free(s);
            return fail(r, "a string runs past the end");
        }
        s[i] = (const char *)r->p + at;
    }
    *out = s, *count = n;
    return 1;
}

static int is_container(uint8_t t) { return t == BYML_ARRAY || t == BYML_DICT || t == BYML_HASH32; }

static int value(Reader *r, BymlNode *out, uint8_t type, uint32_t v, int depth);

static int container(Reader *r, BymlNode *out, uint8_t type, uint32_t off, int depth) {
    uint8_t t;
    uint32_t n;
    if (depth > MAX_DEPTH) return fail(r, "containers nest too deep");
    if (!head(r, off, &t, &n) || t != type) return fail(r, "a container offset does not point at one");
    size_t room = r->n - off - 4;
    size_t need = type == BYML_ARRAY ? ((size_t)n + 3) / 4 * 4 + 4 * (size_t)n
                : type == BYML_DICT  ? 8 * (size_t)n
                                     : 8 * (size_t)n + n;
    if (need > room) return fail(r, "a container runs past the end");
    if (n > MAX_NODES - r->nodes) return fail(r, "too many nodes");
    r->nodes += n;
    out->type = type;
    out->n = n;
    out->items = n ? calloc(n, sizeof *out->items) : NULL;
    if (type == BYML_DICT && n) out->keys = calloc(n, sizeof *out->keys);
    if (type == BYML_HASH32 && n) out->hashes = calloc(n, sizeof *out->hashes);
    if (n && (!out->items || (type == BYML_DICT && !out->keys) || (type == BYML_HASH32 && !out->hashes))) {
        out->n = 0;
        return fail(r, "out of memory");
    }
    size_t base = (size_t)off + 4;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t it;
        uint32_t iv;
        if (type == BYML_ARRAY) {
            it = r->p[base + i];
            iv = rd32(r, base + ((size_t)n + 3) / 4 * 4 + 4 * (size_t)i);
        } else if (type == BYML_DICT) {
            uint32_t w = rd32(r, base + 8 * (size_t)i), k = w & 0xffffff;
            it = (uint8_t)(w >> 24);
            if (k >= r->nkeys) {
                out->n = i;
                return fail(r, "a dict key index is outside the key table");
            }
            out->keys[i] = r->keys[k];
            // The game looks keys up in sorted dicts; one out of order or twice is a broken file, and sorted keys let
            // byml_equal compare two dicts item by item.
            if (i && strcmp(out->keys[i - 1], out->keys[i]) >= 0) {
                out->n = i;
                return fail(r, "a dict's keys are not in order");
            }
            iv = rd32(r, base + 8 * (size_t)i + 4);
        } else {
            out->hashes[i] = rd32(r, base + 8 * (size_t)i);
            if (i && out->hashes[i - 1] >= out->hashes[i]) {
                out->n = i;
                return fail(r, "a hash dict's hashes are not in order");
            }
            iv = rd32(r, base + 8 * (size_t)i + 4);
            it = r->p[base + 8 * (size_t)n + i];
        }
        if (!value(r, &out->items[i], it, iv, depth + 1)) {
            out->n = i + 1;   // byml_free frees what the failed item holds too
            return 0;
        }
    }
    return 1;
}

static int value(Reader *r, BymlNode *out, uint8_t type, uint32_t v, int depth) {
    out->type = type;
    switch (type) {
    case BYML_STRING:
        if (v >= r->nstrs) return fail(r, "a string index is outside the string table");
        out->str = r->strs[v];
        return 1;
    case BYML_BINARY:
        if ((size_t)v + 4 > r->n || r->n - v - 4 < rd32(r, v)) return fail(r, "binary data runs past the end");
        out->len = rd32(r, v);
        out->str = (const char *)r->p + v + 4;
        return 1;
    case BYML_ARRAY: case BYML_DICT: case BYML_HASH32:
        return container(r, out, type, v, depth);
    case BYML_BOOL: case BYML_INT: case BYML_FLOAT: case BYML_UINT:
        out->bits = v;
        return 1;
    case BYML_INT64: case BYML_UINT64: case BYML_DOUBLE:
        if ((size_t)v + 8 > r->n) return fail(r, "a 64-bit value runs past the end");
        out->bits = rd64(r, v);
        return 1;
    case BYML_NULL:
        return 1;
    default:
        return fail(r, "a node type this reader does not know");
    }
}

static void node_free(BymlNode *node) {
    for (uint32_t i = 0; i < node->n; i++) node_free(&node->items[i]);
    free(node->items);
    free(node->keys);
    free(node->hashes);
    memset(node, 0, sizeof *node);
}

int byml_parse(Byml *b, const uint8_t *p, size_t n, const char **why) {
    memset(b, 0, sizeof *b);
    Reader r = {p, n, NULL, NULL, 0, 0, 0, NULL};
    int ok = 0;
    if (n >= 16 && !memcmp(p, "BY", 2)) {
        fail(&r, "a big-endian BYML file");
        goto done;
    }
    if (n < 16 || memcmp(p, "YB", 2)) {
        fail(&r, "not a BYML file");
        goto done;
    }
    b->version = (uint16_t)(p[3] << 8 | p[2]);
    if (b->version < 2 || b->version > 7) {
        fail(&r, "a BYML version this reader does not know");
        goto done;
    }
    if (!table(&r, rd32(&r, 4), &r.keys, &r.nkeys) || !table(&r, rd32(&r, 8), &r.strs, &r.nstrs)) goto done;
    uint32_t root = rd32(&r, 12);
    uint8_t type;
    uint32_t count;
    if (!root) {
        b->root.type = BYML_NULL;
        ok = 1;
        goto done;
    }
    if (!head(&r, root, &type, &count) || !is_container(type)) {
        fail(&r, "the root is not a container");
        goto done;
    }
    ok = container(&r, &b->root, type, root, 0);
done:
    free(r.keys);
    free(r.strs);
    if (!ok) {
        byml_free(b);
        if (why) *why = r.why ? r.why : "unreadable";
    }
    return ok;
}

void byml_free(Byml *b) { node_free(&b->root); }

// ---- queries and edits

BymlNode *byml_get(const BymlNode *node, const char *key) {
    if (!node || node->type != BYML_DICT) return NULL;
    for (uint32_t i = 0; i < node->n; i++)
        if (!strcmp(node->keys[i], key)) return &node->items[i];
    return NULL;
}

BymlNode *byml_path(BymlNode *node, const char *path) {
    char seg[256];
    while (node && *path) {
        size_t len = strcspn(path, "/");
        if (!len || len >= sizeof seg) return NULL;
        memcpy(seg, path, len);
        seg[len] = 0;
        path += len + (path[len] == '/');
        if (node->type == BYML_ARRAY) {
            char *end;
            unsigned long i = strtoul(seg, &end, 10);
            node = *end || i >= node->n ? NULL : &node->items[i];
        } else {
            node = byml_get(node, seg);
        }
    }
    return node;
}

double byml_number(const BymlNode *node) {
    uint32_t lo = (uint32_t)node->bits;
    switch (node->type) {
    case BYML_BOOL: return lo != 0;
    case BYML_INT: return (int32_t)lo;
    case BYML_UINT: return lo;
    case BYML_INT64: return (double)(int64_t)node->bits;
    case BYML_UINT64: return (double)node->bits;
    case BYML_FLOAT: { float f; memcpy(&f, &lo, 4); return f; }
    case BYML_DOUBLE: { double d; memcpy(&d, &node->bits, 8); return d; }
    default: return NAN;
    }
}

int byml_set_number(BymlNode *node, double v) {
    if (v != v) return 0;
    double r = nearbyint(v);
    switch (node->type) {
    case BYML_BOOL: node->bits = v != 0; return 1;
    case BYML_INT:
        if (r < INT32_MIN || r > INT32_MAX) return 0;
        node->bits = (uint32_t)(int32_t)r;
        return 1;
    case BYML_UINT:
        if (r < 0 || r > UINT32_MAX) return 0;
        node->bits = (uint32_t)r;
        return 1;
    case BYML_INT64:
        if (r < -9.2233720368547758e18 || r >= 9.2233720368547758e18) return 0;
        node->bits = (uint64_t)(int64_t)r;
        return 1;
    case BYML_UINT64:
        if (r < 0 || r >= 1.8446744073709552e19) return 0;
        node->bits = (uint64_t)r;
        return 1;
    case BYML_FLOAT: {
        float f = (float)v;
        uint32_t b;
        memcpy(&b, &f, 4);
        node->bits = b;
        return 1;
    }
    case BYML_DOUBLE: memcpy(&node->bits, &v, 8); return 1;
    default: return 0;
    }
}

int byml_equal(const BymlNode *a, const BymlNode *b) {
    if (a->type != b->type) return 0;
    switch (a->type) {
    case BYML_STRING: return !strcmp(a->str, b->str);
    case BYML_BINARY: return a->len == b->len && !memcmp(a->str, b->str, a->len);
    case BYML_ARRAY: case BYML_DICT: case BYML_HASH32:
        if (a->n != b->n) return 0;
        for (uint32_t i = 0; i < a->n; i++) {
            // dicts item by item: the parser takes them sorted, an edit keeps their keys and byml_write sorts them
            if (a->type == BYML_DICT && strcmp(a->keys[i], b->keys[i])) return 0;
            if (a->type == BYML_HASH32 && a->hashes[i] != b->hashes[i]) return 0;
            if (!byml_equal(&a->items[i], &b->items[i])) return 0;
        }
        return 1;
    case BYML_NULL: return 1;
    default: return a->bits == b->bits;
    }
}

// ---- writing

typedef struct { uint8_t *p; size_t n, cap; int ok; } Buf;

static void put(Buf *b, const void *src, size_t n) {
    if (!b->ok) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            b->ok = 0;
            return;
        }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static void pad(Buf *b, size_t align) {
    static const uint8_t zero[8];
    while (b->ok && b->n % align) put(b, zero, 1);
}

static void wr32(uint8_t *q, uint32_t v) {
    for (int i = 0; i < 4; i++) q[i] = (uint8_t)(v >> (8 * i));
}

static void put32(Buf *b, uint32_t v) {
    uint8_t q[4];
    wr32(q, v);
    put(b, q, 4);
}

static uint32_t head_word(uint8_t type, uint32_t count) { return count << 8 | type; }

// A sorted set of strings.
typedef struct { const char **s; size_t n, cap; } Strings;

static int strings_add(Strings *t, const char *s) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        const char **p = realloc(t->s, sizeof *p * cap);
        if (!p) return 0;
        t->s = p, t->cap = cap;
    }
    t->s[t->n++] = s;
    return 1;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void strings_sort(Strings *t) {
    if (!t->n) return;
    qsort(t->s, t->n, sizeof *t->s, cmp_str);
    size_t k = 1;
    for (size_t i = 1; i < t->n; i++)
        if (strcmp(t->s[i], t->s[k - 1])) t->s[k++] = t->s[i];
    t->n = k;
}

static uint32_t strings_index(const Strings *t, const char *s) {
    const char **hit = bsearch(&s, t->s, t->n, sizeof *t->s, cmp_str);
    return (uint32_t)(hit - t->s);
}

static int collect(const BymlNode *node, Strings *keys, Strings *strs) {
    if (node->type == BYML_STRING) return strings_add(strs, node->str);
    for (uint32_t i = 0; i < node->n; i++) {
        if (node->keys && !strings_add(keys, node->keys[i])) return 0;
        if (!collect(&node->items[i], keys, strs)) return 0;
    }
    return 1;
}

static void put_table(Buf *b, const Strings *t) {
    size_t off = 4 + 4 * (t->n + 1);
    put32(b, head_word(0xc2, (uint32_t)t->n));
    for (size_t i = 0; i <= t->n; i++) {
        put32(b, (uint32_t)off);
        if (i < t->n) off += strlen(t->s[i]) + 1;
    }
    for (size_t i = 0; i < t->n; i++) put(b, t->s[i], strlen(t->s[i]) + 1);
    pad(b, 4);
}

// Equal blocks the writer has put down, by a hash of their bytes: a container or an 8-byte value is written once.
typedef struct { uint32_t hash, off, len; } Seen;
typedef struct {
    Buf out;
    Strings keys, strs;
    Seen *seen;
    size_t nseen, capseen;   // open addressing, capseen a power of two
} Writer;

static uint32_t fnv(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static int seen_grow(Writer *w) {
    size_t cap = w->capseen ? w->capseen * 2 : 4096;
    Seen *s = calloc(cap, sizeof *s);
    if (!s) return 0;
    for (size_t i = 0; i < w->capseen; i++) {
        if (!w->seen[i].hash) continue;
        size_t j = w->seen[i].hash & (cap - 1);
        while (s[j].hash) j = (j + 1) & (cap - 1);
        s[j] = w->seen[i];
    }
    free(w->seen);
    w->seen = s, w->capseen = cap;
    return 1;
}

// The offset of block p[0..n) in the output, written at an offset aligned to align unless an equal block is there.
static uint32_t place(Writer *w, const uint8_t *p, size_t n, size_t align) {
    if ((w->nseen + 1) * 2 > w->capseen && !seen_grow(w)) w->out.ok = 0;
    if (!w->out.ok) return 0;
    uint32_t h = fnv(p, n);
    size_t j = h & (w->capseen - 1);
    for (; w->seen[j].hash; j = (j + 1) & (w->capseen - 1))
        if (w->seen[j].hash == h && w->seen[j].len == n && !memcmp(w->out.p + w->seen[j].off, p, n) &&
            w->seen[j].off % align == 0)
            return w->seen[j].off;
    pad(&w->out, align);
    size_t off = w->out.n;
    put(&w->out, p, n);
    if (!w->out.ok || off > UINT32_MAX - n) {
        w->out.ok = 0;
        return 0;
    }
    w->seen[j] = (Seen){h, (uint32_t)off, (uint32_t)n};
    w->nseen++;
    return (uint32_t)off;
}

typedef struct { const char *key; uint32_t hash, i; } Order;

static int cmp_key(const void *a, const void *b) { return strcmp(((const Order *)a)->key, ((const Order *)b)->key); }

static int cmp_hash(const void *a, const void *b) {
    uint32_t x = ((const Order *)a)->hash, y = ((const Order *)b)->hash;
    return x < y ? -1 : x > y;
}

static uint32_t emit(Writer *w, const BymlNode *node, int depth);

static uint32_t emit_container(Writer *w, const BymlNode *node, int depth) {
    uint32_t n = node->n;
    uint32_t *vals = malloc(sizeof *vals * (n ? n : 1));
    Order *ord = malloc(sizeof *ord * (n ? n : 1));
    size_t size = node->type == BYML_ARRAY ? 4 + ((size_t)n + 3) / 4 * 4 + 4 * (size_t)n
                : node->type == BYML_DICT  ? 4 + 8 * (size_t)n
                                           : 4 + 8 * (size_t)n + ((size_t)n + 3) / 4 * 4;
    uint8_t *blk = calloc(1, size);
    uint32_t off = 0;
    if (!vals || !ord || !blk || depth > MAX_DEPTH) {
        w->out.ok = 0;
        goto done;
    }
    for (uint32_t i = 0; i < n; i++) {
        ord[i] = (Order){node->keys ? node->keys[i] : NULL, node->hashes ? node->hashes[i] : 0, i};
        vals[i] = emit(w, &node->items[i], depth + 1);
    }
    if (!w->out.ok) goto done;
    if (node->type == BYML_DICT) qsort(ord, n, sizeof *ord, cmp_key);
    if (node->type == BYML_HASH32) qsort(ord, n, sizeof *ord, cmp_hash);
    wr32(blk, head_word(node->type, n));
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = ord[k].i;
        uint8_t type = node->items[i].type;
        if (node->type == BYML_ARRAY) {
            blk[4 + k] = type;
            wr32(blk + 4 + ((size_t)n + 3) / 4 * 4 + 4 * (size_t)k, vals[i]);
        } else if (node->type == BYML_DICT) {
            wr32(blk + 4 + 8 * (size_t)k, strings_index(&w->keys, ord[k].key) | (uint32_t)type << 24);
            wr32(blk + 8 + 8 * (size_t)k, vals[i]);
        } else {
            wr32(blk + 4 + 8 * (size_t)k, ord[k].hash);
            wr32(blk + 8 + 8 * (size_t)k, vals[i]);
            blk[4 + 8 * (size_t)n + k] = type;
        }
    }
    off = place(w, blk, size, 4);
done:
    free(blk);
    free(ord);
    free(vals);
    return off;
}

static uint32_t emit(Writer *w, const BymlNode *node, int depth) {
    switch (node->type) {
    case BYML_STRING: return strings_index(&w->strs, node->str);
    case BYML_ARRAY: case BYML_DICT: case BYML_HASH32: return emit_container(w, node, depth);
    case BYML_INT64: case BYML_UINT64: case BYML_DOUBLE: {
        uint8_t q[8];
        wr32(q, (uint32_t)node->bits);
        wr32(q + 4, (uint32_t)(node->bits >> 32));
        return place(w, q, 8, 8);
    }
    case BYML_BINARY: {
        uint8_t *q = malloc((size_t)node->len + 4);
        if (!q) {
            w->out.ok = 0;
            return 0;
        }
        wr32(q, node->len);
        memcpy(q + 4, node->str, node->len);
        uint32_t off = place(w, q, (size_t)node->len + 4, 4);
        free(q);
        return off;
    }
    case BYML_NULL: return 0;
    default: return (uint32_t)node->bits;
    }
}

// The 8-byte values (binary: the binary data) below node, ahead of every container: one run of them needs no padding,
// where values put between 4-aligned containers would cost up to 4 bytes each. emit finds each again by its bytes.
static void place_values(Writer *w, const BymlNode *node, int binary, int depth) {
    if (depth > MAX_DEPTH) return;
    if (is_container(node->type)) {
        for (uint32_t i = 0; i < node->n; i++) place_values(w, &node->items[i], binary, depth + 1);
    } else if (binary ? node->type == BYML_BINARY
                      : node->type == BYML_INT64 || node->type == BYML_UINT64 || node->type == BYML_DOUBLE) {
        emit(w, node, depth);
    }
}

uint8_t *byml_write(const Byml *b, size_t *n) {
    Writer w = {{NULL, 0, 0, 1}, {0}, {0}, NULL, 0, 0};
    uint8_t *ret = NULL;
    if (!collect(&b->root, &w.keys, &w.strs)) goto done;
    strings_sort(&w.keys);
    strings_sort(&w.strs);
    uint8_t hdr[16] = {'Y', 'B', (uint8_t)b->version, (uint8_t)(b->version >> 8)};
    put(&w.out, hdr, sizeof hdr);
    uint32_t koff = 0, soff = 0, root = 0;
    if (w.keys.n) {
        koff = (uint32_t)w.out.n;
        put_table(&w.out, &w.keys);
    }
    if (w.strs.n) {
        soff = (uint32_t)w.out.n;
        put_table(&w.out, &w.strs);
    }
    if (b->root.type != BYML_NULL) {
        place_values(&w, &b->root, 0, 0);
        place_values(&w, &b->root, 1, 0);
        root = emit(&w, &b->root, 0);
    }
    if (!w.out.ok) goto done;
    wr32(w.out.p + 4, koff);
    wr32(w.out.p + 8, soff);
    wr32(w.out.p + 12, root);
    ret = w.out.p;
    *n = w.out.n;
    w.out.p = NULL;
done:
    free(w.out.p);
    free(w.keys.s);
    free(w.strs.s);
    free(w.seen);
    return ret;
}
