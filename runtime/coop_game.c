// Co-op progress sync, the game side (coop_game.h), for Ocarina 1.1.1.
//
// The flag database. The game keeps its progress in one manager, *(u64 *)(main+0x4e842a8), as typed containers of 0x60
// bytes from +8: Bool at +0x008, Int at +0x0c8, UInt at +0x788. A container holds its entry count (+0x08), entries
// (+0x10), an open-addressing table of {u32 hash, u32 entry} slots probed from hash % capacity (+0x18, capacity +0x24) and a
// ring of pending sets (+0x28 capacity, +0x30 slots, +0x38 counter, its low 20 bits the count). A Bool entry is 0x18 bytes
// (+0x08 the reset kinds, +0x10 the save file, -1 none, +0x15 default, +0x16 current value), an Int or UInt entry 0x28
// (+0x08, +0x10 the same, +0x18 default, +0x1c current). Structs group flags: the struct table (+0xc10 count, +0xc18
// entries of 0x20, +0xc20 slots, +0xc2c capacity) maps a struct's hash to an entry whose member table (+0x08, capacity +0x14)
// holds {u32 member hash, u32 flag or struct hash} slots. Flags are named by MurmurHash3 of their names; the names stay out
// of this file, the hashes it needs are numbers below.
//
// Writes go through the game's own set routines, SetBool (main+0x682b20) and SetInt (main+0x163b70), with x0 the manager,
// x1 the value and x2 the hash: they queue the change on the ring and the game's per-frame drain (main+0xb99980) writes the
// current and saved values and its own bookkeeping, as for a change the game made. Reads are of the entries' current values.
//
// What is shared (docs/coop.md has the reasons):
// - progress: every Bool that goes to progress.sav (save file 0) and is reset only by a new game or a load (reset kinds 8,
//   16 and 32; flags with kind 1 or 256 revert at scene changes, 2 at a new day, 4 are options), except the songs, the
//   first-get markers and the heart rewards below. Its register is 1 once the flag differs from its default (63 of them
//   start true and count down). Story, world and dungeon flags are one category: the game's flags carry no mark of which
//   is which, and their names do not reach the runtime.
// - items: the item counts of the pouch struct (except the lists below), the capacity struct (wallet, bag and quiver
//   sizes), the magic meter's size, and the first-get struct's markers; the highest count wins.
// - songs: 13 first-get markers. quest: 11 pouch counts (medallions, stones, the stone of agony, the gerudo card).
// - keys: the small-key count of each dungeon (the dungeon struct's member); rupees: the wallet's count; ammunition: 7
//   pouch counts. These three are counters the game spends: shared, they are pools (coop_state.h), else per player.
// - per player always: the trade sequence, rented masks, the knife that breaks, amiibo items and one unknown seed item
//   (a highest-count merge would hand back what the game took away); heart pieces and gold skulltula tokens, and the 11
//   heart reward flags (their pickups are mostly per-object flags this layer does not write, so a shared count would pay a
//   piece twice); life, the magic in the meter, bottle contents, equipment worn, the time of day, Link's place.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_game.h"
#include "coop_state.h"
#include "platform/platform.h"

#define MGR_PTR 0x7004e842a8ull
#define SET_BOOL_FN 0x7000682b20ull
#define SET_INT_FN 0x7000163b70ull

enum {
    C_BOOL = 0x008, C_INT = 0x0c8, C_UINT = 0x788,
    CN_COUNT = 0x08, CN_ENTRIES = 0x10, CN_TABLE = 0x18, CN_CAP = 0x24, CN_RING_CAP = 0x28, CN_RING = 0x30, CN_RING_CTR = 0x38,
    BOOL_SIZE = 0x18, INT_SIZE = 0x28,
    E_RESET = 0x08, E_FILE = 0x10, B_DEFAULT = 0x15, B_CURRENT = 0x16, I_CURRENT = 0x1c,
    ST_COUNT = 0xc10, ST_ENTRIES = 0xc18, ST_TABLE = 0xc20, ST_CAP = 0xc2c, ST_SIZE = 0x20, STE_MEMBERS = 0x08, STE_MCAP = 0x14,
    MGR_SIZE = 0xd80,
    RESET_NEW_GAME = 8 | 16 | 32, RESET_TEMPORARY = 1 | 2 | 4 | 256,
    SLICES = 4,             // a quarter of the flags is read each frame: every flag every 4 frames
    APPLY_PER_FRAME = 16,
    INT_MAX_ITEM = 999,     // an item count above this from the network is not taken
    CLOCK_MS = 2500,        // the game clock advanced this recently: the game is being played
};

