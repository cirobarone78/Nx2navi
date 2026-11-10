// --probe-coop, the progress sync part: the merge engine (coop_state.c) between fake games on a fake network, without a
// game, a socket or a clock (the time is simulated, so a minute of play takes milliseconds).
//
// A fake game is a flag store with the categories of the real one (coop_game.c): flags that only go up, item counts, and
// spendable counters with a cap. It reports its flags to its engine as the game side does (every change), takes what the
// engine hands out, and checks that nothing it applies lowers a flag or an item. The fake network delivers each message
// to every other member (or one), losing, repeating and reordering them as asked.
//
// 1. Merge algebra: the same messages in every order, some twice, give the same state; nothing shrinks.
// 2. A room of two, then a late joiner who catches up with everything, and the room catches up with what it brought.
// 3. A room of four with 30 % of the messages lost, some repeated and reordered: every game ends with the union.
// 4. Rules: they come from the room's maker, a joiner's own settings are ignored, a room without a maker gets the lowest
//    member's, and the modes: items per player, the shared pools (two players spending at once, a cap, a debt paid back,
//    a player who joins with more, a game that restarts), keys per player with progress per player, and a rule switched
//    in the middle of a session (to shared: merges from then on; to per player: stops, keeps what it has).
// 5. Nothing merged survives a change of room or of save: a save loaded after leaving a room, the next room, and another
//    save loaded in a room of one get none of what the last room and the last save had.
// 6. A hostile member: hashes the game does not share, values out of range (a flag set to 2 to freeze it, an item of
//    1000, pool registers of 2^31), contributor ids chosen to collide in the index, a flood; none of it is stored,
//    passed on or applied, the honest values still merge, and the member is ignored after a few dozen.
// 7. Random bytes into the receiver.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_proto.h"
#include "coop_state.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop-sync: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) {
    rng ^= rng >> 12, rng ^= rng << 25, rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32);
}

// ---- a fake game: its flags, in the categories of the real one

enum { MAX_FLAGS = 600, MAX_PLAYERS = 4, RUPEE = 0x52555045, ARROWS = 0x4152524F, KEYS_FOREST = 0x4B455901 };

typedef struct {
    uint32_t hash;
    uint8_t cat;
    int32_t value, reported, cap;   // reported: what the engine last heard
    int known;                      // reported at least once since the last rebind
} Flag;

typedef struct {
    int id;                         // member id
    int made;                       // it made the room: the relay names it the maker
    CoopSync *eng;
    Flag f[MAX_FLAGS];
    int n;
    int lowered;                    // an apply lowered a flag or an item: never allowed
    unsigned applied;
} Game;

static Flag *flag_of(Game *g, uint32_t hash) {
    for (int i = 0; i < g->n; i++)
        if (g->f[i].hash == hash) return &g->f[i];
    return NULL;
}

static Flag *flag_add(Game *g, int cat, uint32_t hash, int32_t value, int32_t cap) {
    Flag *f = flag_of(g, hash);
    if (!f) {
        f = &g->f[g->n++];
        memset(f, 0, sizeof *f);
        f->hash = hash, f->cat = (uint8_t)cat, f->cap = cap;
    }
    f->value = value;
    return f;
}

// The game's table as the engine sees it (coop_game.c's limits): every flag of the fake game in its own category, a
// Bool, song or quest item up to 1, an item up to 4, small keys up to 15 and the other counters up to 999.
static uint32_t fake_limit(int cat, uint32_t hash, void *user) {
    Flag *f = flag_of(user, hash);
    if (!f || f->cat != cat) return 0;
    return cat == COOP_CAT_ITEMS ? 4 : cat == COOP_CAT_KEYS ? 15 : coop_cat_is_pool(cat) ? 999 : 1;
}

// The flag set of the fake game: hashes 1..P are progress flags, 0x1000+ items, 0x2000+ songs, 0x3000+ quest items.
static void game_init(Game *g, int id, uint64_t client) {
    memset(g, 0, sizeof *g);
    g->id = id;
    static const uint8_t index_key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    g->eng = coop_sync_new(client, 0xC0FFEE);
    coop_sync_limits(g->eng, fake_limit, g);
    coop_sync_index_key(g->eng, index_key);
    for (uint32_t h = 1; h <= 300; h++) flag_add(g, COOP_CAT_PROGRESS, h, 0, -1);
    for (uint32_t h = 0x1000; h < 0x1040; h++) flag_add(g, COOP_CAT_ITEMS, h, 0, -1);
    for (uint32_t h = 0x2000; h < 0x200D; h++) flag_add(g, COOP_CAT_SONGS, h, 0, -1);
    for (uint32_t h = 0x3000; h < 0x300B; h++) flag_add(g, COOP_CAT_QUEST, h, 0, -1);
    flag_add(g, COOP_CAT_RUPEES, RUPEE, 0, 200);
    flag_add(g, COOP_CAT_AMMO, ARROWS, 0, 30);
    flag_add(g, COOP_CAT_KEYS, KEYS_FOREST, 0, -1);
}

// What the game side does every frame: report what changed (pools every time), then take what the room has.
static void game_frame(Game *g, uint64_t now) {
    for (int i = 0; i < g->n; i++) {
        Flag *f = &g->f[i];
        if (coop_cat_is_pool(f->cat)) {
            coop_sync_pool_local(g->eng, f->cat, f->hash, f->value, f->cap, now);
        } else if (!f->known || f->value != f->reported) {
            if (f->value || f->known) coop_sync_local(g->eng, f->cat, f->hash, (uint32_t)f->value, now);
            f->reported = f->value, f->known = 1;
        }
    }
    CoopApply a[64];
    int n;
    while ((n = coop_sync_take(g->eng, a, 64)) > 0)
        for (int i = 0; i < n; i++) {
            Flag *f = flag_of(g, a[i].hash);
            if (!f || f->cat != a[i].cat) continue;   // a hash this game does not have: the real game skips it too
            if (a[i].pool) {
                f->value = a[i].value;
                coop_sync_pool_wrote(g->eng, a[i].hash, a[i].value);
            } else {
                if (a[i].value <= f->value) g->lowered++;
                f->value = a[i].value;
            }
            g->applied++;
        }
}

