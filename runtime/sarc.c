// SARC archives (sarc.h). The format: a 0x14-byte header ("SARC", u16 header size, the byte-order mark FF FE, u32 file
// size, u32 offset of the data, u16 version); "SFAT" with a u16 node count and one node per member {u32 name hash, u32
// attributes (bit 24: named; the low 24 bits: the name's offset in SFNT's names / 4), u32 start, u32 end} with start
// and end from the data offset; "SFNT" and 4 more bytes, then the names, each NUL-terminated and padded to 4; then the
// data.
//
// sarc_find and sarc_at check the header and only the node they compare or return, so a damaged node elsewhere in the
// archive does not hide the one asked for, and a call costs the same however many nodes there are. sarc_replace
// moves members, so it checks every node first.
#include <stdlib.h>
#include <string.h>

#include "sarc.h"

typedef struct {
    size_t sfat, names;   // offsets of "SFAT" and of the first name
    uint32_t data;        // offset of the data
    unsigned count;       // nodes
} Head;

typedef struct { uint32_t attr, begin, end; } Node;

static uint32_t rd32(const uint8_t *p, size_t off) {
    return (uint32_t)p[off] | (uint32_t)p[off + 1] << 8 | (uint32_t)p[off + 2] << 16 | (uint32_t)p[off + 3] << 24;
}

static uint16_t rd16(const uint8_t *p, size_t off) { return (uint16_t)(p[off] | p[off + 1] << 8); }

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// The header and the two tables' places; 0 with why. Every node and the names then lie before the data.
static int head(const uint8_t *p, size_t n, Head *h, const char **why) {
    if (n < 0x14 || memcmp(p, "SARC", 4)) return *why = "not a SARC archive", 0;
    if (rd16(p, 6) != 0xfeff) return *why = "a big-endian SARC archive", 0;
    h->sfat = rd16(p, 4);
    h->data = rd32(p, 12);
    if (h->data > n || h->sfat + 12 > h->data || memcmp(p + h->sfat, "SFAT", 4))
        return *why = "a SARC header that is not one", 0;
    h->count = rd16(p, h->sfat + 6);
    size_t sfnt = h->sfat + 12 + (size_t)h->count * 16;
    if (sfnt + 8 > h->data || memcmp(p + sfnt, "SFNT", 4)) return *why = "a SARC node table that runs past its data", 0;
    h->names = sfnt + 8;
    return 1;
}

static Node node(const uint8_t *p, const Head *h, unsigned i) {
    size_t e = h->sfat + 12 + (size_t)i * 16;
    return (Node){rd32(p, e + 4), rd32(p, e + 8), rd32(p, e + 12)};
}

// 1 when the member lies inside the archive's data.
static int inside(const Node *m, const Head *h, size_t n) { return m->begin <= m->end && m->end <= n - h->data; }

// Offset of the member's name, 0 when it has none.
static size_t name_at(const Node *m, const Head *h) {
    return m->attr >> 24 ? h->names + (size_t)(m->attr & 0xffffff) * 4 : 0;
}

// The first node named `name` (its name and NUL before the data), -1 when there is none.
static int find(const uint8_t *p, const Head *h, const char *name) {
    size_t len = strlen(name);
    for (unsigned i = 0; i < h->count; i++) {
        Node m = node(p, h, i);
        size_t s = name_at(&m, h);
        if (s && s + len + 1 <= h->data && !memcmp(p + s, name, len + 1)) return (int)i;
    }
    return -1;
}

int sarc_find(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size, const char **why) {
    Head h;
    const char *w;
    if (!why) why = &w;
    if (!head(p, n, &h, why)) return 0;
    int i = find(p, &h, name);
    if (i < 0) return *why = "the pack has no such member", 0;
    Node m = node(p, &h, (unsigned)i);
    if (!inside(&m, &h, n)) return *why = "a SARC member that runs past the end", 0;
    *at = (size_t)h.data + m.begin, *size = m.end - m.begin;
    return 1;
}

int sarc_at(const uint8_t *p, size_t n, unsigned i, const char **name, size_t *at, size_t *size) {
    Head h;
    const char *why;
    if (!head(p, n, &h, &why) || i >= h.count) return 0;
    Node m = node(p, &h, i);
    if (!inside(&m, &h, n)) return 0;
    if (name) {
        size_t s = name_at(&m, &h);
        if (s && (s >= h.data || !memchr(p + s, 0, h.data - s))) return 0;
        *name = s ? (const char *)p + s : "";
    }
    *at = (size_t)h.data + m.begin, *size = m.end - m.begin;
    return 1;
}

// Every node of the archive (malloc'd), for the calls that move members: each inside the data, and each name ending
// before the data (before the names' last NUL); NULL with why.
static Node *every_node(const uint8_t *p, size_t n, const Head *h, const char **why) {
    size_t names_end = h->data;
    while (names_end > h->names && p[names_end - 1]) names_end--;
    Node *nodes = malloc(sizeof *nodes * (h->count ? h->count : 1));
    if (!nodes) return *why = "out of memory", NULL;
    for (unsigned j = 0; j < h->count; j++) {
        nodes[j] = node(p, h, j);
        size_t s = name_at(&nodes[j], h);
        if (!inside(&nodes[j], h, n)) *why = "a SARC member that runs past the end";
        else if (s && s >= names_end) *why = "a SARC member name that runs past the names";
        else continue;
        free(nodes);
        return NULL;
    }
    return nodes;
}

