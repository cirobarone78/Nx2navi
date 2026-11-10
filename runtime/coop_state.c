// Co-op progress sync: the merge engine (coop_state.h). Pure: no game, no sockets, no threads, no clock of its own.
//
// Registers live in one array that only grows (an index stays valid) with an open-addressing index over (kind, hash,
// contributor). A register's section and its share of the section's sum depend only on its key and value, so the sums are
// kept up to date at every change and recomputed when the rules change which categories count.
//
// Wire format (little-endian), after the peer message's kind byte (coop_proto.h):
//   DIGEST   u8 version, u32 schema, u32 epoch, u8 ncat, ncat x u8 mode, u8 nsec, nsec x u32 sum
//   SECTION  u8 version, u32 schema, u8 n, n x entry
//   entry    u8 kind, u8 category, u32 hash, [u64 contributor: gain and spend only], u32 value
// A pool's base is a signed value stored with its top bit flipped, so the highest unsigned value is the highest signed one.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_proto.h"
#include "coop_state.h"

enum {
    MAX_REGS = 1 << 16,          // a hostile peer cannot grow the state past this; the game has about 12000 flags
    MAX_POOLS = 128,             // the game has 22 counters; an entry for a counter it does not have is refused
    MAX_CONTRIB = 256,           // gain and spend registers of one pool: each binding of a save that gains or spends adds one
    POOL_REG_MAX = 1 << 20,      // a contributor's gains or spends at most, and the lowest base
    PEER_RATE = 49152,           // message bytes per second taken from one peer (it sends at most 32 KB/s)
    PEER_BURST = 65536,
    BAD_MAX = 32,                // values a member sent that this game refuses: past this many it is ignored
    PUSH_SLOTS = 4096,           // local changes waiting for their three sends
    DIGEST_MS = 2000,            // every digest_ms plus up to 400 ms
    ANSWER_MS = 500,             // a section goes to the same peer at most this often
    RATE_BYTES = 32768,          // section and push bytes per second, under the relay's 64 KB/s per member
    BURST_BYTES = 8192,
    MSG_CAP = COOP_MAX_MESSAGE,
    SYNC_HDR = 6,
};

static const char *const cat_names[COOP_CAT_N] = {"progress", "items", "songs", "quest", "keys", "rupees", "ammo"};

typedef struct {
    uint32_t hash, s, l, applied;   // s merged, l local (flags), applied: the value last handed to the game
    uint64_t who;                   // the contributor of a gain or spend, else 0
    uint8_t kind, cat, queued;
} Reg;

typedef struct {
    uint32_t hash;
    uint8_t cat, bound, queued, have_local;
    int32_t local, expected, cap, max;   // max: the highest value the game takes for this counter
    int contrib;                    // gain and spend registers
    uint64_t me;                    // this binding's contributor id
    int64_t gains, spends;          // over every contributor
    int base;                       // the base register's index, -1 none
} Pool;

typedef struct {
    int present, have_digest;
    uint32_t sums[COOP_SYNC_SECTIONS];
    uint64_t want;                  // sections to send it, a bit each
    uint64_t answered_at[COOP_SYNC_SECTIONS];
    uint64_t digest_at;             // the last digest sent to it alone
    int64_t tokens;                 // its message bytes' budget (PEER_RATE)
    uint64_t tok_at;
    unsigned bad;                   // values it sent that this game refuses
    int ignored;                    // past BAD_MAX of them: nothing more is taken from it while it is in the room
} Peer;

typedef struct { int idx; uint8_t round; uint64_t due, first; } Push;

struct CoopSync {
    uint64_t self_id;
    uint32_t schema;
    CoopSyncLimitFn limit;          // the local game's table: what it shares and up to which value
    void *limit_user;
    uint64_t ik0, ik1;              // the index's key
    Reg *regs;
    int nregs, cap;
    int *index;                     // reg index + 1, 0 empty
    int index_cap;
    uint32_t sums[COOP_SYNC_SECTIONS];
    Pool pools[MAX_POOLS];
    int npools;
    uint64_t bindings;

    char code[COOP_SECRET_TEXT + 1];
    int in_room, self, caught_up;
    uint64_t joined_at;
    Peer peer[COOP_MAX_MEMBERS + 1];
    CoopRules room, mine;
    int have_room, have_mine, made;
    int maker;                      // the member id the relay names as the room's maker, 0 none in the room
    uint64_t originated_at;         // the maker makes its rules at most once a second
    unsigned rules_ver;

    Push push[PUSH_SLOTS];
    unsigned push_head, push_tail;
    int *apply;                     // ring of reg indices (>= 0) and pools (-1 - i)
    unsigned apply_head, apply_tail, apply_cap;
    uint64_t next_digest;
    uint64_t digest_to;             // members (bit per id) to send a digest now; bit 0: all
    int64_t tokens;
    uint64_t tok_at;
    uint64_t rng;

    void (*trace)(void *user, const char *line);
    void *trace_user;
    CoopSyncStats st;
};

// ---- small helpers

static uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static uint64_t key_of(uint8_t kind, uint32_t hash, uint64_t who) { return mix64(((uint64_t)kind << 56) ^ hash ^ mix64(who)); }
static int sec_of(const Reg *r) { return (int)(key_of(r->kind, r->hash, r->who) % COOP_SYNC_SECTIONS); }
static uint32_t share_of(const Reg *r, uint32_t v) { return v ? (uint32_t)mix64(key_of(r->kind, r->hash, r->who) ^ v) : 0; }