static void game_free(Game *g) { coop_sync_free(g->eng); }

// ---- a fake network

typedef struct {
    int from, to;          // to: 0 every other member
    uint8_t kind;
    uint16_t n;
    uint8_t b[COOP_MAX_PAYLOAD];
} Packet;

typedef struct {
    Packet *q;
    int n, cap;
    unsigned loss, dup, reorder;   // percent
    Game *games[MAX_PLAYERS];
    int present[MAX_PLAYERS];
    int sending;                   // the member a send comes from
    unsigned sent, lost;
} Net;

static int net_send(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user) {
    Net *net = user;
    if (n > sizeof net->q[0].b) return 0;
    if (net->n == net->cap) {
        int nc = net->cap ? net->cap * 2 : 256;
        Packet *nq = realloc(net->q, (size_t)nc * sizeof *nq);
        if (!nq) return 0;
        net->q = nq, net->cap = nc;
    }
    Packet *p = &net->q[net->n++];
    p->from = net->sending, p->to = member, p->kind = kind, p->n = (uint16_t)n;
    memcpy(p->b, bytes, n);
    net->sent++;
    return 1;
}

static void deliver_one(Net *net, const Packet *p, uint64_t now) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Game *g = net->games[i];
        if (!g || !net->present[i] || g->id == p->from || (p->to && p->to != g->id)) continue;
        if (rnd() % 100 < net->loss) {
            net->lost++;
            continue;
        }
        coop_sync_receive(g->eng, p->from, p->kind, p->b, p->n, now);
        if (rnd() % 100 < net->dup) coop_sync_receive(g->eng, p->from, p->kind, p->b, p->n, now);
    }
}

// Deliver what was sent, in a shuffled order when reorder is set.
static void net_flush(Net *net, uint64_t now) {
    while (net->n) {
        int i = net->reorder && rnd() % 100 < net->reorder ? (int)(rnd() % (unsigned)net->n) : 0;
        Packet p = net->q[i];
        memmove(&net->q[i], &net->q[i + 1], (size_t)(net->n - i - 1) * sizeof p);
        net->n--;
        deliver_one(net, &p, now);
    }
}

// Every present game hears the roster: who is there, and the maker the relay names (a present game that made the room).
static void net_roster(Net *net, const char *code, uint64_t now) {
    int ids[MAX_PLAYERS], n = 0, maker = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (net->games[i] && net->present[i]) {
            ids[n++] = net->games[i]->id;
            if (net->games[i]->made) maker = net->games[i]->id;
        }
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (net->games[i] && net->present[i]) coop_sync_room(net->games[i]->eng, code, net->games[i]->id, ids, n, maker, now);
}

// Run every present game for ms of simulated time in 8 ms steps (the coop thread's step in a room, a frame on the game side).
static uint64_t run(Net *net, uint64_t now, unsigned ms) {
    for (unsigned t = 0; t < ms; t += 8) {
        now += 8;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Game *g = net->games[i];
            if (!g || !net->present[i]) continue;
            game_frame(g, now);
            net->sending = g->id;
            coop_sync_tick(g->eng, now, net_send, net);
        }
        net_flush(net, now);
    }
    return now;
}

static int sums_equal(Game *a, Game *b) {
    uint32_t x[COOP_SYNC_SECTIONS], y[COOP_SYNC_SECTIONS];
    coop_sync_sums(a->eng, x);
    coop_sync_sums(b->eng, y);
    return !memcmp(x, y, sizeof x);
}

static int32_t value_of(Game *g, uint32_t hash) {
    Flag *f = flag_of(g, hash);
    return f ? f->value : -1;
}

// Every flag of a category in games[0..n) equals the highest value want[] holds for it.
static int union_holds(Game **gs, int n, const int32_t *want, int cat_mask) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < gs[k]->n; i++) {
            Flag *f = &gs[k]->f[i];
            if (!(cat_mask >> f->cat & 1) || coop_cat_is_pool(f->cat)) continue;
            if (f->value != want[i]) return 0;
        }
    return 1;
}

static void settings(Game *g, int made, int items, int keys, int rupees, int ammo, int progress) {
    CoopRules r;
    coop_rules_default(&r);
    r.mode[COOP_CAT_PROGRESS] = (uint8_t)progress;
    r.mode[COOP_CAT_ITEMS] = (uint8_t)items;
    r.mode[COOP_CAT_KEYS] = (uint8_t)keys;
    r.mode[COOP_CAT_RUPEES] = (uint8_t)rupees;
    r.mode[COOP_CAT_AMMO] = (uint8_t)ammo;
    coop_sync_settings(g->eng, &r, made);
    g->made = made;
}

static void defaults(Game *g, int made) {
    CoopRules r;
    coop_rules_default(&r);
    coop_sync_settings(g->eng, &r, made);
    g->made = made;
}

// ---- 1. merge algebra