// Struct and member hashes (MurmurHash3 of the names the game uses).
#define S_POUCH 0x3da094e3u      // item counts, one struct per item
#define M_STOCK 0x22344481u      // an item's count
#define S_FIRST_GET 0xade0ef78u  // "got it once" markers, one per item
#define S_DUNGEON 0xf5a32cdcu    // one struct per dungeon: map, compass, boss key, small keys
#define M_SMALL_KEYS 0x1af87831u
#define S_CAPACITY 0xf80f8077u   // wallet, bomb bag, quiver, seed bag, stick and nut capacities
#define F_RUPEES 0xa77921d7u
#define F_MAGIC_SIZE 0x12d63a97u
#define F_PLAY_CLOCK 0xe53eb6d1u // UInt: seconds played, advanced while the game runs
#define F_LOADING 0xf5deef0au    // Bool: a save is being loaded

// Scene changes in progress (Bool, not saved): nothing is read or written while one is set.
static const uint32_t transition_flags[] = {0x9a9506e7, 0xf8633c7f, 0xf96989b6, 0xe19dfdd6};
static const uint32_t song_flags[] = {0x01c20002, 0xb492ac33, 0xd3f8bfcc, 0x165dd1ad, 0x03b3450e, 0x2b39dd4f, 0xdc0a0b54,
                                      0x49041ab9, 0x3fa63986, 0xc2fb95f4, 0x802a36a8, 0xd6320ba4, 0x5e86fb4d};
static const uint32_t heart_reward_flags[] = {0x6b8fc751, 0x1d1793e9, 0xb5127e75, 0xe41e1597, 0x98ca7aab, 0xf1891f6e,
                                              0x3f43245b, 0x1657d7d8, 0xa666baa8, 0x5ff7aa30, 0xf2fab510};
static const uint32_t quest_counts[] = {0x692d57cb, 0x5ce254a6, 0x0151a68f, 0x0c7a5fd0, 0x204d2063, 0x235c7d9a,
                                        0xcbd1e58c, 0x7d69ad47, 0xb8e3bf45, 0xf3b14ce2, 0xfeb4d56b};
static const uint32_t own_counts[] = {0x07f10aaf, 0x0165e7ca, 0x51d89ef0, 0x07de3e6f, 0x58c9bc78, 0xa862d276, 0x2815c82a,
                                      0x897cf7eb, 0x919ea324, 0x5c41a6b1, 0xded8e8d3, 0x24596e8e, 0xb6ca6093, 0x6be5c3b0,
                                      0x7e607033, 0x0e94a8df, 0xfc508da7, 0xb5c10899, 0x47b2d775, 0xf046629e, 0x96459cd3,
                                      0x924e9a46, 0xf5e647da, 0x486c59a4, 0x49d1c374, 0x20820479, 0x396a5c02, 0x8d02f5ea,
                                      0x600e29eb, 0x51ec97b3, 0x04735978};
// Ammunition counts and the capacity member that caps each (0: none known).
static const struct { uint32_t count, cap_member; } ammo[] = {
    {0xdb1bcd4f, 0xc0f40041}, {0xdf7c517a, 0}, {0x518ec74b, 0x59663556}, {0xf30d9f54, 0x83d2e406},
    {0x5fadf7fc, 0x483d0f84}, {0xb9494411, 0xf9e54315}, {0xb003124d, 0},
};
#define CAP_RUPEES 0x3b58ca14u   // the capacity member of the wallet

static int in_list(const uint32_t *list, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}
#define IN(list, h) in_list(list, sizeof list / sizeof list[0], h)

typedef struct {
    uint32_t hash;
    uint8_t is_int, cat, inv;
    int32_t last;           // the raw value last reported
    uint64_t addr;          // the entry
    uint64_t cap_addr;      // a pool counter's capacity entry (Int), 0 none
} GFlag;