// SipHash-2-4 (Aumasson and Bernstein): a keyed hash for the index, so that slots cannot be chosen from outside.
#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND                                                                                                      \
    do {                                                                                                               \
        v0 += v1, v1 = SIP_ROTL(v1, 13), v1 ^= v0, v0 = SIP_ROTL(v0, 32), v2 += v3, v3 = SIP_ROTL(v3, 16), v3 ^= v2;  \
        v0 += v3, v3 = SIP_ROTL(v3, 21), v3 ^= v0, v2 += v1, v1 = SIP_ROTL(v1, 17), v1 ^= v2, v2 = SIP_ROTL(v2, 32);  \
    } while (0)
uint64_t coop_siphash24(uint64_t k0, uint64_t k1, const uint8_t *m, size_t n) {
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w = 0;
        for (int k = 0; k < 8; k++) w |= (uint64_t)m[i + k] << 8 * k;
        v3 ^= w;
        SIP_ROUND;
        SIP_ROUND;
        v0 ^= w;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t k = 0; i + k < n; k++) b |= (uint64_t)m[i + k] << 8 * k;
    v3 ^= b;
    SIP_ROUND;
    SIP_ROUND;
    v0 ^= b;
    v2 ^= 0xff;
    for (int r = 0; r < 4; r++) SIP_ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