// The SECTION messages game i sends about its own batch of flags and items: it sits in a room with a peer that hears only
// its digests, so the peer keeps asking and the game keeps answering (some sections more than once).
static int capture(int i, Packet **out) {
    Game solo, peer;
    game_init(&solo, 1, 200 + (uint64_t)i);
    game_init(&peer, 2, 300 + (uint64_t)i);
    defaults(&solo, 1);
    defaults(&peer, 0);
    for (int k = 0; k < 50; k++) solo.f[(k * 37 + i * 101) % 300].value = 1;
    for (int k = 0; k < 6; k++) solo.f[300 + (k * 11 + i * 7) % 64].value = 1 + (k + i) % 4;
    int ids[2] = {1, 2}, n = 0;
    uint64_t t = 1000;
    coop_sync_room(solo.eng, "ALG001", 1, ids, 2, 1, t);
    coop_sync_room(peer.eng, "ALG001", 2, ids, 2, 1, t);
    Net q = {0};
    for (int step = 0; step < 400; step++) {
        t += 8;
        game_frame(&solo, t);
        q.sending = 1;
        coop_sync_tick(solo.eng, t, net_send, &q);
        for (int k = 0; k < q.n; k++) {
            if (q.q[k].kind == COOP_K_SECTION) {
                *out = realloc(*out, (size_t)(n + 1) * sizeof **out);
                (*out)[n++] = q.q[k];
            } else {
                coop_sync_receive(peer.eng, 1, q.q[k].kind, q.q[k].b, q.q[k].n, t);   // the rules reach the peer
            }
        }
        q.n = 0;
        q.sending = 2;
        coop_sync_tick(peer.eng, t, net_send, &q);
        for (int k = 0; k < q.n; k++) coop_sync_receive(solo.eng, 2, q.q[k].kind, q.q[k].b, q.q[k].n, t);
        q.n = 0;
    }
    free(q.q);
    game_free(&solo);
    game_free(&peer);
    return n;
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static void probe_algebra(void) {
    // three players' messages, delivered to a fresh game in all six orders, the first player's twice
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    Packet *streams[3] = {0};
    int counts[3];
    for (int i = 0; i < 3; i++) counts[i] = capture(i, &streams[i]);
    uint32_t first[COOP_SYNC_SECTIONS];
    int shrank = 0, differ = 0;
    for (int o = 0; o < 6; o++) {
        Game g;
        game_init(&g, 7, 999);
        defaults(&g, 1);
        int ids[2] = {7, 8};
        coop_sync_room(g.eng, "ALG002", 7, ids, 2, 7, 1000);
        Net none = {0};
        none.sending = 7;
        coop_sync_tick(g.eng, 1000, net_send, &none);   // it made the room: its rules (the defaults) are in force
        free(none.q);
        for (int k = 0; k < 3; k++) {
            const int si = orders[o][k];
            for (int m = 0; m < counts[si]; m++)
                for (int rep = 0; rep < (k == 0 ? 2 : 1); rep++) {   // one message every 100 ms: under a member's rate
                    static uint64_t t = 1100;
                    coop_sync_receive(g.eng, 8, COOP_K_SECTION, streams[si][m].b, streams[si][m].n, t += 100);
                }
        }
        // nothing shrinks: every flag a message carried is in the state, at least at the value it carried
        for (int si = 0; si < 3; si++)
            for (int m = 0; m < counts[si]; m++) {
                const uint8_t *b = streams[si][m].b;
                size_t at = 6;
                for (int e = 0; e < b[5]; e++) {
                    if (b[at] == COOP_R_FLAG && coop_sync_flag(g.eng, b[at + 1], le32(b + at + 2)) < le32(b + at + 6)) shrank++;
                    at += b[at] == COOP_R_POOL_GAIN || b[at] == COOP_R_POOL_SPEND ? 18 : 10;
                }
            }
        uint32_t sums[COOP_SYNC_SECTIONS];
        coop_sync_sums(g.eng, sums);
        if (!o) memcpy(first, sums, sizeof sums);
        else if (memcmp(first, sums, sizeof sums)) differ++;
        game_free(&g);
    }
    CHECK(counts[0] && counts[1] && counts[2], "every player sent its sections (%d %d %d messages)", counts[0], counts[1], counts[2]);
    CHECK(!differ, "%d of 6 orders give another state", differ);
    CHECK(!shrank, "%d values are lower than a message said", shrank);
    for (int i = 0; i < 3; i++) free(streams[i]);
    printf("coop-probe: sync algebra: %d + %d + %d messages in 6 orders, the first player's twice, give one state\n", counts[0],
           counts[1], counts[2]);
}

// ---- 2 and 3. rooms

static void probe_rooms(unsigned loss, int players, int late) {
    Net net = {.loss = loss, .dup = loss ? 5 : 0, .reorder = loss ? 20 : 0};
    Game g[MAX_PLAYERS];
    int32_t want[MAX_FLAGS] = {0};
    for (int i = 0; i < players; i++) {
        game_init(&g[i], i + 1, 0xA000 + (uint64_t)i);
        defaults(&g[i], i == 0);
        net.games[i] = &g[i];
        net.present[i] = !(late && i == players - 1);
    }
    uint64_t now = 1000;
    net_roster(&net, "ROOM01", now);
    // two minutes of play: each game gains flags and items now and then; the late joiner plays alone meanwhile
    for (int sec = 0; sec < 120; sec++) {
        for (int i = 0; i < players; i++)
            if (rnd() % 3 == 0) {
                int k = (int)(rnd() % 364);
                int32_t v = k < 300 ? 1 : 1 + (int32_t)(rnd() % 3);
                if (v > g[i].f[k].value) g[i].f[k].value = v;
            }
        if (late && sec == 60) {   // the last player joins half way
            net.present[players - 1] = 1;
            net_roster(&net, "ROOM01", now);
        }
        now = run(&net, now, 1000);
    }
    for (int i = 0; i < players; i++)   // the union of everything any game reached
        for (int k = 0; k < g[i].n; k++)
            if (g[i].f[k].value > want[k] && !coop_cat_is_pool(g[i].f[k].cat)) want[k] = g[i].f[k].value;
    now = run(&net, now, 20000);   // quiet: let the digests repair what was lost
    Game *gs[MAX_PLAYERS];
    for (int i = 0; i < players; i++) gs[i] = &g[i];
    int all = 1;
    for (int i = 1; i < players; i++) all &= sums_equal(&g[0], &g[i]);
    unsigned lowered = 0, applied = 0;
    for (int i = 0; i < players; i++) lowered += (unsigned)g[i].lowered, applied += g[i].applied;
    CHECK(all, "%d players at %u %% loss: the states differ", players, loss);
    CHECK(union_holds(gs, players, want, 0xF), "%d players at %u %% loss: a game lacks part of the union", players, loss);
    CHECK(!lowered, "%u applies lowered a value", lowered);
    printf("coop-probe: sync %d players%s, %u %% loss: every game holds the union (%u applies, %u of %u messages lost)\n", players,
           late ? " (one joins late)" : "", loss, applied, net.lost, net.sent);
    for (int i = 0; i < players; i++) game_free(&g[i]);
    free(net.q);
}

// ---- 4. rules and modes

static void probe_rules(void) {
    Net net = {0};
    Game a, b, c;
    game_init(&a, 1, 0xA1);
    game_init(&b, 2, 0xB2);
    game_init(&c, 3, 0xC3);
    settings(&a, 1, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN);   // progress own: items may be own
    settings(&b, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);  // ignored: A made the room
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &c;
    net.present[0] = net.present[1] = 1;
    a.f[0].value = 1;   // a progress flag
    a.f[300].value = 1; // an item
    flag_of(&a, RUPEE)->value = 50;
    flag_of(&b, RUPEE)->value = 30;
    flag_of(&a, ARROWS)->value = 10;
    flag_of(&b, ARROWS)->value = 20;
    flag_of(&a, KEYS_FOREST)->value = 1;
    uint64_t now = run(&net, (net_roster(&net, "RULE01", 1000), 1000), 3000);
    CoopRules ra, rb;
    coop_sync_rules(a.eng, &ra);
    coop_sync_rules(b.eng, &rb);
    CHECK(ra.epoch && !memcmp(ra.mode, rb.mode, COOP_CAT_N) && rb.maker == 1, "the joiner uses the maker's rules");
    CHECK(rb.mode[COOP_CAT_KEYS] == COOP_MODE_OWN, "with progress per player keys are per player (normalized), not %d", rb.mode[COOP_CAT_KEYS]);
    CHECK(value_of(&b, 1) == 0 && value_of(&b, 0x1000) == 0, "progress and items per player stay with A");
    CHECK(value_of(&b, KEYS_FOREST) == 0, "keys per player stay with A");
    CHECK(value_of(&a, RUPEE) == 50 && value_of(&b, RUPEE) == 50, "a joiner with fewer rupees gets the pool's 50 (%d %d)", value_of(&a, RUPEE),
          value_of(&b, RUPEE));
    CHECK(value_of(&a, ARROWS) == 20 && value_of(&b, ARROWS) == 20, "the arrow pool takes the higher count (%d %d)", value_of(&a, ARROWS),
          value_of(&b, ARROWS));

    // two players spend at once, before either hears of the other's: both spends count
    flag_of(&a, RUPEE)->value -= 30;
    flag_of(&b, RUPEE)->value -= 15;
    now = run(&net, now, 2000);
    CHECK(value_of(&a, RUPEE) == 5 && value_of(&b, RUPEE) == 5, "two spends at once: 50 - 30 - 15 = 5 (%d %d)", value_of(&a, RUPEE),
          value_of(&b, RUPEE));
    // both spend the last units at once: the pool goes below zero, shows 0, and the next gain pays the debt first
    flag_of(&a, RUPEE)->value -= 5;
    flag_of(&b, RUPEE)->value -= 5;
    now = run(&net, now, 2000);
    int64_t v;
    coop_sync_pool(a.eng, RUPEE, &v);
    CHECK(v == -5 && value_of(&a, RUPEE) == 0 && value_of(&b, RUPEE) == 0, "spent twice: the pool is -5 and shows 0 (%lld)", (long long)v);
    flag_of(&b, RUPEE)->value += 20;
    now = run(&net, now, 2000);
    CHECK(value_of(&a, RUPEE) == 15 && value_of(&b, RUPEE) == 15, "a gain of 20 after a debt of 5 shows 15 (%d %d)", value_of(&a, RUPEE),
          value_of(&b, RUPEE));
    // gains at once from both, past the cap: the pool counts both, each game shows its cap
    flag_of(&a, ARROWS)->value = 30;
    flag_of(&b, ARROWS)->value = 30;
    now = run(&net, now, 2000);
    coop_sync_pool(a.eng, ARROWS, &v);
    CHECK(v == 40 && value_of(&a, ARROWS) == 30 && value_of(&b, ARROWS) == 30, "two gains of 10 at once: pool 40, both show the cap 30 (%lld)",
          (long long)v);
    flag_of(&a, ARROWS)->value -= 5;   // a spend from a capped count: the pool falls to 35, still over the cap
    now = run(&net, now, 2000);
    CHECK(value_of(&a, ARROWS) == 30 && value_of(&b, ARROWS) == 30, "a spend of 5 from 40 still shows the cap (%d %d)", value_of(&a, ARROWS),
          value_of(&b, ARROWS));

    // a third player joins with more rupees than the pool: the pool rises to it, nobody loses any
    flag_of(&c, RUPEE)->value = 120;
    settings(&c, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);
    net.present[2] = 1;
    net_roster(&net, "RULE01", now);
    now = run(&net, now, 4000);
    CHECK(value_of(&a, RUPEE) == 120 && value_of(&b, RUPEE) == 120 && value_of(&c, RUPEE) == 120, "a joiner with 120 raises the pool to 120 (%d %d %d)",
          value_of(&a, RUPEE), value_of(&b, RUPEE), value_of(&c, RUPEE));

    // B's game restarts: a new engine, its save had the 120 it showed; the pool does not pay it twice
    int32_t saved = value_of(&b, RUPEE);
    game_free(&b);
    game_init(&b, 2, 0xB2);
    settings(&b, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);
    flag_of(&b, RUPEE)->value = saved;
    net_roster(&net, "RULE01", now);
    now = run(&net, now, 4000);
    coop_sync_pool(a.eng, RUPEE, &v);
    CHECK(v == 120 && value_of(&b, RUPEE) == 120, "a restarted game rejoins the pool without adding to it (%lld, %d)", (long long)v, value_of(&b, RUPEE));

    // mid-session: the maker switches items to shared; what A got while they were per player reaches the others, and later items too
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN);
    now = run(&net, now, 4000);
    CHECK(value_of(&b, 0x1000) == 1 && value_of(&c, 0x1000) == 1, "items switched to shared: A's earlier item reaches B and C");
    b.f[301].value = 2;
    now = run(&net, now, 3000);
    CHECK(value_of(&a, 0x1001) == 2 && value_of(&c, 0x1001) == 2, "items shared: B's new item reaches A and C");
    // and back to per player: nothing merges any more, nothing merged is taken back
    settings(&a, 1, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_OWN);
    now = run(&net, now, 3000);
    c.f[302].value = 1;
    flag_of(&c, RUPEE)->value = 7;   // rupees per player now: C's change stays with C
    now = run(&net, now, 3000);
    CHECK(value_of(&a, 0x1002) == 0 && value_of(&b, 0x1002) == 0, "items switched to per player: C's new item stays with C");
    CHECK(value_of(&a, 0x1000) == 1 && value_of(&b, 0x1001) == 2, "items switched to per player: what was merged stays");
    CHECK(value_of(&a, RUPEE) == 120 && value_of(&b, RUPEE) == 120 && value_of(&c, RUPEE) == 7, "rupees switched to per player: each keeps its own");
    CHECK(!a.lowered && !b.lowered && !c.lowered, "no apply lowered a flag or an item");

    // the maker leaves; a new member still gets the room's rules from the others
    net.present[0] = 0;
    Game d;
    game_init(&d, 4, 0xD4);
    defaults(&d, 0);
    net.games[3] = &d, net.present[3] = 1;
    net_roster(&net, "RULE01", now);
    now = run(&net, now, 3000);
    CoopRules rd;
    coop_sync_rules(d.eng, &rd);
    CHECK(rd.epoch && rd.mode[COOP_CAT_ITEMS] == COOP_MODE_OWN, "the maker left: a joiner adopts the rules the others hold");
    game_free(&a), game_free(&b), game_free(&c), game_free(&d);
    free(net.q);

    // a room whose maker is gone before anyone heard the rules: nobody else makes any, nothing merges
    Net n2 = {0};
    Game x, y;
    game_init(&x, 5, 0x55);
    game_init(&y, 3, 0x33);
    settings(&x, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_SHARED);
    settings(&y, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_SHARED);
    x.f[0].value = 1;
    n2.games[0] = &x, n2.games[1] = &y, n2.present[0] = n2.present[1] = 1;
    now = run(&n2, (net_roster(&n2, "RULE02", 1000), 1000), 8000);
    CoopRules rx;
    coop_sync_rules(x.eng, &rx);
    CHECK(!rx.epoch && value_of(&y, 1) == 0, "a room without its maker: no member makes rules, nothing merges (epoch %u)", rx.epoch);
    game_free(&x), game_free(&y);
    free(n2.q);

    // normalization: with progress shared, items, songs and quest items are shared whatever the settings say
    CoopRules r;
    coop_rules_default(&r);
    r.mode[COOP_CAT_SONGS] = COOP_MODE_OWN;
    r.mode[COOP_CAT_RUPEES] = COOP_MODE_SHARED;   // not a mode rupees have
    unsigned changed = coop_rules_normalize(&r);
    CHECK(r.mode[COOP_CAT_SONGS] == COOP_MODE_SHARED && r.mode[COOP_CAT_RUPEES] == COOP_MODE_OWN &&
              changed == (1u << COOP_CAT_SONGS | 1u << COOP_CAT_RUPEES), "normalize: songs shared with progress shared, rupees own");
    printf("coop-probe: sync rules: the maker's rules hold, pools spend and gain at once, rules switch mid-session\n");
}