uint8_t *sarc_replace(const uint8_t *p, size_t n, const char *name, const uint8_t *data, size_t size, size_t *out_n,
                      const char **why) {
    Head h;
    if (!head(p, n, &h, why)) return NULL;
    int i = find(p, &h, name);
    if (i < 0) return *why = "the pack has no such member", NULL;
    Node *nodes = every_node(p, n, &h, why);
    uint8_t *out = NULL;
    if (!nodes) return NULL;
    // The members after it (starting at or past its end) and the room up to the first of them.
    size_t begin = nodes[i].begin, slot = n - h.data, align = 4;
    for (unsigned j = 0; j < h.count; j++) {
        if ((int)j == i) continue;
        if (nodes[j].begin >= nodes[i].end) {
            if (nodes[j].begin < slot) slot = nodes[j].begin;
            size_t off = (size_t)h.data + nodes[j].begin, low = off ? off & (~off + 1) : 0x2000;
            if (low > align) align = low > 0x2000 ? 0x2000 : low;
        } else if (nodes[j].end > begin && nodes[j].begin < nodes[i].end) {
            *why = "SARC members overlap";
            goto done;
        }
    }
    if (size > UINT32_MAX - begin) {
        *why = "the member is too large";
        goto done;
    }
    size_t shift = begin + size > slot ? (begin + size - slot + align - 1) / align * align : 0;
    *out_n = n + shift;
    if (*out_n > UINT32_MAX || !(out = calloc(1, *out_n))) {
        *why = "out of memory";
        goto done;
    }
    memcpy(out, p, h.data + begin);
    memcpy(out + h.data + begin, data, size);
    memcpy(out + h.data + slot + shift, p + h.data + slot, n - h.data - slot);
    wr32(out + 8, (uint32_t)*out_n);
    wr32(out + h.sfat + 12 + (size_t)i * 16 + 12, (uint32_t)(begin + size));
    for (unsigned j = 0; j < h.count; j++)
        if ((int)j != i && nodes[j].begin >= nodes[i].end) {
            wr32(out + h.sfat + 12 + (size_t)j * 16 + 8, nodes[j].begin + (uint32_t)shift);
            wr32(out + h.sfat + 12 + (size_t)j * 16 + 12, nodes[j].end + (uint32_t)shift);
        }
done:
    free(nodes);
    return out;
}

static size_t low_bit(size_t off) { return off ? (off & (~off + 1)) > 0x2000 ? 0x2000 : off & (~off + 1) : 0x2000; }

size_t sarc_alignment(const uint8_t *p, size_t n, const char *name) {
    size_t at, size;
    return sarc_find(p, n, name, &at, &size, NULL) ? low_bit(at) : 0;
}

uint8_t *sarc_add(const uint8_t *p, size_t n, const char *name, const uint8_t *data, size_t size, size_t align,
                  size_t *out_n, const char **why) {
    Head h;
    if (!head(p, n, &h, why)) return NULL;
    Node *nodes = every_node(p, n, &h, why);
    if (!nodes) return NULL;
    uint8_t *out = NULL;
    uint32_t mult = rd32(p, h.sfat + 8), hash = 0;
    for (const char *c = name; *c; c++) hash = hash * mult + (uint32_t)(int32_t)(signed char)*c;
    // The new node's index (after every node of a smaller hash), where the names' padding ends, and the largest
    // alignment a member's offset has: the data moves by a multiple of it, so each member keeps its own.
    unsigned at = 0;
    size_t end = h.names, step = 4;
    for (unsigned i = 0; i < h.count; i++) {
        uint32_t nh = rd32(p, h.sfat + 12 + (size_t)i * 16);
        size_t s = name_at(&nodes[i], &h);
        if (s && !strcmp((const char *)p + s, name)) {
            *why = "the pack has that member already";
            goto done;
        }
        if (nh == hash) {
            *why = "another member of the pack has the name's hash";
            goto done;
        }
        if (nh < hash) at = i + 1;
        if (s) {
            size_t e = h.names + (s - h.names + strlen((const char *)p + s) + 4) / 4 * 4;
            if (e > end) end = e;
        }
        size_t a = low_bit((size_t)h.data + nodes[i].begin);
        if (a > step) step = a;
    }
    if (end > h.data) {
        *why = "a SARC name table that runs into the data";
        goto done;
    }
    size_t len = strlen(name), room = (len + 4) / 4 * 4, need = end + 16 + room;
    size_t shift = need > h.data ? (need - h.data + step - 1) / step * step : 0;
    size_t data_n = n - h.data, base = h.data + shift;
    size_t new_at = ((base + data_n + align - 1) & ~(align - 1)) - base;   // aligned in the file
    if ((end - h.names + room) / 4 > 0xffffff || h.count + 1 > 0xffff || new_at + size > UINT32_MAX - base) {
        *why = "the pack would be too large";
        goto done;
    }
    *out_n = base + new_at + size;
    if (!(out = calloc(1, *out_n))) {
        *why = "out of memory";
        goto done;
    }
    size_t node_at = h.sfat + 12 + (size_t)at * 16;
    memcpy(out, p, node_at);                                    // the header and the nodes before the new one
    wr32(out + node_at, hash);
    wr32(out + node_at + 4, 0x01000000u | (uint32_t)((end - h.names) / 4));
    wr32(out + node_at + 8, (uint32_t)new_at);
    wr32(out + node_at + 12, (uint32_t)(new_at + size));
    memcpy(out + node_at + 16, p + node_at, end - node_at);    // the nodes after it, SFNT and the names
    memcpy(out + end + 16, name, len);                          // its name, NUL-padded by calloc
    memcpy(out + base, p + h.data, data_n);                     // the data, moved by shift
    memcpy(out + base + new_at, data, size);
    out[h.sfat + 6] = (uint8_t)(h.count + 1), out[h.sfat + 7] = (uint8_t)((h.count + 1) >> 8);
    wr32(out + 8, (uint32_t)*out_n);
    wr32(out + 12, (uint32_t)base);
done:
    free(nodes);
    return out;
}