static void put8(uint8_t **p, uint8_t v) { *(*p)++ = v; }
static void put32(uint8_t **p, uint32_t v) { for (int i = 0; i < 4; i++) *(*p)++ = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t **p, uint64_t v) { for (int i = 0; i < 8; i++) *(*p)++ = (uint8_t)(v >> 8 * i); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void tracef(CoopSync *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tracef(CoopSync *s, const char *fmt, ...) {
    if (!s->trace) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->trace(s->trace_user, line);
}

static int32_t base_decode(uint32_t v) { return (int32_t)(v ^ 0x80000000u); }
static uint32_t base_encode(int64_t v) {
    if (v < INT32_MIN + 1) v = INT32_MIN + 1;   // 0 stays "absent"
    if (v > INT32_MAX) v = INT32_MAX;
    return (uint32_t)(int32_t)v ^ 0x80000000u;
}

// ---- rules

const char *coop_cat_name(int cat) { return cat >= 0 && cat < COOP_CAT_N ? cat_names[cat] : "?"; }
const char *coop_mode_name(int mode) { return mode == COOP_MODE_SHARED ? "shared" : mode == COOP_MODE_POOL ? "pool" : "own"; }
int coop_cat_is_pool(int cat) { return cat == COOP_CAT_KEYS || cat == COOP_CAT_RUPEES || cat == COOP_CAT_AMMO; }

void coop_rules_default(CoopRules *r) {
    memset(r, 0, sizeof *r);
    for (int c = 0; c < COOP_CAT_N; c++) r->mode[c] = coop_cat_is_pool(c) ? COOP_MODE_OWN : COOP_MODE_SHARED;
    r->mode[COOP_CAT_KEYS] = COOP_MODE_POOL;
}

unsigned coop_rules_normalize(CoopRules *r) {
    CoopRules d;
    coop_rules_default(&d);
    unsigned changed = 0;
    for (int c = 0; c < COOP_CAT_N; c++) {
        int m = r->mode[c];
        int ok = coop_cat_is_pool(c) ? m == COOP_MODE_OWN || m == COOP_MODE_POOL : m == COOP_MODE_OWN || m == COOP_MODE_SHARED;
        if (!ok) r->mode[c] = d.mode[c], changed |= 1u << c;
    }
    if (r->mode[COOP_CAT_PROGRESS] == COOP_MODE_OWN) {
        if (r->mode[COOP_CAT_KEYS] != COOP_MODE_OWN) r->mode[COOP_CAT_KEYS] = COOP_MODE_OWN, changed |= 1u << COOP_CAT_KEYS;
    } else {
        static const int pickups[] = {COOP_CAT_ITEMS, COOP_CAT_SONGS, COOP_CAT_QUEST};
        for (int i = 0; i < 3; i++)
            if (r->mode[pickups[i]] != COOP_MODE_SHARED) r->mode[pickups[i]] = COOP_MODE_SHARED, changed |= 1u << pickups[i];
    }
    return changed;
}

void coop_rules_text(const CoopRules *r, char *out, size_t cap) {
    size_t at = 0;
    if (cap) out[0] = 0;
    for (int c = 0; c < COOP_CAT_N && at < cap; c++) {
        int k = snprintf(out + at, cap - at, "%s%s %s", c ? ", " : "", cat_names[c], coop_mode_name(r->mode[c]));
        if (k < 0) break;
        at += (size_t)k;
    }
}

// ---- registers

static int reg_synced(const CoopSync *s, const Reg *r) {
    if (!s->have_room) return 0;
    int m = s->room.mode[r->cat];
    return r->kind == COOP_R_FLAG ? m == COOP_MODE_SHARED : m == COOP_MODE_POOL;
}

// The slot of a register in the index: keyed with a key of this process, so that a peer that chooses contributor ids
// cannot pile its registers onto one run of slots.
static unsigned slot_of(const CoopSync *s, uint8_t kind, uint32_t hash, uint64_t who) {
    uint8_t m[13] = {kind};
    for (int i = 0; i < 4; i++) m[1 + i] = (uint8_t)(hash >> 8 * i);
    for (int i = 0; i < 8; i++) m[5 + i] = (uint8_t)(who >> 8 * i);
    return (unsigned)coop_siphash24(s->ik0, s->ik1, m, sizeof m);
}

static int find_reg(const CoopSync *s, uint8_t kind, uint32_t hash, uint64_t who) {
    if (!s->index_cap) return -1;
    unsigned mask = (unsigned)s->index_cap - 1, i = slot_of(s, kind, hash, who) & mask;
    for (;;) {
        int k = s->index[i];
        if (!k) return -1;
        const Reg *r = &s->regs[k - 1];
        if (r->kind == kind && r->hash == hash && r->who == who) return k - 1;
        i = (i + 1) & mask;
    }
}

static void index_insert(CoopSync *s, int idx) {
    const Reg *r = &s->regs[idx];
    unsigned mask = (unsigned)s->index_cap - 1, i = slot_of(s, r->kind, r->hash, r->who) & mask, steps = 1;
    while (s->index[i]) i = (i + 1) & mask, steps++;
    s->index[i] = idx + 1;
    if (steps > s->st.probe_max) s->st.probe_max = steps;
}

// The highest value of (cat, hash) the local game takes (CoopSyncLimitFn); 0: none, or no limits yet.
static uint32_t limit_of(const CoopSync *s, int cat, uint32_t hash) { return s->limit ? s->limit(cat, hash, s->limit_user) : 0; }

static Pool *pool_get(CoopSync *s, uint32_t hash, int cat, int create) {
    for (int i = 0; i < s->npools; i++)
        if (s->pools[i].hash == hash) return &s->pools[i];
    if (!create || s->npools == MAX_POOLS) return NULL;
    Pool *p = &s->pools[s->npools++];
    memset(p, 0, sizeof *p);
    p->hash = hash, p->cat = (uint8_t)cat, p->cap = -1, p->base = -1;
    uint32_t max = limit_of(s, cat, hash);
    p->max = max && max < INT32_MAX ? (int32_t)max : INT32_MAX;
    return p;
}

static int reg_get(CoopSync *s, uint8_t kind, int cat, uint32_t hash, uint64_t who) {
    int idx = find_reg(s, kind, hash, who);
    if (idx >= 0) return idx;
    if (s->nregs >= MAX_REGS) return -1;
    Pool *pool = kind == COOP_R_FLAG ? NULL : pool_get(s, hash, cat, 1);
    if (kind != COOP_R_FLAG && !pool) return -1;
    if ((kind == COOP_R_POOL_GAIN || kind == COOP_R_POOL_SPEND) && pool->contrib >= MAX_CONTRIB) return -1;
    if (s->nregs == s->cap) {
        int nc = s->cap ? s->cap * 2 : 1024;
        Reg *nr = realloc(s->regs, (size_t)nc * sizeof *nr);
        if (!nr) return -1;
        s->regs = nr, s->cap = nc;
    }
    if ((s->nregs + 1) * 10 > s->index_cap * 7) {   // keep the index under 70 % full
        int nc = s->index_cap ? s->index_cap * 2 : 2048;
        int *ni = calloc((size_t)nc, sizeof *ni);
        if (!ni) return -1;
        free(s->index);
        s->index = ni, s->index_cap = nc;
        for (int i = 0; i < s->nregs; i++) index_insert(s, i);
    }
    idx = s->nregs++;
    Reg *r = &s->regs[idx];
    memset(r, 0, sizeof *r);
    r->kind = kind, r->cat = (uint8_t)cat, r->hash = hash, r->who = who;
    index_insert(s, idx);
    if (kind == COOP_R_POOL_BASE) pool->base = idx;
    else if (kind != COOP_R_FLAG) pool->contrib++;
    return idx;
}

static void queue_apply(CoopSync *s, int code) {
    if (s->apply_tail - s->apply_head >= s->apply_cap) {
        unsigned nc = s->apply_cap ? s->apply_cap * 2 : 1024;
        int *na = malloc(nc * sizeof *na);
        if (!na) return;
        for (unsigned i = 0; i < s->apply_tail - s->apply_head; i++) na[i] = s->apply[(s->apply_head + i) % s->apply_cap];
        s->apply_tail -= s->apply_head, s->apply_head = 0;
        free(s->apply);
        s->apply = na, s->apply_cap = nc;
    }
    s->apply[s->apply_tail++ % s->apply_cap] = code;
}

static int64_t pool_value(const CoopSync *s, const Pool *p) {
    int64_t base = p->base >= 0 && s->regs[p->base].s ? base_decode(s->regs[p->base].s) : 0;
    return base + p->gains - p->spends;
}

static int32_t pool_target(const CoopSync *s, const Pool *p) {
    int64_t v = pool_value(s, p);
    if (v < 0) v = 0;
    if (p->cap >= 0 && v > p->cap) v = p->cap;
    if (v > p->max) v = p->max;
    if (v > INT32_MAX) v = INT32_MAX;
    return (int32_t)v;
}

static void pool_check(CoopSync *s, Pool *p) {
    if (!p->bound || p->queued || pool_target(s, p) == p->expected) return;
    p->queued = 1;
    queue_apply(s, -1 - (int)(p - s->pools));
}

static void flag_check(CoopSync *s, int idx) {
    Reg *r = &s->regs[idx];
    if (r->queued || !reg_synced(s, r) || r->s <= r->l || r->s == r->applied) return;
    r->queued = 1;
    queue_apply(s, idx);
}

// Raise register idx to v (v above its value): sums, pool totals, applies.
static void reg_raise(CoopSync *s, int idx, uint32_t v) {
    Reg *r = &s->regs[idx];
    uint32_t old = r->s;
    if (reg_synced(s, r)) {
        int sec = sec_of(r);
        s->sums[sec] += share_of(r, v) - share_of(r, old);
    }
    r->s = v;
    if (r->kind == COOP_R_FLAG) {
        flag_check(s, idx);
        return;
    }
    Pool *p = pool_get(s, r->hash, r->cat, 0);
    if (!p) return;
    if (r->kind == COOP_R_POOL_GAIN) p->gains += v - old;
    else if (r->kind == COOP_R_POOL_SPEND) p->spends += v - old;
    pool_check(s, p);
}

static void recompute_sums(CoopSync *s) {
    memset(s->sums, 0, sizeof s->sums);
    for (int i = 0; i < s->nregs; i++) {
        const Reg *r = &s->regs[i];
        if (r->s && reg_synced(s, r)) s->sums[sec_of(r)] += share_of(r, r->s);
    }
}

static void push_local(CoopSync *s, int idx, uint64_t now) {
    if (!s->in_room || !reg_synced(s, &s->regs[idx])) return;   // a digest carries it once the room is known
    if (s->push_tail - s->push_head >= PUSH_SLOTS) return;     // the digests repair what this drops
    s->push[s->push_tail++ % PUSH_SLOTS] = (Push){idx, 0, now, now};
}

// Forget every register, pool, queued push and apply (the allocations stay for reuse).
static void drop_state(CoopSync *s) {
    s->nregs = 0;
    if (s->index) memset(s->index, 0, (size_t)s->index_cap * sizeof *s->index);
    s->npools = 0;
    s->push_head = s->push_tail = 0;
    s->apply_head = s->apply_tail = 0;
    memset(s->sums, 0, sizeof s->sums);
}

void coop_sync_limits(CoopSync *s, CoopSyncLimitFn fn, void *user) { s->limit = fn, s->limit_user = user; }

void coop_sync_index_key(CoopSync *s, const uint8_t key[16]) {
    s->ik0 = s->ik1 = 0;
    for (int i = 0; i < 8; i++) s->ik0 |= (uint64_t)key[i] << 8 * i, s->ik1 |= (uint64_t)key[8 + i] << 8 * i;
    if (s->index) memset(s->index, 0, (size_t)s->index_cap * sizeof *s->index);
    for (int i = 0; i < s->nregs; i++) index_insert(s, i);
}

void coop_sync_schema(CoopSync *s, uint32_t schema) {
    if (schema == s->schema) return;
    s->schema = schema;
    drop_state(s);   // what was merged under another classification means nothing under this one
    s->caught_up = 0;
    s->digest_to |= 1;
}

unsigned coop_sync_ignored(const CoopSync *s) {
    unsigned mask = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) mask |= (unsigned)(s->peer[id].present && s->peer[id].ignored) << id;
    return mask;
}

// ---- lifetime

CoopSync *coop_sync_new(uint64_t self_id, uint32_t schema) {
    CoopSync *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->self_id = self_id, s->schema = schema;
    s->rng = mix64(self_id ^ 0x5EED);
    return s;
}

void coop_sync_free(CoopSync *s) {
    if (!s) return;
    free(s->regs);
    free(s->index);
    free(s->apply);
    free(s);
}

void coop_sync_trace(CoopSync *s, void (*fn)(void *user, const char *line), void *user) { s->trace = fn, s->trace_user = user; }

// ---- rules in force

static void update_caught_up(CoopSync *s) {
    if (s->caught_up || !s->in_room || !s->have_room) return;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Peer *p = &s->peer[id];
        if (p->present && (!p->have_digest || memcmp(p->sums, s->sums, sizeof s->sums))) return;
    }
    s->caught_up = 1;
    tracef(s, "caught up with the room");
}