// Keys in a pool with progress shared: a key one player picks up opens a door for the other; the door spends it for both.
static void probe_keys(void) {
    Net net = {.loss = 30, .dup = 5, .reorder = 20};
    Game a, b;
    game_init(&a, 1, 0x1A);
    game_init(&b, 2, 0x2B);
    defaults(&a, 1);
    defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    // at 30 % loss the room is known (and the pools join) after a few digests; each step waits several of them
    uint64_t now = run(&net, (net_roster(&net, "KEYS01", 1000), 1000), 15000);
    flag_of(&a, KEYS_FOREST)->value = 1;   // A picks up a key (its pickup flag is progress)
    a.f[10].value = 1;
    now = run(&net, now, 15000);
    CHECK(value_of(&b, KEYS_FOREST) == 1 && value_of(&b, 11) == 1, "A's key and its pickup reach B");
    flag_of(&b, KEYS_FOREST)->value = 0;   // B opens a door with it
    b.f[20].value = 1;
    now = run(&net, now, 15000);
    CHECK(value_of(&a, KEYS_FOREST) == 0 && value_of(&a, 21) == 1, "B's door spends the key for A too, and opens for A");
    CHECK(!a.lowered && !b.lowered, "no apply lowered a flag");
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync keys: a key picked up by one opens a door for the other, once, at 30 %% loss\n");
}