// A set of hashes (open addressing, 0 empty), for the struct memberships while the table is built.
typedef struct { uint32_t *h; unsigned cap; } HSet;
static void hset_add(HSet *s, uint32_t h) {
    if (!h || !s->h) return;
    unsigned i = h & (s->cap - 1);
    while (s->h[i] && s->h[i] != h) i = (i + 1) & (s->cap - 1);
    s->h[i] = h;
}
static int hset_has(const HSet *s, uint32_t h) {
    if (!h || !s->h) return 0;
    unsigned i = h & (s->cap - 1);
    while (s->h[i]) {
        if (s->h[i] == h) return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

typedef struct { uint32_t hash; int32_t value; uint32_t frame; int done; } TestSet;

static struct {
    // settings, re-read when cfg_version changes (the game thread's copy and the coop thread's)
    unsigned cfg_seen, cfg_seen_net;
    int on, said_version;
    TestSet test[8];        // NX2NAVI_COOP_TEST_SET
    int ntest;

    PlatMutex lock;         // around eng
    CoopSync *eng;
    uint64_t self_id;

    // the game thread's
    uint64_t mgr, bool_entries, int_entries;
    GFlag *flags;           // flags first, then the pool counters
    int *by_hash;           // indices of flags in hash order
    int nflags, npools, cap;
    unsigned count[COOP_CAT_N];
    int bound, gate, slice;
    uint32_t clock;
    uint64_t clock_at, clock_entry, loading_entry, transition_entry[4];
    uint32_t frames;        // frames played since the save was bound
    unsigned applied[COOP_CAT_N], applied_total;
    uint64_t applied_log_at;
    unsigned rules_seen;
} G;

#define TRACE_ON (rt_trace & RT_TRACE_COOP)

// ---- reading the database

static int rd_ok(uint64_t a, uint64_t n) { return a >= 0x1000 && mem_readable(a, n); }

// The entry of hash in the container at base (stride bytes per entry), 0 when absent.
static uint64_t entry_of(uint64_t mgr, unsigned base, unsigned stride, uint32_t hash) {
    uint64_t c = mgr + base, table = LD64(c + CN_TABLE), entries = LD64(c + CN_ENTRIES);
    uint32_t cap = LD32(c + CN_CAP), count = LD32(c + CN_COUNT);
    if (!hash || !cap || !table || !entries) return 0;
    for (uint32_t i = hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == hash) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            return entries + (uint64_t)(idx < count ? idx : 0) * stride;
        }
        if (!h) return 0;
    }
    return 0;
}

static uint64_t struct_of(uint64_t mgr, uint32_t hash) {
    uint64_t table = LD64(mgr + ST_TABLE), entries = LD64(mgr + ST_ENTRIES);
    uint32_t cap = LD32(mgr + ST_CAP), count = LD32(mgr + ST_COUNT);
    if (!cap || !table || !entries || !rd_ok(table, 8ull * cap)) return 0;
    for (uint32_t i = hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == hash) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            uint64_t e = entries + (uint64_t)(idx < count ? idx : 0) * ST_SIZE;
            return rd_ok(e, ST_SIZE) ? e : 0;
        }
        if (!h) return 0;
    }
    return 0;
}

// fn(member hash, flag or struct hash, user) for each member of struct entry e.
static void members(uint64_t e, void (*fn)(uint32_t m, uint32_t v, void *u), void *u) {
    uint64_t t = LD64(e + STE_MEMBERS);
    uint32_t cap = LD32(e + STE_MCAP);
    if (!t || !cap || cap > 65536 || !rd_ok(t, 8ull * cap)) return;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t m = LD32(t + 8ull * i);
        if (m) fn(m, LD32(t + 8ull * i + 4), u);
    }
}

typedef struct { uint64_t mgr; HSet *set; uint32_t want; } Walk;
static void add_member(uint32_t m, uint32_t v, void *u) { (void)m, hset_add(((Walk *)u)->set, v); }
static void pick(uint32_t m, uint32_t v, void *u) {
    Walk *w = u;
    if (m == w->want) hset_add(w->set, v);
}
// A struct of structs: the flag of member want in each.
static void add_nested(uint32_t m, uint32_t v, void *u) {
    (void)m;
    Walk *w = u;
    uint64_t e = struct_of(w->mgr, v);
    if (e) members(e, pick, w);
}