static void adopt_rules(CoopSync *s, const CoopRules *r) {
    CoopRules n = *r;
    coop_rules_normalize(&n);
    int had = s->have_room;
    CoopRules old = s->room;
    s->room = n;
    s->have_room = 1;
    s->rules_ver++;
    recompute_sums(s);
    for (int i = 0; i < s->npools; i++) {   // a pool switched to per player stops; its counter stays as it is
        Pool *p = &s->pools[i];
        if (s->room.mode[p->cat] != COOP_MODE_POOL) p->bound = 0;
    }
    for (int c = 0; c < COOP_CAT_N; c++)
        if (!had || old.mode[c] != n.mode[c]) {
            s->caught_up = 0;   // pools of a category that just joined bind once the room is known again
            break;
        }
    for (int i = 0; i < s->nregs; i++)
        if (s->regs[i].kind == COOP_R_FLAG) flag_check(s, i);
    s->digest_to |= 1;
    update_caught_up(s);
}

// The maker's own settings become the room's rules, one epoch later. The epoch saturates: only the maker raises it,
// one for each change, so it never gets there, and no digest of anyone else can push it there and wrap it to 0.
static void originate(CoopSync *s) {
    CoopRules r = s->mine;
    r.epoch = s->room.epoch == UINT32_MAX ? UINT32_MAX : s->room.epoch + 1;
    r.maker = (uint8_t)s->self;
    adopt_rules(s, &r);
}

