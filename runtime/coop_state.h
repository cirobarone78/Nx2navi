// Co-op progress sync: the merge engine, without the game. Each player's game reports its flags here (coop_game.c reads
// them from the game's flag database); the engine keeps the merged state of the room, gossips it between the players
// (coop.c carries the messages) and hands back what the local game lacks. It knows flags only as (category, 32-bit hash,
// value), so the probe drives it with a fake flag store.
//
// The state is a set of registers, each merged by keeping the highest value: a flag's register holds 1 for a flag that
// changed from its default (it stays changed), or the count of an item (the highest wins). Merging is commutative,
// associative and idempotent, so lost, repeated and reordered messages do no harm, and nothing a player has shrinks.
// Spendable counters (rupees, ammunition, small keys) in a shared pool use four kinds of register: a base, and per
// contributor the units gained and spent; the pool's value is base + all gains - all spends, which every player sees.
//
// Gossip. Registers are split into COOP_SYNC_SECTIONS sections by hash; a DIGEST carries a sum per section (and the
// room's rules), every 2 s and at once to a member who joins; a section whose sum differs from a peer's is sent to it in
// SYNC messages, so two players converge within a few digests and a late joiner catches up with everything. A local
// change goes out at once and again 80 and 250 ms later. Sending is paced (32 KB/s) under the relay's per-member limit.
//
// Rules belong to the room: every game merges the same way. The player who made the room originates them from their own
// settings (coop_sync_settings); they ride in every DIGEST, ordered by (epoch, owner, modes), and the highest wins, so a
// joiner and a game that rejoined after a relay restart adopt them. The maker raises the epoch when it changes a setting.
// If no member has heard rules 5 s after joining (the maker left a room that lives on), the member with the lowest id
// originates them. A category switched to shared starts merging (its digest sums differ, the sections are exchanged); one
// switched to per player stops; what was merged stays.
//
// Threads: none of its own and no lock. coop_game.c holds one lock around every call.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Categories of shared state (coop_game.c classifies the game's flags) and their modes.
enum {
    COOP_CAT_PROGRESS,   // story, world and dungeon flags: shared | own
    COOP_CAT_ITEMS,      // equipment, key items, upgrades and capacities: shared | own
    COOP_CAT_SONGS,      // shared | own
    COOP_CAT_QUEST,      // medallions, stones and the other quest items: shared | own
    COOP_CAT_KEYS,       // small keys, per dungeon: pool | own
    COOP_CAT_RUPEES,     // own | pool
    COOP_CAT_AMMO,       // own | pool
    COOP_CAT_N
};
enum { COOP_MODE_OWN = 0, COOP_MODE_SHARED = 1, COOP_MODE_POOL = 2 };

typedef struct {
    uint32_t epoch;      // 0: no rules
    uint64_t owner;      // the client id that made them
    uint8_t mode[COOP_CAT_N];
} CoopRules;

const char *coop_cat_name(int cat);    // "progress", ... (the settings key without "coop_")
const char *coop_mode_name(int mode);  // "own", "shared", "pool"
int coop_cat_is_pool(int cat);         // keys, rupees, ammo: own | pool; the others own | shared
// The defaults: everything shared, keys in a pool, rupees and ammunition per player.
void coop_rules_default(CoopRules *r);
// Makes r a set the engine can run: a mode a category does not have becomes its default; with progress per player, keys
// are per player too (a pool would pay twice for a key each world still holds); with progress shared, items, songs and
// quest items are shared (per player, the one who took one from the shared world would leave the others without it).
// The categories it changed, as a bit mask.
unsigned coop_rules_normalize(CoopRules *r);
// "progress shared, items shared, ..." into out.
void coop_rules_text(const CoopRules *r, char *out, size_t cap);

// Register kinds on the wire.
enum { COOP_R_FLAG = 1, COOP_R_POOL_BASE = 2, COOP_R_POOL_GAIN = 3, COOP_R_POOL_SPEND = 4 };
enum { COOP_SYNC_SECTIONS = 64, COOP_K_DIGEST_VER = 1 };