// ---- 5. carry-over between rooms and saves

static void reload_save(Game *g) {   // the game loads another save: every flag at its default, reported again
    for (int k = 0; k < g->n; k++) g->f[k].value = 0, g->f[k].reported = 0, g->f[k].known = 0;
    coop_sync_rebind(g->eng);
}

static void probe_carry_over(void) {
    Net net = {0};
    Game x, p, q;
    game_init(&x, 1, 0x51);
    game_init(&p, 2, 0x52);
    game_init(&q, 3, 0x53);
    defaults(&x, 1);
    defaults(&p, 0);
    for (int k = 0; k < 10; k++) x.f[k].value = 1;    // save X: progress flags 1..10
    for (int k = 10; k < 20; k++) p.f[k].value = 1;   // P: 11..20
    net.games[0] = &x, net.games[1] = &p, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "ROOMAA", 1000), 1000), 5000);
    CHECK(value_of(&x, 11) == 1 && value_of(&p, 1) == 1, "carry-over: room A merged X and P");
    // X leaves room A, loads save Y (only flag 31), and joins room B, which Q made (Q has flag 41)
    net.present[1] = 0;
    coop_sync_room(x.eng, "", 0, NULL, 0, 0, now);
    reload_save(&x);
    x.f[30].value = 1;
    q.f[40].value = 1;
    defaults(&x, 0);
    defaults(&q, 1);
    net.games[2] = &q, net.present[2] = 1;
    now = run(&net, (net_roster(&net, "ROOMBB", now), now), 6000);
    int leaked = 0;
    for (uint32_t h = 1; h <= 20; h++) leaked += value_of(&x, h) != 0 || value_of(&q, h) != 0;
    CHECK(!leaked, "carry-over: %d flags of room A and save X reached save Y or room B", leaked);
    CHECK(value_of(&q, 31) == 1 && value_of(&x, 41) == 1, "carry-over: room B merges Y and Q");
    // in a room of its own, X loads save Z: nothing of Y reaches it
    net.present[2] = 0;
    defaults(&x, 1);
    now = run(&net, (net_roster(&net, "ROOMCC", now), now), 3000);
    reload_save(&x);
    x.f[50].value = 1;
    now = run(&net, now, 3000);
    CHECK(value_of(&x, 31) == 0 && value_of(&x, 41) == 0 && value_of(&x, 51) == 1, "carry-over: save Z got nothing of save Y (%d %d)",
          value_of(&x, 31), value_of(&x, 41));
    game_free(&x), game_free(&p), game_free(&q);
    free(net.q);
    printf("coop-probe: sync carry-over: a new room and a new save start with nothing of the last ones\n");
}