unsigned coop_sync_rules(const CoopSync *s, CoopRules *out) {
    if (out) {
        if (s->have_room) *out = s->room;
        else memset(out, 0, sizeof *out);
    }
    return s->rules_ver;
}

int coop_sync_caught_up(const CoopSync *s) { return s->caught_up; }

void coop_sync_settings(CoopSync *s, const CoopRules *mine, int made_room) {
    s->mine = *mine;
    coop_rules_normalize(&s->mine);
    s->have_mine = 1;
    s->made = made_room;
}

// ---- the room

void coop_sync_room(CoopSync *s, const char *code, int self, const int *ids, int n, int maker, uint64_t now) {
    if (!code) code = "";
    if (strcmp(code, s->code)) {
        snprintf(s->code, sizeof s->code, "%s", code);
        s->in_room = code[0] != 0;
        s->have_room = 0, s->caught_up = 0;
        memset(&s->room, 0, sizeof s->room);
        memset(s->peer, 0, sizeof s->peer);
        s->joined_at = now, s->originated_at = 0;
        s->rules_ver++;
        // everything merged belongs to the last room: nothing of it (flags, the save it came from) reaches the next one
        drop_state(s);
        s->next_digest = now;
    }
    s->self = self;
    s->maker = maker >= 1 && maker <= COOP_MAX_MEMBERS ? maker : 0;
    int seen[COOP_MAX_MEMBERS + 1] = {0};
    for (int i = 0; i < n; i++)
        if (ids[i] >= 1 && ids[i] <= COOP_MAX_MEMBERS && ids[i] != self) seen[ids[i]] = 1;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        Peer *p = &s->peer[id];
        if (seen[id] && !p->present) {
            memset(p, 0, sizeof *p);
            p->present = 1;
            s->digest_to |= 1ull << id;   // a member who just joined hears this player's state at once
        } else if (!seen[id] && p->present) {
            memset(p, 0, sizeof *p);
        }
    }
    update_caught_up(s);
}

// ---- messages

static size_t entry_size(uint8_t kind) { return kind == COOP_R_POOL_GAIN || kind == COOP_R_POOL_SPEND ? 18 : 10; }

typedef struct {
    uint8_t buf[MSG_CAP];
    uint8_t *at;
    int n;
} Msg;

static void msg_begin(CoopSync *s, Msg *m) {
    m->at = m->buf;
    put8(&m->at, COOP_K_DIGEST_VER);
    put32(&m->at, s->schema);
    put8(&m->at, 0);
    m->n = 0;
}

static int msg_add(Msg *m, const Reg *r) {
    if ((size_t)(m->at - m->buf) + entry_size(r->kind) > sizeof m->buf || m->n == 255) return 0;
    put8(&m->at, r->kind);
    put8(&m->at, r->cat);
    put32(&m->at, r->hash);
    if (entry_size(r->kind) == 18) put64(&m->at, r->who);
    put32(&m->at, r->s);
    m->n++;
    return 1;
}

static int msg_send(CoopSync *s, Msg *m, int to, CoopSyncSendFn send, void *user) {
    if (!m->n) return 1;
    m->buf[5] = (uint8_t)m->n;
    size_t len = (size_t)(m->at - m->buf);
    int ok = send(to, COOP_K_SECTION, m->buf, len, user);
    if (ok) s->st.sent_msgs++, s->st.sent_bytes += (unsigned)len;
    s->tokens -= (int64_t)len;
    return ok;
}

static void trace_entry(CoopSync *s, const char *verb, const Reg *r, uint32_t v, int member) {
    if (!s->trace) return;
    char to[24];
    if (member) snprintf(to, sizeof to, "member %d", member);
    else snprintf(to, sizeof to, "all");
    if (r->kind == COOP_R_FLAG) tracef(s, "%s %s %08x = %u %s %s", verb, cat_names[r->cat], r->hash, v, *verb == 's' ? "to" : "from", to);
    else
        tracef(s, "%s %s %08x %s %lld %s %s", verb, cat_names[r->cat], r->hash,
               r->kind == COOP_R_POOL_BASE ? "base" : r->kind == COOP_R_POOL_GAIN ? "gain" : "spend",
               r->kind == COOP_R_POOL_BASE ? (long long)base_decode(v) : (long long)v, *verb == 's' ? "to" : "from", to);
}

static void send_digest(CoopSync *s, int to, CoopSyncSendFn send, void *user) {
    uint8_t buf[16 + COOP_CAT_N + 1 + 4 * COOP_SYNC_SECTIONS + 8], *p = buf;
    put8(&p, COOP_K_DIGEST_VER);
    put32(&p, s->schema);
    put32(&p, s->have_room ? s->room.epoch : 0);
    put8(&p, COOP_CAT_N);
    for (int c = 0; c < COOP_CAT_N; c++) put8(&p, s->have_room ? s->room.mode[c] : 0);
    put8(&p, COOP_SYNC_SECTIONS);
    for (int i = 0; i < COOP_SYNC_SECTIONS; i++) put32(&p, s->sums[i]);
    if (send(to, COOP_K_DIGEST, buf, (size_t)(p - buf), user)) s->st.sent_msgs++, s->st.sent_bytes += (unsigned)(p - buf);
}