typedef struct CoopSync CoopSync;
// self_id: this player's client id (the owner of rules it makes). schema: games merge only with the same (game version
// and classification); 0 is a valid schema.
CoopSync *coop_sync_new(uint64_t self_id, uint32_t schema);
void coop_sync_free(CoopSync *s);
// Trace lines (sent, received, by hash and value) go to fn when set.
void coop_sync_trace(CoopSync *s, void (*fn)(void *user, const char *line), void *user);

// ---- the room (the coop thread)
// The room and who is in it: code "" when in none. A new code (or none) forgets the rules, the peers and everything
// merged: nothing of one room reaches the next.
// self: this player's member id; ids: every member's (self included).
void coop_sync_room(CoopSync *s, const char *code, int self, const int *ids, int n, uint64_t now_ms);
// This player's own rule settings, and whether it made the room (its settings are the room's).
void coop_sync_settings(CoopSync *s, const CoopRules *mine, int made_room);
// A peer message (kind COOP_K_DIGEST or COOP_K_SECTION, the bytes after the kind) from member from.
void coop_sync_receive(CoopSync *s, int from, uint8_t kind, const uint8_t *bytes, size_t n, uint64_t now_ms);
// Timers: digests, repeated pushes, answers to digests. send(member or 0 for all, kind, bytes, n, user) queues one
// message (1 when queued); call it every few ms.
typedef int (*CoopSyncSendFn)(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user);
void coop_sync_tick(CoopSync *s, uint64_t now_ms, CoopSyncSendFn send, void *user);
// The rules in force (epoch 0: none yet) and a counter that changes when they do.
unsigned coop_sync_rules(const CoopSync *s, CoopRules *out);
// 1 once every peer's digest matched this player's after it joined (or it is alone): the room's pools are known.
int coop_sync_caught_up(const CoopSync *s);

// ---- the local game (the game thread)
// Another save was loaded: everything merged is forgotten (it holds the last save's progress), the pools bind again.
// The game reports its flags again and the room sends what it holds, so the new save gets the room's state only.
void coop_sync_rebind(CoopSync *s);
// A flag's local value: 1 for a flag that differs from its default, a count for an item. Values only grow the merged state.
void coop_sync_local(CoopSync *s, int cat, uint32_t hash, uint32_t value, uint64_t now_ms);
// A pool counter's local value and its cap (-1: none). Before the room is known it is only remembered; then it joins the
// pool (the pool rises to it if lower: nothing is taken away) and each later change counts as a gain or a spend.
void coop_sync_pool_local(CoopSync *s, int cat, uint32_t hash, int32_t value, int32_t cap, uint64_t now_ms);

typedef struct {
    uint8_t pool;      // 0: a flag (value: the merged value), 1: a pool counter (value: what to show, within its cap)
    uint8_t cat;
    uint32_t hash;
    int32_t value;
} CoopApply;
// Up to max values the local game should take. A flag is handed out once per merged value, so the game may lower it
// afterwards (spend, lose) without getting it back; a pool counter until the game shows it (coop_sync_pool_wrote).
int coop_sync_take(CoopSync *s, CoopApply *out, int max);
// The game was given the value of a pool counter (coop_sync_take): it is the expected local value from now on.
void coop_sync_pool_wrote(CoopSync *s, uint32_t hash, int32_t value);
// Put a taken apply back (the game could not take it this frame).
void coop_sync_untake(CoopSync *s, const CoopApply *a);

// ---- inspection (tests, logs)
typedef struct {
    unsigned regs, synced;          // registers, and those under the room's rules
    unsigned sent_msgs, recv_msgs, sent_bytes;
    unsigned merged;                // registers a message raised
    unsigned pools_bound;
} CoopSyncStats;
void coop_sync_stats(const CoopSync *s, CoopSyncStats *out);
// The merged value of a flag (0 when absent), and a pool's value (base + gains - spends) and whether it is bound.
uint32_t coop_sync_flag(const CoopSync *s, int cat, uint32_t hash);
int coop_sync_pool(const CoopSync *s, uint32_t hash, int64_t *value);
// The digest sums, for tests: two engines with equal sums hold the same synced state.
void coop_sync_sums(const CoopSync *s, uint32_t out[COOP_SYNC_SECTIONS]);

int probe_coop_sync(void);   // coop_sync_probe.c: the engine's part of --probe-coop; failures