// ---- 6. a hostile member

static uint8_t *entry_flag(uint8_t *p, int cat, uint32_t hash, uint32_t v) {
    *p++ = COOP_R_FLAG, *p++ = (uint8_t)cat;
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(hash >> 8 * i);
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(v >> 8 * i);
    return p;
}

static uint8_t *entry_pool(uint8_t *p, int kind, int cat, uint32_t hash, uint64_t who, uint32_t v) {
    *p++ = (uint8_t)kind, *p++ = (uint8_t)cat;
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(hash >> 8 * i);
    if (kind != COOP_R_POOL_BASE)
        for (int i = 0; i < 8; i++) *p++ = (uint8_t)(who >> 8 * i);
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(v >> 8 * i);
    return p;
}

// A SECTION message of the entries in [b + 6, end) (count of them) from member from to games a and b.
static void inject(Game *a, Game *b, int from, uint8_t *buf, uint8_t *end, int count, uint64_t now) {
    buf[0] = COOP_K_DIGEST_VER, buf[1] = 0xEE, buf[2] = 0xFF, buf[3] = 0xC0, buf[4] = 0, buf[5] = (uint8_t)count;
    coop_sync_receive(a->eng, from, COOP_K_SECTION, buf, (size_t)(end - buf), now);
    if (b) coop_sync_receive(b->eng, from, COOP_K_SECTION, buf, (size_t)(end - buf), now);
}