static void on_digest(CoopSync *s, int from, const uint8_t *b, size_t n, uint64_t now) {
    if (n < 1 + 4 + 4 + 1 || b[0] != COOP_K_DIGEST_VER || get32(b + 1) != s->schema) return;
    size_t ncat = b[9];
    if (n < 10 + ncat + 1) return;
    size_t nsec = b[10 + ncat];
    if (nsec != COOP_SYNC_SECTIONS || n < 11 + ncat + 4 * nsec) return;
    CoopRules r = {.epoch = get32(b + 5), .maker = (uint8_t)from};
    for (size_t c = 0; c < ncat && c < COOP_CAT_N; c++) r.mode[c] = b[10 + c];
    // only the maker the relay names changes the rules, and only to a later epoch; a game that holds none while no
    // maker is in the room takes any member's (the player sees them before accepting the room)
    if (r.epoch) {
        int by_maker = s->maker && from == s->maker;
        int newer = !s->have_room || s->room.maker != from || r.epoch > s->room.epoch ||
                    (r.epoch == UINT32_MAX && s->room.epoch == UINT32_MAX && memcmp(r.mode, s->room.mode, COOP_CAT_N));
        if (by_maker ? newer : !s->have_room && !s->maker) adopt_rules(s, &r);
    }
    Peer *p = &s->peer[from];
    if (!p->present) return;
    p->have_digest = 1;
    int differs = 0;
    for (int i = 0; i < COOP_SYNC_SECTIONS; i++) {
        p->sums[i] = get32(b + 11 + ncat + 4 * (size_t)i);
        if (p->sums[i] != s->sums[i]) {
            differs = 1;
            if (now - p->answered_at[i] >= ANSWER_MS || !p->answered_at[i]) p->want |= 1ull << i;
        }
    }
    // the peer may hold what this player lacks: a digest back lets it answer at once instead of at its next one
    if (differs && now - p->digest_at >= ANSWER_MS) s->digest_to |= 1ull << from, p->digest_at = now;
    update_caught_up(s);
}

// A value this game refuses: counted, and against its sender when no honest member sends it (an honest one with the
// same schema refuses it too and never passes it on).
static void refuse(CoopSync *s, int from, int hostile, const char *why, uint32_t hash) {
    Peer *p = &s->peer[from];
    s->st.rejected++;
    if (!hostile) return;
    p->bad++;
    tracef(s, "refused from member %d: %s %08x (%u so far)", from, why, hash, p->bad);
    if (p->bad >= BAD_MAX && !p->ignored) {
        p->ignored = 1;
        tracef(s, "member %d is ignored while it stays in the room", from);
    }
}

static void on_section(CoopSync *s, int from, const uint8_t *b, size_t n) {
    if (n < SYNC_HDR || b[0] != COOP_K_DIGEST_VER || get32(b + 1) != s->schema) return;
    size_t count = b[5], at = SYNC_HDR;
    for (size_t i = 0; i < count && !s->peer[from].ignored; i++) {
        if (at + 2 > n) return;
        uint8_t kind = b[at], cat = b[at + 1];
        if (kind < COOP_R_FLAG || kind > COOP_R_POOL_SPEND || cat >= COOP_CAT_N) return;
        size_t sz = entry_size(kind);
        if (at + sz > n) return;
        uint32_t hash = get32(b + at + 2);
        uint64_t who = sz == 18 ? get64(b + at + 6) : 0;
        uint32_t v = get32(b + at + sz - 4);
        at += sz;
        if (!s->have_room || s->room.mode[cat] != (kind == COOP_R_FLAG ? COOP_MODE_SHARED : COOP_MODE_POOL)) continue;
        if (!s->limit) continue;   // the game's table is not known yet
        // checked before anything is stored or passed on: a hash this game shares in that category, a value it takes
        uint32_t max = limit_of(s, cat, hash);
        int isflag = kind == COOP_R_FLAG, contrib = kind == COOP_R_POOL_GAIN || kind == COOP_R_POOL_SPEND;
        // a refused pool register does not count against its sender: an honest member passes on registers whose sum
        // this game cannot check (a base above the counter's limit is honest after spends), so only flags tell
        if (isflag == coop_cat_is_pool(cat) || contrib == !who || !max) {
            refuse(s, from, isflag, "not a value this game shares", hash);
            continue;
        }
        if (isflag ? v > max : kind == COOP_R_POOL_BASE ? v && (base_decode(v) < -POOL_REG_MAX || base_decode(v) > POOL_REG_MAX) : v > POOL_REG_MAX) {
            refuse(s, from, isflag, "a value out of range for", hash);
            continue;
        }
        int idx = find_reg(s, kind, hash, who);
        if (idx >= 0 && s->regs[idx].cat != cat) continue;   // classified differently here: this game's view wins
        if (idx >= 0 && v <= s->regs[idx].s) continue;
        Pool *p = contrib ? pool_get(s, hash, cat, 0) : NULL;
        if (p && p->bound && who == p->me) {   // only this game counts its own gains and spends
            refuse(s, from, 0, "a raise of this game's own contributor", hash);
            continue;
        }
        if (idx < 0 && !v) continue;
        if (idx < 0 && (idx = reg_get(s, kind, cat, hash, who)) < 0) {
            s->st.rejected++;   // the state is full (or the pool has its most contributors)
            continue;
        }
        trace_entry(s, "received", &s->regs[idx], v, from);
        reg_raise(s, idx, v);
        s->st.merged++;
    }
    update_caught_up(s);
}