typedef struct { uint64_t mgr; uint32_t member; uint64_t flag; } FindMember;
static void find_member(uint32_t m, uint32_t v, void *u) {
    FindMember *f = u;
    if (m == f->member) f->flag = entry_of(f->mgr, C_INT, INT_SIZE, v);
}
// The Int entry a struct member names, 0 when absent.
static uint64_t member_entry(uint64_t mgr, uint32_t s, uint32_t member) {
    uint64_t e = struct_of(mgr, s);
    FindMember f = {mgr, member, 0};
    if (e) members(e, find_member, &f);
    return f.flag;
}

static GFlag *add_flag(void) {
    if (G.nflags == G.cap) {
        int nc = G.cap ? G.cap * 2 : 4096;
        GFlag *nf = realloc(G.flags, (size_t)nc * sizeof *nf);
        if (!nf) return NULL;
        G.flags = nf, G.cap = nc;
    }
    GFlag *f = &G.flags[G.nflags++];
    memset(f, 0, sizeof *f);
    return f;
}

static int cmp_pool_last(const void *a, const void *b) {
    return coop_cat_is_pool(((const GFlag *)a)->cat) - coop_cat_is_pool(((const GFlag *)b)->cat);
}

static int cmp_by_hash(const void *a, const void *b) {
    uint32_t x = G.flags[*(const int *)a].hash, y = G.flags[*(const int *)b].hash;
    return x < y ? -1 : x > y;
}

// The classified table of the manager's flags (the header comment's rules). 0 when the database is not readable.
static int build_table(uint64_t mgr) {
    G.nflags = G.npools = 0;
    memset(G.count, 0, sizeof G.count);
    HSet first_get = {0}, pouch = {0}, keys = {0}, capacity = {0};
    HSet *sets[] = {&first_get, &pouch, &keys, &capacity};
    for (int i = 0; i < 4; i++) {
        sets[i]->cap = 1024;
        sets[i]->h = calloc(sets[i]->cap, sizeof *sets[i]->h);
    }
    uint64_t e;
    Walk w = {mgr, &first_get, 0};
    if ((e = struct_of(mgr, S_FIRST_GET))) members(e, add_member, &w);
    w.set = &pouch, w.want = M_STOCK;
    if ((e = struct_of(mgr, S_POUCH))) members(e, add_nested, &w);
    w.set = &keys, w.want = M_SMALL_KEYS;
    if ((e = struct_of(mgr, S_DUNGEON))) members(e, add_nested, &w);
    w.set = &capacity;
    if ((e = struct_of(mgr, S_CAPACITY))) members(e, add_member, &w);

    for (int type = 0; type < 2; type++) {
        unsigned base = type ? C_INT : C_BOOL, stride = type ? INT_SIZE : BOOL_SIZE;
        uint64_t c = mgr + base, table = LD64(c + CN_TABLE), entries = LD64(c + CN_ENTRIES);
        uint32_t cap = LD32(c + CN_CAP), count = LD32(c + CN_COUNT);
        if (!cap || !count || count > 1000000 || !rd_ok(table, 8ull * cap) || !rd_ok(entries, (uint64_t)count * stride)) continue;
        for (uint32_t i = 0; i < cap; i++) {
            uint32_t h = LD32(table + 8ull * i), idx = LD32(table + 8ull * i + 4);
            if (!h || idx >= count) continue;
            uint64_t a = entries + (uint64_t)idx * stride;
            uint32_t reset = LD32(a + E_RESET);
            int32_t file = (int32_t)LD32(a + E_FILE);
            int cat = -1;
            uint8_t inv = 0;
            uint64_t cap_addr = 0;
            if (!type) {
                if (file != 0 || reset & RESET_TEMPORARY || !(reset & RESET_NEW_GAME) || IN(heart_reward_flags, h)) continue;
                cat = IN(song_flags, h) ? COOP_CAT_SONGS : hset_has(&first_get, h) ? COOP_CAT_ITEMS : COOP_CAT_PROGRESS;
                inv = LD8(a + B_DEFAULT) != 0;
            } else if (h == F_RUPEES) {
                cat = COOP_CAT_RUPEES;
                cap_addr = member_entry(mgr, S_CAPACITY, CAP_RUPEES);
            } else if (hset_has(&keys, h)) {
                cat = COOP_CAT_KEYS;
            } else if (hset_has(&pouch, h)) {
                if (IN(own_counts, h)) continue;
                cat = IN(quest_counts, h) ? COOP_CAT_QUEST : COOP_CAT_ITEMS;
                for (size_t k = 0; k < sizeof ammo / sizeof ammo[0]; k++)
                    if (ammo[k].count == h) {
                        cat = COOP_CAT_AMMO;
                        if (ammo[k].cap_member) cap_addr = member_entry(mgr, S_CAPACITY, ammo[k].cap_member);
                    }
            } else if (hset_has(&capacity, h) || h == F_MAGIC_SIZE) {
                cat = COOP_CAT_ITEMS;
            }
            if (cat < 0) continue;
            GFlag *f = add_flag();
            if (!f) break;
            f->hash = h, f->is_int = (uint8_t)type, f->cat = (uint8_t)cat, f->inv = inv, f->addr = a, f->cap_addr = cap_addr;
            G.count[cat]++;
        }
    }
    for (int i = 0; i < 4; i++) free(sets[i]->h);
    qsort(G.flags, (size_t)G.nflags, sizeof *G.flags, cmp_pool_last);   // the scan reads flags in slices, the pools every frame
    for (int i = 0; i < G.nflags; i++) G.npools += coop_cat_is_pool(G.flags[i].cat);
    free(G.by_hash);
    G.by_hash = malloc((size_t)G.nflags * sizeof *G.by_hash);
    if (!G.by_hash) return 0;
    for (int i = 0; i < G.nflags; i++) G.by_hash[i] = i;
    qsort(G.by_hash, (size_t)G.nflags, sizeof *G.by_hash, cmp_by_hash);
    return G.nflags > 0;
}

