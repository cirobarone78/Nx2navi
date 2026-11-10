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

uint8_t *sarc_replace(const uint8_t *p, size_t n, const char *name, const uint8_t *data, size_t size, size_t *out_n,
                      const char **why) {
    Head h;
    if (!head(p, n, &h, why)) return NULL;
    int i = find(p, &h, name);
    if (i < 0) return *why = "the pack has no such member", NULL;
    // Every node, as this moves them: inside the data, and a name that ends before it (before the names' last NUL).
    size_t names_end = h.data;
    while (names_end > h.names && p[names_end - 1]) names_end--;
    Node *nodes = malloc(sizeof *nodes * (h.count ? h.count : 1));
    uint8_t *out = NULL;
    if (!nodes) {
        *why = "out of memory";
        goto done;
    }
    for (unsigned j = 0; j < h.count; j++) {
        nodes[j] = node(p, &h, j);
        size_t s = name_at(&nodes[j], &h);
        if (!inside(&nodes[j], &h, n)) {
            *why = "a SARC member that runs past the end";
            goto done;
        }
        if (s && s >= names_end) {
            *why = "a SARC member name that runs past the names";
            goto done;
        }
    }
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
