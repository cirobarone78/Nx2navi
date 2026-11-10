// What a save holds at a glance (save_summary.h). The game's .sav files share one layout: a u32 0x01020304, the u32
// revision of the game's flag list, the u32 file offset of the data section, zeros up to 0x28, then a table of
// {u32 hash, u32 value} pairs up to the data section. A flag's hash is MurmurHash3 x86 32 (seed 0) of its name; a Bool
// or Int value is inline, a String64 value is the file offset of its NUL-terminated text. Pairs {0, k} close a type's
// group and are never looked up (no name hashes to 0 here). Each slot_NN folder holds a caption.sav, what the game's
// own load list shows of the slot (its save time, hearts and area), and progress.sav, the slot's progress, whose
// Sequence_IsChildhood flag tells child from adult.
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "save_summary.h"

#define SAV_MAGIC 0x01020304u
#define SAV_TABLE 0x28u
#define TABLE_MAX (4u << 20)   // a progress.sav table is about 96 KiB

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t rotl(uint32_t x, int r) { return x << r | x >> (32 - r); }

// MurmurHash3 x86 32 with seed 0: the hash the game files a flag under.
static uint32_t flag_hash(const char *name) {
    const uint8_t *d = (const uint8_t *)name;
    size_t n = strlen(name), blocks = n / 4;
    uint32_t h = 0, k;
    for (size_t i = 0; i < blocks; i++) {
        k = rd32(d + 4 * i) * 0xcc9e2d51u;
        h ^= rotl(k, 15) * 0x1b873593u;
        h = rotl(h, 13) * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)d[4 * blocks + 2] << 16; /* fall through */
    case 2: k ^= (uint32_t)d[4 * blocks + 1] << 8; /* fall through */
    case 1: k ^= d[4 * blocks]; h ^= rotl(k * 0xcc9e2d51u, 15) * 0x1b873593u;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ h >> 16;
}

// A .sav's bytes; n may stop at the data section when only inline values are wanted.
typedef struct { const uint8_t *b; size_t n; uint32_t end; } Sav;

static int sav_open(Sav *s, const uint8_t *b, size_t n) {
    if (!b || n < SAV_TABLE || rd32(b) != SAV_MAGIC) return 0;
    uint32_t end = rd32(b + 8);
    if (end <= SAV_TABLE) return 0;
    *s = (Sav){b, n, end < n ? end : (uint32_t)n};
    return 1;
}

// The value filed under name's hash; 0 when the save does not hold it.
static int sav_get(const Sav *s, const char *name, uint32_t *value) {
    uint32_t h = flag_hash(name);
    for (uint32_t o = SAV_TABLE; o + 8 <= s->end; o += 8)
        if (rd32(s->b + o) == h) {
            *value = rd32(s->b + o + 4);
            return 1;
        }
    return 0;
}

static int sav_int(const Sav *s, const char *name, int fallback) {
    uint32_t v;
    return sav_get(s, name, &v) ? (int)v : fallback;
}

// A String64 value into out; 0 when absent or outside the file.
static int sav_string(const Sav *s, const char *name, char *out, size_t cap) {
    uint32_t at;
    if (!sav_get(s, name, &at) || at >= s->n) return 0;
    size_t i = 0;
    for (; i + 1 < cap && at + i < s->n && i < 64 && s->b[at + i]; i++) out[i] = (char)s->b[at + i];
    out[i] = 0;
    return i > 0;
}

