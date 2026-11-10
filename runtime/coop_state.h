// Co-op progress sync: the merge engine, without the game. Each player's game reports its flags here (coop_game.c reads
// them from the game's flag database); the engine keeps the merged state of the room, gossips it between the players
// (coop.c carries the messages) and hands back what the local game lacks. It knows flags only as (category, 32-bit hash,
// value), so the probe drives it with a fake flag store.
//
// The state is a set of registers, each merged by keeping the highest value: a flag's register holds 1 for a flag that
// changed from its default (it stays changed), or the count of an item (the highest wins). Merging is commutative,
// associative and idempotent, so lost, repeated and reordered messages do no harm, and nothing a player has shrinks.
// Spendable counters (rupees, ammunition, small keys) in a shared pool are records, one per contributor (a member's
// identity) and pool: the base it brought when it joined the pool, and the units it gained and spent, signed with the
// member's key, so every member passes them on and nobody but their owner can change them. The pool's value is the
// highest base + all gains - all spends, where no contributor's spends count beyond its own gains plus the counter's
// limit: a contributor can take at most what the pool can hold. Only identities the maker admitted contribute.
//
// Gossip. Registers are split into COOP_SYNC_SECTIONS sections by hash; a DIGEST carries a sum per section (and the
// room's rules), every 2 s and at once to a member who joins; a section whose sum differs from a peer's is sent to it in
// SYNC messages, so two players converge within a few digests and a late joiner catches up with everything. A local
// change goes out at once and again 80 and 250 ms later. Sending is paced (32 KB/s) under the relay's per-member limit.
//
// Identities. Each game has a signing key for each room and start of the game (from coop_sync_seed's random value and the
// room's secret), and signs its identity in that room: the key, the name and the session the relay lists for it. A
// game takes values only from a member whose identity it checked, an identity is bound to one session, and a session
// that ended is never taken again, so an identity cannot be copied or reused by another member.
//
// Rules belong to the room: every game merges the same way. The maker's game signs a charter: the rules (the maker's
// settings, coop_sync_settings), the identities admitted to the pools, the identities removed and the pools' epoch,
// with an epoch it raises at each change; it rides in every DIGEST, so every member passes it on. A game takes a
// charter only signed by the key it pinned when its player accepted the room (coop_sync_accept); before that, by the
// key of the member the relay names as the maker, or, while no maker is there, by the one key every member's charter
// carries. A member the relay marks later (a restart, a hostile relay) changes nothing for a player who accepted. A
// category switched to shared starts merging (its digest sums differ, the sections are exchanged); one switched to per
// player stops; what was merged stays. A removed identity's messages are ignored and its records dropped everywhere; a
// pool reset (a new pools' epoch) drops every record, and the games join the pools again with what they hold.
//
// Acceptance. Nothing is handed to the game (coop_sync_take) until its player accepted the room as it is now (the
// identities in it, the rules, the maker's key and the save being played: coop_sync_offer) and a copy of the save was
// made (coop_sync_copied). The offer's number is keyed with this start's random value, so nobody can compute it in
// advance; it changes with any of those, and so does the acceptance.
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
    uint8_t maker;       // the member whose rules they are (this game's view: who sent them)
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

// Register kinds on the wire (2 to 4 were protocol 2's unsigned pool registers).
enum { COOP_R_FLAG = 1, COOP_R_POOL = 5 };
enum { COOP_SYNC_SECTIONS = 64, COOP_K_DIGEST_VER = 3 };   // 3: identities, a signed charter, signed pool records

typedef struct CoopSync CoopSync;
// self_id: random for this start of the game (contributor ids derive from it). schema: games merge only with the same
// (this layer's version, the game's, and the classified table); 0 until the table is known.
CoopSync *coop_sync_new(uint64_t self_id, uint32_t schema);
void coop_sync_free(CoopSync *s);
// Trace lines (sent, received, by hash and value) go to fn when set.
void coop_sync_trace(CoopSync *s, void (*fn)(void *user, const char *line), void *user);
// What the local game takes from the room: the highest value of flag or counter (cat, hash), 0 when this game does not
// share that hash in that category (a flag: 1 for a Bool; a pool counter: what the game can show). Every value a
// member sends is checked against it before it is stored or passed on; one that fails is dropped and counted, and a
// member that sends many is ignored while it stays in the room. Called with the engine's lock held; no limits, nothing
// is taken.
typedef uint32_t (*CoopSyncLimitFn)(int cat, uint32_t hash, void *user);
void coop_sync_limits(CoopSync *s, CoopSyncLimitFn fn, void *user);
// A random key of this process for the register index (SipHash-2-4), so a peer cannot choose where its registers go.
void coop_sync_index_key(CoopSync *s, const uint8_t key[16]);
uint64_t coop_siphash24(uint64_t k0, uint64_t k1, const uint8_t *m, size_t n);
// The schema, once the local game's table is known: a new one forgets everything merged.
void coop_sync_schema(CoopSync *s, uint32_t schema);
// Members (a bit per id) whose messages are ignored for sending values this game refuses.
unsigned coop_sync_ignored(const CoopSync *s);