static uint64_t old_slot_mix(uint64_t x) {   // the unkeyed index hash the engine used before: an attacker can invert it
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void probe_hostile(void) {
    uint8_t msg[15];
    for (int i = 0; i < 15; i++) msg[i] = (uint8_t)i;
    uint64_t k0 = 0x0706050403020100ull, k1 = 0x0f0e0d0c0b0a0908ull;   // the paper's key 00..0f and message 00..0e
    CHECK(coop_siphash24(k0, k1, msg, 15) == 0xa129ca6149be45e5ull && coop_siphash24(k0, k1, msg, 0) == 0x726fdb47dd0e0e31ull,
          "SipHash-2-4 reference vectors");

    Net net = {0};
    Game a, b, h;
    game_init(&a, 1, 0x61);
    game_init(&b, 2, 0x62);
    game_init(&h, 3, 0x63);
    defaults(&a, 1), defaults(&b, 0), defaults(&h, 0);
    flag_of(&a, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &h, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "HOST01", 1000), 1000), 4000);
    uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    for (uint32_t k = 0; k < 10; k++) p = entry_flag(p, COOP_CAT_PROGRESS, 0xDEAD0000 + k, 1);   // not flags of the game
    p = entry_flag(p, COOP_CAT_PROGRESS, 5, 2);                                                    // a flag at 2: frozen
    p = entry_flag(p, COOP_CAT_ITEMS, 0x1000, 1000);                                               // an item out of range
    p = entry_pool(p, COOP_R_POOL_SPEND, COOP_CAT_KEYS, KEYS_FOREST, 0x1234, 0xFFFFFFFFu);        // keys to 0 for good
    p = entry_pool(p, COOP_R_POOL_GAIN, COOP_CAT_KEYS, KEYS_FOREST, 0x5678, 0x7FFFFFFFu);         // or to 2^31
    p = entry_pool(p, COOP_R_POOL_BASE, COOP_CAT_KEYS, KEYS_FOREST, 0, 0xFFFFFFFFu);
    inject(&a, &b, 3, buf, p, 15, now);
    now = run(&net, now, 2000);
    a.f[4].value = 1;   // the honest value of flag 5
    now = run(&net, now, 3000);
    int64_t ka = -1, kb = -1;
    coop_sync_pool(a.eng, KEYS_FOREST, &ka);
    coop_sync_pool(b.eng, KEYS_FOREST, &kb);
    CHECK(!coop_sync_flag(a.eng, COOP_CAT_PROGRESS, 0xDEAD0001) && !coop_sync_flag(b.eng, COOP_CAT_PROGRESS, 0xDEAD0009),
          "hostile: hashes the game does not share are not stored");
    CHECK(value_of(&b, 5) == 1 && value_of(&a, 5) == 1, "hostile: a flag sent as 2 does not freeze it: the honest 1 reaches B (%d)", value_of(&b, 5));
    CHECK(value_of(&a, 0x1000) == 0 && value_of(&b, 0x1000) == 0, "hostile: an item of 1000 is not applied (%d)", value_of(&b, 0x1000));
    CHECK(ka == 2 && kb == 2 && value_of(&a, KEYS_FOREST) == 2 && value_of(&b, KEYS_FOREST) == 2,
          "hostile: pool registers of 2^32 leave the key pool at 2 (%lld %lld, shown %d %d)", (long long)ka, (long long)kb,
          value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST));
    CoopSyncStats st;
    coop_sync_stats(b.eng, &st);
    CHECK(st.rejected >= 15 && !(coop_sync_ignored(b.eng) >> 3 & 1), "hostile: 15 refused values counted (%u), member 3 not yet ignored", st.rejected);
    // more of them: member 3 is ignored, even for a value the game would take
    p = buf + 6;
    for (uint32_t k = 0; k < 20; k++) p = entry_flag(p, COOP_CAT_SONGS, 0xBAD00000 + k, 1);
    inject(&a, &b, 3, buf, p, 20, now);
    p = entry_flag(buf + 6, COOP_CAT_PROGRESS, 7, 1);
    inject(&a, &b, 3, buf, p, 1, now + 10);
    CHECK((coop_sync_ignored(a.eng) >> 3 & 1) && (coop_sync_ignored(b.eng) >> 3 & 1) && !coop_sync_flag(b.eng, COOP_CAT_PROGRESS, 7),
          "hostile: after 35 refused values member 3 is ignored (%x)", coop_sync_ignored(b.eng));
    // a flood from member 2 at one instant: past its rate the messages are dropped
    coop_sync_stats(a.eng, &st);
    unsigned dropped = st.rate_dropped;
    p = buf + 6;
    for (int k = 0; k < 100; k++) p = entry_flag(p, COOP_CAT_PROGRESS, 1 + (uint32_t)k, 0);
    for (int k = 0; k < 200; k++) inject(&a, NULL, 2, buf, p, 100, now + 20);
    coop_sync_stats(a.eng, &st);
    CHECK(st.rate_dropped > dropped + 100, "hostile: a flood of 200 KB at once from one member is rate limited (%u dropped)", st.rate_dropped - dropped);
    CHECK(!a.lowered && !b.lowered, "hostile: no apply lowered a value");
    game_free(&a), game_free(&b), game_free(&h);
    free(net.q);

    // contributor ids that the unkeyed hash put in one run of slots, as many as a member likes
    Game c, d;
    Net n2 = {0};
    game_init(&c, 1, 0x71);
    game_init(&d, 2, 0x72);
    defaults(&c, 1), defaults(&d, 0);
    n2.games[0] = &c, n2.games[1] = &d, n2.present[0] = n2.present[1] = 1;
    now = run(&n2, (net_roster(&n2, "HOST02", now), now), 3000);
    CoopSyncStats before;
    coop_sync_stats(c.eng, &before);
    int sent = 0;
    p = buf + 6;
    int count = 0;
    for (uint64_t who = 1; sent < 600; who++) {
        uint64_t key = old_slot_mix(((uint64_t)COOP_R_POOL_GAIN << 56) ^ KEYS_FOREST ^ old_slot_mix(who));
        if (key & 0xFFFF) continue;
        p = entry_pool(p, COOP_R_POOL_GAIN, COOP_CAT_KEYS, KEYS_FOREST, who, 1);
        sent++, count++;
        if (count == 60) {
            inject(&c, NULL, 2, buf, p, count, now += 100);
            p = buf + 6, count = 0;
        }
    }
    coop_sync_stats(c.eng, &st);
    CHECK(st.regs - before.regs <= 256 && st.probe_max < 40, "index: 600 chosen contributor ids make %u registers, the longest probe %u",
          st.regs - before.regs, st.probe_max);
    game_free(&c), game_free(&d);
    free(n2.q);
    printf("coop-probe: sync hostile member: refused values are not stored, passed on or applied, a frozen flag still merges, the "
           "member is ignored, a flood is limited, the index stays short (%u)\n", st.probe_max);
}