// The game names areas in one word per capital ("HyruleField", "InsideTheDekuTree", "ZoraDomain_Child"): a space
// goes before each capital that follows a small letter or a digit, and an underscore becomes a space.
static void area_words(const char *id, char *out, size_t cap) {
    size_t k = 0;
    for (const char *p = id; *p && k + 2 < cap; p++) {
        int up = *p >= 'A' && *p <= 'Z', after = p > id && ((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= '0' && p[-1] <= '9'));
        if (*p == '_') {
            if (k && out[k - 1] != ' ') out[k++] = ' ';
            continue;
        }
        if (up && after) out[k++] = ' ';
        out[k++] = *p;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

typedef struct { char names[100][8]; int n; } Slots;

static int slot_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Slots *s = ctx;
    if (kind == PLAT_FS_DIR && !strncmp(name, "slot_", 5) && name[5] >= '0' && name[5] <= '9' && name[6] >= '0' &&
        name[6] <= '9' && !name[7] && s->n < 100)
        memcpy(s->names[s->n++], name, 8);
    return 1;
}

static int path_of(char *out, size_t cap, const char *dir, const char *slot, const char *file) {
    size_t n = strlen(dir) + strlen(slot) + strlen(file) + 3;
    if (n > cap) return 0;
    strcpy(out, dir);
    strcat(out, "/");
    strcat(out, slot);
    strcat(out, "/");
    strcat(out, file);
    return 1;
}

// progress.sav's table only: the flag wanted is inline, and the data section after it is most of the file.
static int progress_adult(const char *path) {
    PlatFile *f = plat_file_open(path);
    if (!f) return -1;
    uint8_t head[SAV_TABLE];
    int adult = -1;
    if (plat_file_pread(f, head, SAV_TABLE, 0) == SAV_TABLE && rd32(head) == SAV_MAGIC) {
        uint32_t end = rd32(head + 8);
        uint8_t *b = end > SAV_TABLE && end <= TABLE_MAX ? malloc(end) : NULL;
        Sav s;
        uint32_t child;
        if (b && plat_file_pread(f, b, end, 0) == (int64_t)end && sav_open(&s, b, end) &&
            sav_get(&s, "Sequence_IsChildhood", &child))
            adult = child == 0;
        free(b);
    }
    plat_file_close(f);
    return adult;
}

void save_summary(const char *dir, SaveSummary *out) {
    memset(out, 0, sizeof *out);
    out->adult = out->life = out->max_life = -1;
    Slots sl = {.n = 0};
    plat_fs_list(dir, slot_cb, &sl);
    char path[PLAT_PATH_MAX * 3], newest[8] = "";
    uint64_t best = 0;
    for (int i = 0; i < sl.n; i++) {
        if (!path_of(path, sizeof path, dir, sl.names[i], "progress.sav") || !plat_file_exists(path)) continue;
        out->slots++;
        uint64_t size = 0;
        uint8_t *b = path_of(path, sizeof path, dir, sl.names[i], "caption.sav") ? plat_file_read(path, &size) : NULL;
        Sav s;
        if (sav_open(&s, b, (size_t)size)) {
            int y = sav_int(&s, "CaptionData.Date.Year", 0), mo = sav_int(&s, "CaptionData.Date.Month", 0),
                d = sav_int(&s, "CaptionData.Date.Day", 0), h = sav_int(&s, "CaptionData.Date.Hour", 0),
                mi = sav_int(&s, "CaptionData.Date.Minute", 0), se = sav_int(&s, "CaptionData.Date.Second", 0);
            uint64_t when = ((((((uint64_t)(y & 0xffff) * 16 + (mo & 15)) * 32 + (d & 31)) * 32 + (h & 31)) * 64 +
                             (mi & 63)) * 64 + (se & 63)) + 1;
            if (when > best) {
                best = when;
                memcpy(newest, sl.names[i], 8);
                out->year = y, out->month = mo, out->day = d, out->hour = h, out->minute = mi;
                out->life = sav_int(&s, "CaptionData.Life", -1);
                out->max_life = sav_int(&s, "CaptionData.MaxLife", -1);
                char id[72];
                if (sav_string(&s, "CaptionData.LocationName", id, sizeof id)) area_words(id, out->area, sizeof out->area);
                else out->area[0] = 0;
            }
        }
        free(b);
    }
    if (*newest && path_of(path, sizeof path, dir, newest, "progress.sav")) out->adult = progress_adult(path);
}