static int32_t raw_of(const GFlag *f) { return f->is_int ? (int32_t)LD32(f->addr + I_CURRENT) : LD8(f->addr + B_CURRENT); }
static uint32_t reg_of(const GFlag *f, int32_t raw) {
    if (!f->is_int) return (raw != 0) != f->inv;   // 1: differs from its default
    return raw > 0 ? (uint32_t)raw : 0;
}

static GFlag *flag_by_hash(uint32_t hash) {
    int lo = 0, hi = G.nflags - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        GFlag *f = &G.flags[G.by_hash[mid]];
        if (f->hash == hash) return f;
        if (f->hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// A set of this hash waits on the Int ring (the game changed it this frame): a pool write now would overwrite it.
static int int_pending(uint64_t mgr, uint32_t hash) {
    uint64_t c = mgr + C_INT, ring = LD64(c + CN_RING);
    uint32_t n = LD32(c + CN_RING_CTR) & 0xFFFFF, cap = LD32(c + CN_RING_CAP);
    if (!ring || n > cap || !rd_ok(ring, 12ull * n)) return 1;
    for (uint32_t i = 0; i < n; i++)
        if (LD32(ring + 12ull * i + 4) == hash) return 1;
    return 0;
}

// ---- settings

static int mode_of(const char *key, int pool) {
    const char *v = cfg_value(key);
    if (!strcmp(v, "own")) return COOP_MODE_OWN;
    return pool ? (!strcmp(v, "pool") ? COOP_MODE_POOL : COOP_MODE_OWN) : (!strcmp(v, "shared") ? COOP_MODE_SHARED : COOP_MODE_OWN);
}

static void read_rules(CoopRules *r) {
    static const char *const keys[COOP_CAT_N] = {"NX2NAVI_COOP_PROGRESS", "NX2NAVI_COOP_ITEMS", "NX2NAVI_COOP_SONGS", "NX2NAVI_COOP_QUEST",
                                                 "NX2NAVI_COOP_KEYS", "NX2NAVI_COOP_RUPEES", "NX2NAVI_COOP_AMMO"};
    coop_rules_default(r);
    for (int c = 0; c < COOP_CAT_N; c++) r->mode[c] = (uint8_t)mode_of(keys[c], coop_cat_is_pool(c));
    CoopRules as_set = *r;
    if (coop_rules_normalize(r)) {
        char a[200], b[200];
        coop_rules_text(&as_set, a, sizeof a);
        coop_rules_text(r, b, sizeof b);
        rt_log("coop: sync: the settings ask for %s; a room made with them uses %s", a, b);
    }
}

// "hash=value@frame" items separated by ',' or ';' (hash in hex): set that flag at that frame of play, for tests.
static void read_test_aid(void) {
    G.ntest = 0;
    const char *t = cfg_get("NX2NAVI_COOP_TEST_SET");
    while (t && *t && G.ntest < (int)(sizeof G.test / sizeof G.test[0])) {
        char *end;
        unsigned long h = strtoul(t, &end, 16);
        if (*end != '=') break;
        long v = strtol(end + 1, &end, 0);
        if (*end != '@') break;
        unsigned long fr = strtoul(end + 1, &end, 0);
        G.test[G.ntest++] = (TestSet){(uint32_t)h, (int32_t)v, (uint32_t)fr, 0};
        while (*end == ',' || *end == ';' || *end == ' ') end++;
        t = end;
    }
    if (t && *t) rt_log("coop: test: COOP_TEST_SET is \"hash=value@frame,...\" (hex hash); stopped at \"%s\"", t);
}

// ---- the engine's side of the coop thread

static void trace_line(void *user, const char *line) { (void)user, rt_log("coop: sync: %s", line); }

static int ensure_engine(void) {
    if (G.eng) return 1;
    uint64_t h = 0xcbf29ce484222325ull;   // the schema: game version and this file's classification
    const char *tag = "nx2navi-coop-sync/1";
    for (const char *p = tag; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    h = (h ^ (uint64_t)rt_game_version) * 0x100000001b3ull;
    G.self_id = coop_client_id();
    CoopSync *e = coop_sync_new(G.self_id, (uint32_t)(h ^ h >> 32));
    if (!e) return 0;
    if (TRACE_ON) coop_sync_trace(e, trace_line, NULL);
    plat_mutex_lock(&G.lock);
    G.eng = e;
    plat_mutex_unlock(&G.lock);
    return 1;
}

static int send_fn(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    return member ? coop_send_to(member, kind, bytes, n) : coop_send(kind, bytes, n);
}

static void on_message(int from, const uint8_t *bytes, size_t n, void *user) {
    uint8_t kind = (uint8_t)(uintptr_t)user;
    plat_mutex_lock(&G.lock);
    if (G.eng) coop_sync_receive(G.eng, from, kind, bytes, n, plat_now_ms());
    plat_mutex_unlock(&G.lock);
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    int ids[COOP_MAX_MEMBERS], n = 0;
    int in_room = v->state == COOP_JOINED || v->state == COOP_RECONNECTING;
    for (int i = 0; in_room && i < v->n; i++) ids[n++] = v->member[i].id;
    plat_mutex_lock(&G.lock);
    if (G.eng) {
        coop_sync_room(G.eng, in_room ? v->code : "", v->self, ids, n, plat_now_ms());
        CoopRules mine;
        read_rules(&mine);
        coop_sync_settings(G.eng, &mine, in_room && v->made);
    }
    plat_mutex_unlock(&G.lock);
}

static void on_tick(uint64_t now, void *user) {
    (void)user;
    unsigned v = cfg_version();
    plat_mutex_lock(&G.lock);
    if (!G.eng) {
        plat_mutex_unlock(&G.lock);
        return;
    }
    if (v != G.cfg_seen_net) {   // a rule setting changed: the room's maker passes it on
        G.cfg_seen_net = v;
        CoopView view;
        coop_view_get(&view);
        CoopRules mine;
        read_rules(&mine);
        coop_sync_settings(G.eng, &mine, (view.state == COOP_JOINED || view.state == COOP_RECONNECTING) && view.made);
    }
    coop_sync_tick(G.eng, now, send_fn, NULL);
    CoopRules r;
    unsigned ver = coop_sync_rules(G.eng, &r);
    if (ver != G.rules_seen) {
        G.rules_seen = ver;
        if (r.epoch) {
            char text[200];
            coop_rules_text(&r, text, sizeof text);
            rt_log("coop: sync: the room's rules: %s%s", text, r.owner == G.self_id ? " (this player's settings)" : "");
        }
    }
    plat_mutex_unlock(&G.lock);
}

void coop_game_init(void) {
    plat_mutex_init(&G.lock);
    G.cfg_seen = ~0u;
    coop_on_message(COOP_K_DIGEST, on_message, (void *)(uintptr_t)COOP_K_DIGEST);
    coop_on_message(COOP_K_SECTION, on_message, (void *)(uintptr_t)COOP_K_SECTION);
    coop_on_view(on_view, NULL);
    coop_on_tick(on_tick, NULL);
}

// ---- the game thread

static void unbind(const char *why) {
    if (!G.bound) return;
    G.bound = 0;
    rt_log("coop: sync: %s; nothing is read or written until a save is played", why);
}

static void bind(uint64_t mgr) {
    uint64_t be = LD64(mgr + C_BOOL + CN_ENTRIES), ie = LD64(mgr + C_INT + CN_ENTRIES);
    if (mgr != G.mgr || be != G.bool_entries || ie != G.int_entries || !G.nflags) {
        if (!build_table(mgr)) return;
        G.mgr = mgr, G.bool_entries = be, G.int_entries = ie;
    }
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&G.lock);
    coop_sync_rebind(G.eng);
    for (int i = 0; i < G.nflags; i++) {
        GFlag *f = &G.flags[i];
        f->last = raw_of(f);
        if (!coop_cat_is_pool(f->cat) && reg_of(f, f->last)) coop_sync_local(G.eng, f->cat, f->hash, reg_of(f, f->last), now);
    }
    plat_mutex_unlock(&G.lock);
    G.bound = 1, G.frames = 0, G.slice = 0;
    rt_log("coop: sync: playing a save: %u progress flags, %u items, %u songs, %u quest items, %u key counters, rupees, %u "
           "ammunition counters; the room's rules decide which are shared",
           G.count[COOP_CAT_PROGRESS], G.count[COOP_CAT_ITEMS], G.count[COOP_CAT_SONGS], G.count[COOP_CAT_QUEST],
           G.count[COOP_CAT_KEYS], G.count[COOP_CAT_AMMO]);
}

// The game is being played: the clock runs, no save is loading, no scene is changing. Notes a save change (unbind).
static int gate(uint64_t mgr) {
    if (mgr != G.mgr || !G.clock_entry) {   // the addresses of the few flags read every frame
        G.clock_entry = entry_of(mgr, C_UINT, INT_SIZE, F_PLAY_CLOCK);
        G.loading_entry = entry_of(mgr, C_BOOL, BOOL_SIZE, F_LOADING);
        for (int i = 0; i < 4; i++) G.transition_entry[i] = entry_of(mgr, C_BOOL, BOOL_SIZE, transition_flags[i]);
        if (!G.clock_entry || !rd_ok(G.clock_entry, INT_SIZE)) return 0;
    }
    uint64_t now = plat_now_ms();
    uint32_t clock = LD32(G.clock_entry + I_CURRENT);
    int loading = G.loading_entry && LD8(G.loading_entry + B_CURRENT);
    unsigned moving = 0;
    for (int i = 0; i < 4; i++) moving |= (unsigned)(G.transition_entry[i] && LD8(G.transition_entry[i] + B_CURRENT)) << i;
    if (clock != G.clock) {
        // the clock counts seconds of play one by one; any other change is a save being loaded (a load, a new game, the
        // same save again from where it was saved), which also sets the flags: play starts at the next tick
        int tick = clock > G.clock && clock - G.clock <= 5;
        if (!tick) unbind("another save was loaded");
        G.clock = clock;
        G.clock_at = tick ? now : 0;
    }
    if (loading) unbind("a save is loading");
    int open = !loading && !moving && G.clock_at && now - G.clock_at < CLOCK_MS;
    if (open != G.gate && TRACE_ON)
        rt_log("coop: sync: game %s (play clock %u, loading %d, scene change %#x)", open ? "running" : "paused", clock, loading, moving);
    G.gate = open;
    return open;
}

// Calls a set routine of the game with the guest's registers kept as they were at the present (as res_present does).
static void set_flag(Cpu *c, uint64_t mgr, int is_int, uint32_t hash, int32_t value) {
    rt_call(c, is_int ? SET_INT_FN : SET_BOOL_FN, 3, mgr, (uint64_t)(uint32_t)value, (uint64_t)hash);
}

static void run_test_aid(Cpu *c, uint64_t mgr) {
    for (int i = 0; i < G.ntest; i++) {
        if (G.test[i].done || G.frames < G.test[i].frame) continue;
        G.test[i].done = 1;
        uint32_t h = G.test[i].hash;
        int is_int = entry_of(mgr, C_BOOL, BOOL_SIZE, h) ? 0 : entry_of(mgr, C_INT, INT_SIZE, h) ? 1 : -1;
        if (is_int < 0) {
            rt_log("coop: test: no Bool or Int flag %08x", h);
            continue;
        }
        set_flag(c, mgr, is_int, h, G.test[i].value);
        rt_log("coop: test: set %s %08x = %d at frame %u", is_int ? "Int" : "Bool", h, G.test[i].value, G.frames);
    }
}

static void apply(Cpu *c, uint64_t mgr) {
    CoopApply a[APPLY_PER_FRAME];
    plat_mutex_lock(&G.lock);
    int n = coop_sync_take(G.eng, a, APPLY_PER_FRAME);
    plat_mutex_unlock(&G.lock);
    for (int i = 0; i < n; i++) {
        GFlag *f = flag_by_hash(a[i].hash);
        if (!f || f->cat != a[i].cat) continue;   // classified differently here: not taken
        int32_t v = a[i].value;
        if (a[i].pool) {
            if (int_pending(mgr, f->hash)) {
                plat_mutex_lock(&G.lock);
                coop_sync_untake(G.eng, &a[i]);
                plat_mutex_unlock(&G.lock);
                continue;
            }
        } else if (!f->is_int) {
            if (v != 1) continue;
            v = !f->inv;
        } else if (v > INT_MAX_ITEM) {
            continue;
        }
        set_flag(c, mgr, f->is_int, f->hash, v);
        if (a[i].pool) {
            plat_mutex_lock(&G.lock);
            coop_sync_pool_wrote(G.eng, f->hash, v);
            plat_mutex_unlock(&G.lock);
        }
        G.applied[f->cat]++, G.applied_total++;
        if (TRACE_ON) rt_log("coop: sync: applied %s %08x = %d", coop_cat_name(f->cat), f->hash, v);
    }
    uint64_t now = plat_now_ms();
    if (G.applied_total && now - G.applied_log_at >= 5000) {
        char text[200];
        size_t at = 0;
        for (int k = 0; k < COOP_CAT_N; k++)
            if (G.applied[k] && at < sizeof text)
                at += (size_t)snprintf(text + at, sizeof text - at, "%s%s %u", at ? ", " : "", coop_cat_name(k), G.applied[k]);
        rt_log("coop: sync: took %u values from the room (%s)", G.applied_total, text);
        memset(G.applied, 0, sizeof G.applied);
        G.applied_total = 0;
        G.applied_log_at = now;
    }
}

static void scan(uint64_t now) {
    int nflat = G.nflags - G.npools;
    int from = nflat * G.slice / SLICES, to = nflat * (G.slice + 1) / SLICES;
    G.slice = (G.slice + 1) % SLICES;
    plat_mutex_lock(&G.lock);
    for (int i = from; i < to; i++) {
        GFlag *f = &G.flags[i];
        int32_t raw = raw_of(f);
        if (raw == f->last) continue;
        f->last = raw;
        coop_sync_local(G.eng, f->cat, f->hash, reg_of(f, raw), now);
    }
    for (int i = nflat; i < G.nflags; i++) {
        GFlag *f = &G.flags[i];
        f->last = raw_of(f);
        int32_t cap = f->cap_addr ? (int32_t)LD32(f->cap_addr + I_CURRENT) : -1;
        coop_sync_pool_local(G.eng, f->cat, f->hash, f->last, cap > 0 ? cap : -1, now);
    }
    plat_mutex_unlock(&G.lock);
}

void coop_present(Cpu *c) {
    unsigned v = cfg_version();
    if (v != G.cfg_seen) {
        G.cfg_seen = v;
        G.on = cfg_bool("NX2NAVI_COOP");
        if (G.on) read_test_aid();
    }
    if (!G.on) return;
    if (!RT_V111) {
        if (!G.said_version) rt_log("coop: sync: progress sync knows game 1.1.1 only; this is %d", rt_game_version);
        G.said_version = 1;
        return;
    }
    if (!G.eng && !ensure_engine()) return;
    uint64_t mgr = LD64(MGR_PTR);
    if (!rd_ok(mgr, MGR_SIZE)) {
        unbind("the game's flags are gone");
        return;
    }
    if (G.bound && (mgr != G.mgr || LD64(mgr + C_BOOL + CN_ENTRIES) != G.bool_entries || LD64(mgr + C_INT + CN_ENTRIES) != G.int_entries))
        unbind("the game made its flags anew");
    if (!gate(mgr)) return;
    if (!G.bound) bind(mgr);
    if (!G.bound) return;
    Cpu keep = *c;
    G.frames++;
    run_test_aid(c, mgr);
    scan(plat_now_ms());
    apply(c, mgr);
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
}