void coop_sync_receive(CoopSync *s, int from, uint8_t kind, const uint8_t *bytes, size_t n, uint64_t now) {
    if (from < 1 || from > COOP_MAX_MEMBERS || from == s->self || !s->in_room) return;
    Peer *p = &s->peer[from];
    if (!p->present || p->ignored) return;
    if (!p->tok_at) p->tok_at = now, p->tokens = PEER_BURST;   // a member's messages are taken at PEER_RATE
    if (now > p->tok_at) p->tokens += (int64_t)(now - p->tok_at) * PEER_RATE / 1000, p->tok_at = now;
    if (p->tokens > PEER_BURST) p->tokens = PEER_BURST;
    if (p->tokens < (int64_t)n) {
        s->st.rate_dropped++;
        return;
    }
    p->tokens -= (int64_t)n;
    s->st.recv_msgs++;
    if (kind == COOP_K_DIGEST) on_digest(s, from, bytes, n, now);
    else if (kind == COOP_K_SECTION) on_section(s, from, bytes, n);
}

// One section's synced registers to member to; 0 when the sender refused a message (the outbox is full).
static int send_section(CoopSync *s, int sec, int to, CoopSyncSendFn send, void *user) {
    Msg m;
    msg_begin(s, &m);
    for (int i = 0; i < s->nregs; i++) {
        const Reg *r = &s->regs[i];
        if (!r->s || sec_of(r) != sec || !reg_synced(s, r)) continue;
        if (!msg_add(&m, r)) {
            if (!msg_send(s, &m, to, send, user)) return 0;
            msg_begin(s, &m);
            msg_add(&m, r);
        }
        trace_entry(s, "sent", r, r->s, to);
    }
    return msg_send(s, &m, to, send, user);
}

void coop_sync_tick(CoopSync *s, uint64_t now, CoopSyncSendFn send, void *user) {
    if (!s->tok_at) s->tok_at = now, s->tokens = BURST_BYTES;
    s->tokens += (int64_t)(now - s->tok_at) * RATE_BYTES / 1000;
    if (s->tokens > BURST_BYTES) s->tokens = BURST_BYTES;
    s->tok_at = now;
    if (!s->in_room) return;

    // rules: the maker's settings are the room's (the relay names the maker); nobody else makes any
    if (s->have_mine && s->made) {
        int stale = !s->have_room || s->room.maker != s->self || memcmp(s->room.mode, s->mine.mode, COOP_CAT_N);
        if (stale && (!s->originated_at || now - s->originated_at >= 1000)) {
            s->originated_at = now ? now : 1;
            originate(s);
        }
    }

    // digests: every DIGEST_MS to all, at once to whoever needs one
    if (now >= s->next_digest) {
        s->digest_to |= 1;
        s->rng = mix64(s->rng);
        s->next_digest = now + DIGEST_MS + s->rng % 400;
    }
    if (s->digest_to & 1) send_digest(s, 0, send, user);
    else
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
            if (s->digest_to >> id & 1 && s->peer[id].present) send_digest(s, id, send, user);
    s->digest_to = 0;
    if (!s->have_room) return;

    // local changes: three sends, 0, 80 and 250 ms after the change
    Msg m;
    msg_begin(s, &m);
    unsigned keep = s->push_tail;
    while (s->push_head != keep && s->tokens > 0) {
        Push *q = &s->push[s->push_head % PUSH_SLOTS];
        if (q->due > now) {   // not yet: to the back of the line (the line stays in order of due time per round)
            s->push[s->push_tail++ % PUSH_SLOTS] = *q;
            s->push_head++;
            continue;
        }
        const Reg *r = &s->regs[q->idx];
        if (!msg_add(&m, r)) {
            msg_send(s, &m, 0, send, user);
            msg_begin(s, &m);
            msg_add(&m, r);
        }
        if (!q->round) trace_entry(s, "sent", r, r->s, 0);
        if (q->round < 2) s->push[s->push_tail++ % PUSH_SLOTS] = (Push){q->idx, (uint8_t)(q->round + 1), q->first + (q->round ? 250 : 80), q->first};
        s->push_head++;
    }
    msg_send(s, &m, 0, send, user);

    // sections a peer's digest showed to differ, paced
    for (int id = 1; id <= COOP_MAX_MEMBERS && s->tokens > 0; id++) {
        Peer *p = &s->peer[id];
        if (!p->present) {
            p->want = 0;
            continue;
        }
        while (p->want && s->tokens > 0) {
            int sec = __builtin_ctzll(p->want);
            if (!send_section(s, sec, id, send, user)) return;   // the outbox is full: next tick
            p->want &= p->want - 1;
            p->answered_at[sec] = now ? now : 1;
        }
    }
}

// ---- the local game

void coop_sync_rebind(CoopSync *s) {
    // the merged state holds the last save's progress: forgotten, so that none of it reaches this save; the game
    // reports its flags again and the room sends what it holds within a few digests
    drop_state(s);
    s->caught_up = 0;
    s->digest_to |= 1;
}

void coop_sync_local(CoopSync *s, int cat, uint32_t hash, uint32_t value, uint64_t now) {
    if (cat < 0 || cat >= COOP_CAT_N || coop_cat_is_pool(cat)) return;
    int idx = find_reg(s, COOP_R_FLAG, hash, 0);
    if (idx < 0) {
        if (!value || (idx = reg_get(s, COOP_R_FLAG, cat, hash, 0)) < 0) return;
    }
    Reg *r = &s->regs[idx];
    r->l = value;
    if (value > r->s) {
        reg_raise(s, idx, value);
        push_local(s, idx, now);
    }
}