// ---- the room (the coop thread)
// The room and who is in it: code "" when in none. A new code (or none) forgets the peers and everything merged:
// nothing of one room reaches the next. The acceptance, the pinned maker and the charter stay while the code comes
// back the same (the relay lost the room and the session reached it again); another room starts them anew.
// self: this player's member id; ids and sessions: every member's (self included; sessions NULL: each id's own
// number); maker: the member the relay names as the room's maker, 0 when none is in the room.
void coop_sync_room(CoopSync *s, const char *code, int self, const int *ids, const uint64_t *sessions, int n, int maker,
                    uint64_t now_ms);
// This player's own rule settings, and whether it made the room (its settings are the room's).
void coop_sync_settings(CoopSync *s, const CoopRules *mine, int made_room);
// A peer message (kind COOP_K_DIGEST or COOP_K_SECTION, the bytes after the kind) from member from.
void coop_sync_receive(CoopSync *s, int from, uint8_t kind, const uint8_t *bytes, size_t n, uint64_t now_ms);
// Timers: digests, repeated pushes, answers to digests. send(member or 0 for all, kind, bytes, n, user) queues one
// message (1 when queued); call it every few ms.
typedef int (*CoopSyncSendFn)(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user);
void coop_sync_tick(CoopSync *s, uint64_t now_ms, CoopSyncSendFn send, void *user);
// The rules in force (epoch 0: none yet; maker: the member holding the key that signed them, 0 none) and a counter
// that changes when they do.
unsigned coop_sync_rules(const CoopSync *s, CoopRules *out);

// ---- identity, the offer and acceptance
// A random value of this start of the game: the identity keys (one per room) and the offer's number derive from it.
void coop_sync_seed(CoopSync *s, const uint8_t seed[32]);
void coop_sync_name(CoopSync *s, const char *name);   // this player's name, signed into its identity
// What joining the room means, for the player to accept: the other members (by the names their identities carry), the
// rules and the key that signed them. heard: its identity was checked (a member not identified a few seconds after
// it appeared shows with heard 0 and nothing is taken from it); removed: the maker removed it. No offer (token 0)
// outside a room, before the rules are known, and while a member that just appeared is not identified yet.
typedef struct {
    int id, heard, maker, removed;
    char name[COOP_NAME_MAX + 1];   // the name it signed; "" until heard (coop_game.c shows the relay's)
    char key[20];                   // its key's fingerprint, xxxx-xxxx-xxxx-xxxx; "" until heard
} CoopOfferMember;
typedef struct {
    uint32_t token;     // the offer's number, random for each start of the game; 0: none
    int accepted;       // the player accepted this offer (values are applied once the copy is made: copy)
    int n;              // the other members
    CoopOfferMember member[COOP_MAX_MEMBERS];
    CoopRules rules;
    char maker_key[20]; // the fingerprint of the key that signed the rules
    int maker_moved;    // the relay names another member as the maker than the one whose key signs the rules
    int new_save;       // another save was loaded since the last acceptance: it gets the room's progress, which holds
                        // what every save played in the room brought (the last one's too)
    int copy;           // the copy of the save for this acceptance: 0 not yet, 1 made, -1 failed (accept again to retry)
} CoopOffer;
int coop_sync_offer(CoopSync *s, CoopOffer *out, uint64_t now_ms);   // 1 when there is an offer
// Accepts the offer of that number: its identities, rules and maker's key (pinned from now on in this room). 1 when
// token is the current offer's. A copy of the save is wanted when none was made for the save being played.
int coop_sync_accept(CoopSync *s, uint32_t token);
int coop_sync_copy_wanted(const CoopSync *s);
void coop_sync_copied(CoopSync *s, int ok);
// Values are handed out: the current offer is accepted and its copy made (coop_sync_take checks it itself).
int coop_sync_open(CoopSync *s);
// The maker's game (it holds the key the charter is signed with): removes a member (its identity is dropped by every
// game: its messages ignored, its pool records gone), or resets the pools (every record dropped; the games join them
// again with what they hold). 1 when done.
int coop_sync_remove(CoopSync *s, int member);
int coop_sync_reset_pools(CoopSync *s);
int coop_sync_is_maker(const CoopSync *s);
// 1 once every peer's digest matched this player's after it joined (or it is alone): the room's pools are known.
int coop_sync_caught_up(const CoopSync *s);

// ---- the local game (the game thread)
// A save is being played (the game calls it at every bind). After the first one: another save was loaded, so
// everything merged is forgotten, the pools bind again (this game's own pool records stay: they are the room's), and
// the acceptance is asked again with a new copy, since the room's progress holds what the last save brought.
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
    unsigned rejected;              // values refused (not shared here, out of range, the state full)
    unsigned rate_dropped;          // messages over a member's rate
    unsigned probe_max;             // the longest run of index slots an insert walked
} CoopSyncStats;
void coop_sync_stats(const CoopSync *s, CoopSyncStats *out);
// The merged value of a flag (0 when absent), and a pool's value and whether it is bound.
uint32_t coop_sync_flag(const CoopSync *s, int cat, uint32_t hash);
int coop_sync_pool(const CoopSync *s, uint32_t hash, int64_t *value);
// The digest sums, for tests: two engines with equal sums hold the same synced state.
void coop_sync_sums(const CoopSync *s, uint32_t out[COOP_SYNC_SECTIONS]);

int probe_coop_sync(void);   // coop_sync_probe.c: the engine's part of --probe-coop; failures