// A member who is not the maker sends digests with the epoch at 2^32-1 and every category shared or pooled, in the
// digest format of this engine and of the one before (which had an owner field): the rules stay the maker's, and the
// maker's next change still reaches everyone.
static size_t forge_digest(uint8_t *b, int version) {
    size_t at = 0;
    b[at++] = (uint8_t)version;
    b[at++] = 0xEE, b[at++] = 0xFF, b[at++] = 0xC0, b[at++] = 0;
    for (int i = 0; i < 4; i++) b[at++] = 0xFF;   // epoch 2^32-1
    if (version == 1)
        for (int i = 0; i < 8; i++) b[at++] = 0xFF;   // the owner field the version-1 digest had
    b[at++] = COOP_CAT_N;
    for (int c = 0; c < COOP_CAT_N; c++) b[at++] = coop_cat_is_pool(c) ? COOP_MODE_POOL : COOP_MODE_SHARED;
    b[at++] = COOP_SYNC_SECTIONS;
    memset(b + at, 0, 4 * COOP_SYNC_SECTIONS);
    return at + 4 * COOP_SYNC_SECTIONS;
}

static void probe_takeover(void) {
    Net net = {0};
    Game a, b, h;
    game_init(&a, 1, 0x81);
    game_init(&b, 2, 0x82);
    game_init(&h, 3, 0x83);
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);   // rupees own
    defaults(&b, 0), defaults(&h, 0);
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &h, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "TAKE01", 1000), 1000), 3000);
    uint8_t d[400];
    for (int version = 1; version <= 2; version++) {
        size_t n = forge_digest(d, version);
        coop_sync_receive(b.eng, 3, COOP_K_DIGEST, d, n, now += 10);
        coop_sync_receive(a.eng, 3, COOP_K_DIGEST, d, n, now += 10);
    }
    CoopRules ra, rb;
    coop_sync_rules(a.eng, &ra);
    coop_sync_rules(b.eng, &rb);
    CHECK(rb.mode[COOP_CAT_RUPEES] == COOP_MODE_OWN && ra.mode[COOP_CAT_RUPEES] == COOP_MODE_OWN && rb.epoch != UINT32_MAX,
          "takeover: a member who is not the maker cannot change the rules (rupees %s, epoch %u)", coop_mode_name(rb.mode[COOP_CAT_RUPEES]),
          rb.epoch);
    now = run(&net, now, 3000);
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_SHARED);   // the maker pools rupees
    now = run(&net, now, 3000);
    coop_sync_rules(b.eng, &rb);
    CHECK(rb.mode[COOP_CAT_RUPEES] == COOP_MODE_POOL && rb.mode[COOP_CAT_AMMO] == COOP_MODE_OWN,
          "takeover: the maker's next change still reaches everyone (rupees %s, ammo %s)", coop_mode_name(rb.mode[COOP_CAT_RUPEES]),
          coop_mode_name(rb.mode[COOP_CAT_AMMO]));
    game_free(&a), game_free(&b), game_free(&h);
    free(net.q);
    printf("coop-probe: sync rules takeover: a member's digest at epoch 2^32-1 changes nothing; the maker's changes still hold\n");
}

// ---- 7. hostile bytes

static uint32_t fuzz_limit(int cat, uint32_t hash, void *user) {
    (void)user;
    return hash & 1 ? 0 : coop_cat_is_pool(cat) ? 999 : 1;
}

static void probe_sync_fuzz(void) {
    CoopSync *s = coop_sync_new(1, 0xC0FFEE);
    coop_sync_limits(s, fuzz_limit, NULL);
    CoopRules r;
    coop_rules_default(&r);
    coop_sync_settings(s, &r, 1);
    int ids[2] = {1, 2};
    coop_sync_room(s, "FUZZ01", 1, ids, 2, 1, 1000);
    Net none = {0};
    coop_sync_tick(s, 1000, net_send, &none);
    uint8_t b[COOP_MAX_PAYLOAD];
    for (int i = 0; i < 200000; i++) {
        size_t n = rnd() % sizeof b;
        for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rnd();
        if (n > 6 && rnd() % 2) b[0] = COOP_K_DIGEST_VER, b[1] = 0xEE, b[2] = 0xFF, b[3] = 0xC0, b[4] = 0;   // a valid header
        coop_sync_receive(s, 1 + (int)(rnd() % 9), (uint8_t)(rnd() % 2 ? COOP_K_SECTION : COOP_K_DIGEST), b, n, 1000 + (uint64_t)i);
        if (i % 1000 == 0) {
            coop_sync_tick(s, 1000 + (uint64_t)i, net_send, &none);
            none.n = 0;
            CoopApply a[64];
            while (coop_sync_take(s, a, 64) > 0) {}
        }
    }
    CoopSyncStats st;
    coop_sync_stats(s, &st);
    CHECK(st.regs <= 65536, "registers stay bounded (%u)", st.regs);
    coop_sync_free(s);
    free(none.q);
    printf("coop-probe: sync fuzz: 200000 random messages, %u registers made\n", st.regs);
}

int probe_coop_sync(void) {
    fails = 0;
    probe_algebra();
    probe_rooms(0, 2, 1);
    probe_rooms(30, 4, 1);
    probe_rooms(30, 4, 0);
    probe_rules();
    probe_keys();
    probe_carry_over();
    probe_hostile();
    probe_takeover();
    probe_sync_fuzz();
    return fails;
}