static void contribute(CoopSync *s, Pool *p, uint8_t kind, uint32_t add, uint64_t now) {
    int idx = reg_get(s, kind, p->cat, p->hash, p->me);
    if (idx < 0) return;
    uint64_t v = (uint64_t)s->regs[idx].s + add;   // saturates: never wraps to a small value
    if (v > POOL_REG_MAX) v = POOL_REG_MAX;
    if (v <= s->regs[idx].s) return;
    reg_raise(s, idx, (uint32_t)v);
    push_local(s, idx, now);
}

void coop_sync_pool_local(CoopSync *s, int cat, uint32_t hash, int32_t value, int32_t cap, uint64_t now) {
    if (cat < 0 || cat >= COOP_CAT_N || !coop_cat_is_pool(cat)) return;
    Pool *p = pool_get(s, hash, cat, 1);
    if (!p) return;
    p->cap = cap, p->local = value, p->have_local = 1;
    if (!s->have_room || s->room.mode[cat] != COOP_MODE_POOL) {
        p->bound = 0;
        return;
    }
    if (!p->bound) {
        if (!s->caught_up) return;
        // join the pool: its base rises so that the pool holds at least what this player has
        p->me = mix64(s->self_id ^ mix64(++s->bindings) ^ now << 20);
        int b = reg_get(s, COOP_R_POOL_BASE, cat, hash, 0);
        if (b < 0) return;
        int64_t claim = (int64_t)value - (p->gains - p->spends);
        uint32_t want = base_encode(claim < -POOL_REG_MAX ? -POOL_REG_MAX : claim > POOL_REG_MAX ? POOL_REG_MAX : claim);
        if (want > s->regs[b].s) {
            reg_raise(s, b, want);
            push_local(s, b, now);
        }
        p->expected = value, p->bound = 1;
        s->st.pools_bound++;
        tracef(s, "pool %s %08x joined at %d, the pool holds %lld", cat_names[cat], hash, value, (long long)pool_value(s, p));
        pool_check(s, p);
        return;
    }
    int64_t d = (int64_t)value - p->expected;
    if (d > 0) contribute(s, p, COOP_R_POOL_GAIN, (uint32_t)d, now);
    else if (d < 0) contribute(s, p, COOP_R_POOL_SPEND, (uint32_t)-d, now);
    p->expected = value;
    pool_check(s, p);
}

int coop_sync_take(CoopSync *s, CoopApply *out, int max) {
    int n = 0;
    while (n < max && s->apply_head != s->apply_tail) {
        int code = s->apply[s->apply_head++ % s->apply_cap];
        if (code >= 0) {
            Reg *r = &s->regs[code];
            r->queued = 0;
            if (!reg_synced(s, r) || r->s <= r->l || r->s == r->applied) continue;
            r->applied = r->s;
            out[n++] = (CoopApply){0, r->cat, r->hash, (int32_t)(r->s > INT32_MAX ? INT32_MAX : r->s)};
        } else {
            Pool *p = &s->pools[-1 - code];
            p->queued = 0;
            if (!p->bound) continue;
            int32_t t = pool_target(s, p);
            if (t == p->expected) continue;
            out[n++] = (CoopApply){1, p->cat, p->hash, t};
        }
    }
    return n;
}

void coop_sync_pool_wrote(CoopSync *s, uint32_t hash, int32_t value) {
    Pool *p = pool_get(s, hash, 0, 0);
    if (!p) return;
    p->expected = value;
    pool_check(s, p);
}

void coop_sync_untake(CoopSync *s, const CoopApply *a) {
    if (a->pool) {
        Pool *p = pool_get(s, a->hash, 0, 0);
        if (p && !p->queued) p->queued = 1, queue_apply(s, -1 - (int)(p - s->pools));
        return;
    }
    int idx = find_reg(s, COOP_R_FLAG, a->hash, 0);
    if (idx < 0) return;
    s->regs[idx].applied = 0;
    flag_check(s, idx);
}

// ---- inspection

void coop_sync_stats(const CoopSync *s, CoopSyncStats *out) {
    *out = s->st;
    out->regs = (unsigned)s->nregs;
    out->synced = 0;
    for (int i = 0; i < s->nregs; i++)
        if (s->regs[i].s && reg_synced(s, &s->regs[i])) out->synced++;
    out->pools_bound = 0;
    for (int i = 0; i < s->npools; i++) out->pools_bound += s->pools[i].bound;
}

uint32_t coop_sync_flag(const CoopSync *s, int cat, uint32_t hash) {
    (void)cat;
    int idx = find_reg(s, COOP_R_FLAG, hash, 0);
    return idx < 0 ? 0 : s->regs[idx].s;
}

int coop_sync_pool(const CoopSync *s, uint32_t hash, int64_t *value) {
    for (int i = 0; i < s->npools; i++)
        if (s->pools[i].hash == hash) {
            if (value) *value = pool_value(s, &s->pools[i]);
            return s->pools[i].bound;
        }
    if (value) *value = 0;
    return 0;
}

void coop_sync_sums(const CoopSync *s, uint32_t out[COOP_SYNC_SECTIONS]) { memcpy(out, s->sums, sizeof s->sums); }
