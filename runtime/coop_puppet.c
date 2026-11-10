// Co-op puppets, game 1.1.1: the other players of a co-op room shown in this game as Links of their own (in progress).
//
// The hook. The framework's per-frame update (main+0x6e6be0) steps the time manager (main+0x3b4e300) before it updates
// the scene. The patch set "coop_puppet" (recomp/patches-v111.txt) turns that bl into a call of puppet_frame on the patch
// page (+0xf00), which makes the call itself and then does the puppets' work on the game's update thread, between two
// frames: spawning and removing actors, posing them, asking for their animations.
//
// Actors. The actor manager *(main+0x4e91888) takes create requests (main+0x372a70): x1 the address of the actor
// name's pointer ("Pack/Actor/<name>.pack" is loaded), x2 the placement (+0x00 position, +0x0c a 3x3 rotation, +0x30
// scale, +0x40 a text, +0x48 a byte, +0x98 a mode byte), x3 a cell the game puts the request into (a 0x30-byte pool
// entry: +0 its state in the low 16 bits, +8 the actor), w4 1, x5..x7 0, then on the stack a byte 0, a u32 for the
// outcome, and 0. The actor is made (actor state 3) but is in no list of the actor system: when the request reaches
// state 3 its asker must take the actor (main+0x36fbd0(request, 1, 0), as the game's own askers do at main+0x32bcb88
// and main+0xbcd89c). The take wakes it (main+0x94f30: flags at actor +0x30, then main+0xb3db0 links it into the
// actor system's list at +0x3e8), and from that list it enters the scene (state 4) and is updated and drawn; the
// request is the game's again. An actor never taken stays in state 3 for good, not drawn. A request the game gives up
// (state 4) is dropped (main+0x36fde0), one that failed (5, 6) released. A taken actor is removed with the game's
// generic delete (main+0xa6490(actor, 0), the call behind the AI's "delete this actor"); a request not yet made is
// cancelled in the pending queue (main+0xb2e020 over *(main+0x4e90d90)+0x458, with a matcher whose vtable is
// main+0x4c595e0, then main+0xb2e6e0 frees it), else main+0xb2e2e0(request, 1) deletes its actor.
//
// Posing: the engine's transform setter main+0xc6fd60(actor, 3x4 matrix, 0) hands the transform to the actor's physics
// body or keeps it for the actor's next update (+0x358, +0x364); the actor's own copies (+0x31c, +0x328) are written
// as well so that a read in the same frame sees it.
//
// Status (game 1.1.1, run-checked): a pot and the puppet (CoopPuppet_Child / _Adult, coop_puppet_files.c) are made,
// taken and drawn where they were asked for, the puppet plays Link's nodes and follows a pose, and two games in one
// room each make the other's Link as a puppet. The puppet's arms stay half raised where Link's hang down, and it wears
// no equipment, so the other players' puppets stay off unless NX2NAVI_COOP_PUPPETS is set.
// NX2NAVI_COOP_PUPPET_TEST=spike|shadow are the one-game tests.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_offer.h"
#include "coop_puppet.h"
#include "host.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE

#define MAIN 0x7000000000ull
#define HOST_FRAME (RT_PATCH_PAGE + 0xf00)
#define TIME_STEP (MAIN + 0x3b4e300)        // the call the site replaces: the time manager's step (x0, s0)
#define ACTOR_MGR (MAIN + 0x4e91888)        // *: the actor manager
#define CREATE_FN (MAIN + 0x372a70)         // the create request
#define ACTOR_SYS (MAIN + 0x4e90d90)        // *: +0x458 the queue of pending creates
#define QUEUE_FIND (MAIN + 0xb2e020)        // (queue, matcher): a pending create the matcher names is taken out (1)
#define MATCH_VTBL (MAIN + 0x4c595e0)       // the matcher: {vtable, address of the request cell}
#define HANDLE_FREE (MAIN + 0xb2e6e0)       // (request): a request whose create never ran goes back to its pool
#define HANDLE_DELETE (MAIN + 0xb2e2e0)     // (request, 1): the actor the request holds is deleted
#define CREATE_TAKE (MAIN + 0x36fbd0)       // (request, 1, 0) -> the actor, woken into the scene
#define CREATE_DROP (MAIN + 0x36fde0)       // (request): a request in state 4 is given up
#define ACTOR_DELETE (MAIN + 0xa6490)       // (actor, 0): the game's generic delete of an actor
#define ACTOR_SET_MTX (MAIN + 0xc6fd60)     // (actor, 3x4 row-major matrix, 0): the engine's transform setter
#define EMPTY_TEXT (MAIN + 0x3ee4f8c)       // "" in main's read-only data
#define PLAYER_HOLDER (MAIN + 0x4e371d0)    // *: +0x480 Link's actor
#define AS_PLAY (MAIN + 0x2022a0)           // (request): an AS node by name on an actor's AS slot
#define AS_FRAME_GET (MAIN + 0x37b57c0)     // (AS control, slot) -> s0
#define AS_FRAME_SET (MAIN + 0x37b5d70)     // (AS control, slot, s0)
#define FLAGS_MGR (MAIN + 0x4e842a8)        // *: the game's flag database (coop_game.c)

// Actor layout
#define A_STATE 0x2c        // u32: 3 made, 4 in the scene
#define A_SLEEP 0x30        // u32: flags the take sets while the actor waits in the actor system's list (0 in the scene)
#define A_DEF_NAME 0x260    // char *: the actor pack's name
#define A_COMPONENTS 0x270  // -> +0x40 -> +0x18 -> +0x18: the AS control
#define A_POS 0x31c         // float[3]
#define A_ROT 0x328         // float[9], row-major
#define A_POS2 0x2ec        // a second copy of the transform
#define A_ROT2 0x2f8

// The String64 flag of the current area: container +0x608 of the flag database, entries of 0x108 bytes, text at +0x68.
#define S64_BASE 0x608
#define S64_SIZE 0x108
#define S64_TEXT 0x68
#define F_CURRENT_AREA 0x1d6189dau   // Sequence_CurrentBanc

typedef struct {
    uint64_t name;          // char * of the actor's name: the request's x1 points here
    uint64_t handle;        // the create request (x3 points here) until its actor is taken
    uint32_t status;        // the request's outcome
    uint32_t pad;
    uint8_t place[0xa0];    // the placement (x2)
    uint64_t matcher[2];    // for the release
    uint64_t actor;         // the actor once taken
    char text[64];          // the name's bytes
} __attribute__((aligned(16))) Spawn;

// The local Link's pose of one frame, for the shadow test: transform and, per AS slot, the node it plays and its frame.
#define SLOTS 4
typedef struct {
    float pos[3], rot[9];
    int nslots;
    char node[SLOTS][64];
    float frame[SLOTS];
} Pose;
#define DELAY 60      // frames: the shadow is the local Link of one second ago
#define RING 64

static struct {
    int test;               // NX2NAVI_COOP_PUPPET_TEST: 1 spike, 2 shadow
    int remote;             // NX2NAVI_COOP_PUPPETS: the other players' puppets
    unsigned cfg_seen;
    int set;                // the patch set's index
    int want;               // the present thread's verdict: the hook has work
    // the game thread's
    uint32_t frames, player_frames;
    uint64_t player;
    char area[80];
    char anim[4][64], menu_anim[4][64];
    Spawn spawn[2];
    uint64_t actor[2];
    int sp_state[2], played;
    uint32_t asked_frame[2];
    // the shadow test
    char shadow_name[64];   // "": CoopPuppet_<Link's age>
    float side;             // metres to the left of where Link was
    Pose ring[RING];
} P;

static int ok_ptr(uint64_t a, uint64_t n) { return a >= 0x10000 && a < (1ull << 48) && mem_readable(a, n); }

static float f32_at(uint64_t a) {
    float f;
    memcpy(&f, (const void *)(uintptr_t)a, 4);
    return f;
}

// A C string of the guest at a (at most cap - 1 bytes); "" when a is not readable.
static const char *text_at(uint64_t a, char *out, size_t cap) {
    out[0] = 0;
    if (!ok_ptr(a, 1)) return out;
    size_t i = 0;
    for (; i + 1 < cap && ok_ptr(a + i, 1); i++) {
        char ch = (char)LD8(a + i);
        if (!ch) break;
        out[i] = ch >= 0x20 && ch < 0x7f ? ch : '?';
    }
    out[i] = 0;
    return out;
}

// Seconds since Link's actor was found (frames at 60 per second): the world clock (frame_step_world_us) does not run
// on 1.1.1 in this branch (frame_rate.c player_pos reads 1.2.1's actor list), so the spike times itself.
static double wsec(void);

static double wsec(void) { return P.player ? P.player_frames / 60.0 : -1.0; }

static uint64_t player_actor(void) {
    uint64_t h = LD64(PLAYER_HOLDER);
    if (!ok_ptr(h, 0x488)) return 0;
    uint64_t a = LD64(h + 0x480);
    return ok_ptr(a, 0x400) ? a : 0;
}

static uint64_t as_control(uint64_t actor) {
    uint64_t p = LD64(actor + A_COMPONENTS);
    if (!ok_ptr(p, 0x48)) return 0;
    p = LD64(p + 0x40);
    if (!ok_ptr(p, 0x20)) return 0;
    p = LD64(p + 0x18);
    if (!ok_ptr(p, 0x20)) return 0;
    p = LD64(p + 0x18);
    return ok_ptr(p, 0x140) ? p : 0;
}

static int as_slots(uint64_t asc) { return asc ? (int)LD32(asc + 0x18) : 0; }

static uint64_t as_slot(uint64_t asc, int slot) {
    if (!asc || slot < 0 || slot >= as_slots(asc)) return 0;
    uint64_t arr = LD64(asc + 0x20);
    if (!ok_ptr(arr + 0x48ull * (unsigned)slot, 8)) return 0;
    uint64_t e = LD64(arr + 0x48ull * (unsigned)slot);
    return ok_ptr(e, 0x110) ? e : 0;
}

static float call_f(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1) {
    rt_call(c, fn, 2, a0, a1);
    float f;
    uint32_t bits = (uint32_t)c->v[0];
    memcpy(&f, &bits, 4);
    return f;
}

static const char *area_now(char *out, size_t cap) {
    out[0] = 0;
    uint64_t mgr = LD64(FLAGS_MGR);
    if (!ok_ptr(mgr, 0xd80)) return out;
    uint64_t cn = mgr + S64_BASE, table = LD64(cn + 0x18), entries = LD64(cn + 0x10);
    uint32_t cap_n = LD32(cn + 0x24), count = LD32(cn + 0x08);
    if (!cap_n || !ok_ptr(table, 8ull * cap_n) || !entries) return out;
    for (uint32_t i = F_CURRENT_AREA % cap_n, k = 0; k < cap_n; k++, i = i + 1 == cap_n ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == F_CURRENT_AREA) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            if (idx >= count) return out;
            uint64_t e = entries + (uint64_t)idx * S64_SIZE;
            if (ok_ptr(e, S64_SIZE)) text_at(LD64(e + S64_TEXT), out, cap);
            return out;
        }
        if (!h) return out;
    }
    return out;
}

// ---- create and release

static int spawn_request(Cpu *c, Spawn *s, const char *name, const float pos[3], const float rot[9]) {
    memset(s, 0, sizeof *s);
    snprintf(s->text, sizeof s->text, "%s", name);
    s->name = (uint64_t)(uintptr_t)s->text;
    memcpy(s->place + 0x00, pos, 12);
    memcpy(s->place + 0x0c, rot, 36);
    const float one[3] = {1, 1, 1};
    memcpy(s->place + 0x30, one, 12);
    uint64_t empty = EMPTY_TEXT;
    memcpy(s->place + 0x40, &empty, 8);
    // +0x48 and +0x98 as the AI's create node sets them for an actor it makes in play (main+0x32bca60..0x32bcaa0):
    // +0x48 a byte 0 (with 1 the asker leaves the actor asleep when it takes it; other callers store 0x10000 or
    // 0x10001) and +0x98 1
    s->place[0x98] = 1;
    uint64_t sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;
    ST64(c->sp + 0x00, 0);
    ST64(c->sp + 0x08, (uint64_t)(uintptr_t)&s->status);
    ST64(c->sp + 0x10, 0);
    uint64_t mgr = LD64(ACTOR_MGR);
    uint64_t r = rt_call(c, CREATE_FN, 8, mgr, (uint64_t)(uintptr_t)&s->name, (uint64_t)(uintptr_t)s->place,
                         (uint64_t)(uintptr_t)&s->handle, 1ull, 0ull, 0ull, 0ull);
    c->sp = sp;
    rt_log("coop: puppet: w=%.2f create %s at %.2f %.2f %.2f: returned %llu, outcome %u, handle %#llx (manager %#llx, thread %u)",
           wsec(), name, pos[0], pos[1], pos[2], (unsigned long long)(r & 1), s->status,
           (unsigned long long)s->handle, (unsigned long long)mgr, plat_thread_id());
    return (int)(r & 1);
}

static void spawn_release(Cpu *c, Spawn *s) {
    if (s->actor) {
        uint64_t a = s->actor;
        uint32_t st = LD32(a + A_STATE);
        uint64_t r = rt_call(c, ACTOR_DELETE, 2, a, 0ull) & 1;
        rt_log("coop: puppet: w=%.2f release %s: delete actor %#llx -> %llu (state %u -> %u)", wsec(), s->text,
               (unsigned long long)a, (unsigned long long)r, st, LD32(a + A_STATE));
        s->actor = 0;
        return;
    }
    if (!s->handle) return;
    uint64_t sys = LD64(ACTOR_SYS);
    s->matcher[0] = MATCH_VTBL;
    s->matcher[1] = (uint64_t)(uintptr_t)&s->handle;
    uint64_t pending = rt_call(c, QUEUE_FIND, 2, LD64(sys + 0x458), (uint64_t)(uintptr_t)s->matcher) & 1;
    uint64_t h = s->handle;
    uint32_t st = LD32(h);
    if (pending) rt_call(c, HANDLE_FREE, 1, h);
    else rt_call(c, HANDLE_DELETE, 2, h, 1ull);
    rt_log("coop: puppet: w=%.2f release %s: %s (request state %#x -> %#x)", wsec(), s->text,
           pending ? "the create was still pending" : "its actor is deleted", st, LD32(h));
    s->handle = 0;
}

enum { SPAWN_FAILED = -1, SPAWN_PENDING = 0, SPAWN_MADE = 1 };

// Follows a create request: once it is ready (state 3) the actor is taken as the game's askers take theirs, which
// puts it in the actor system's list and so into the scene; the request is then forgotten. SPAWN_MADE with s->actor
// set, SPAWN_PENDING, or SPAWN_FAILED
// (given up by the game, or failed; nothing is left to release).
static int spawn_take(Cpu *c, Spawn *s) {
    if (s->actor) return SPAWN_MADE;
    if (!s->handle || !ok_ptr(s->handle, 0x30)) return SPAWN_FAILED;
    uint64_t req = s->handle;
    uint32_t st = LD32(req) & 0xffff;
    if (st == 3) {
        uint64_t made = LD64(req + 8);
        uint32_t sleep_was = ok_ptr(made, 0x400) ? LD32(made + A_SLEEP) : 0;
        uint64_t a = rt_call(c, CREATE_TAKE, 3, req, 1ull, 0ull);
        s->handle = 0;
        s->actor = ok_ptr(a, 0x400) ? a : 0;
        rt_log("coop: puppet: w=%.2f %s taken from request %#llx: actor %#llx, state %u, +0x30 %#x -> %#x", wsec(),
               s->text, (unsigned long long)req, (unsigned long long)a, s->actor ? LD32(a + A_STATE) : 0, sleep_was,
               s->actor ? LD32(a + A_SLEEP) : 0);
        return s->actor ? SPAWN_MADE : SPAWN_FAILED;
    }
    if (st == 4) {
        rt_call(c, CREATE_DROP, 1, req);
        s->handle = 0;
        rt_log("coop: puppet: w=%.2f %s: the game gave the create up (request state 4)", wsec(), s->text);
        return SPAWN_FAILED;
    }
    if (st == 5 || st == 6) {
        rt_log("coop: puppet: w=%.2f %s: the create failed (request state %u)", wsec(), s->text, st);
        spawn_release(c, s);
        return SPAWN_FAILED;
    }
    return SPAWN_PENDING;
}

// Puts an actor at pos turned as rot (row-major) through the engine's setter, and into its own copies.
static void put_actor(Cpu *c, uint64_t a, const float pos[3], const float rot[9]) {
    static float m[12] __attribute__((aligned(16)));
    for (int r = 0; r < 3; r++) {
        memcpy(m + 4 * r, rot + 3 * r, 12);
        m[4 * r + 3] = pos[r];
    }
    rt_call(c, ACTOR_SET_MTX, 3, a, (uint64_t)(uintptr_t)m, 0ull);
    memcpy((void *)(uintptr_t)(a + A_POS), pos, 12);
    memcpy((void *)(uintptr_t)(a + A_ROT), rot, 36);
    memcpy((void *)(uintptr_t)(a + A_POS2), pos, 12);
    memcpy((void *)(uintptr_t)(a + A_ROT2), rot, 36);
}

// ---- the spike (NX2NAVI_COOP_PUPPET_TEST=spike): actors of the game two metres in front of Link, left standing there

static void place_ahead(uint64_t player, float dist, float pos[3], float rot[9]) {
    float m[9];
    memcpy(m, (const void *)(uintptr_t)(player + A_ROT), sizeof m);
    memcpy(pos, (const void *)(uintptr_t)(player + A_POS), 12);
    pos[0] += m[2] * dist, pos[1] += m[5] * dist, pos[2] += m[8] * dist;
    memcpy(rot, m, sizeof m);
}

static void log_as(Cpu *c, const char *who, uint64_t actor, int always, char (*seen)[64]) {
    uint64_t asc = as_control(actor);
    int n = as_slots(asc);
    for (int i = 0; i < n && i < 4; i++) {
        uint64_t e = as_slot(asc, i);
        char name[64];
        text_at(e ? LD64(e + 0x10) : 0, name, sizeof name);
        int changed = strcmp(name, seen[i]) != 0;
        if (!changed && !always) continue;
        snprintf(seen[i], sizeof seen[i], "%s", name);
        float fr = call_f(c, AS_FRAME_GET, asc, (uint64_t)i);
        rt_log("coop: puppet: w=%.2f %s AS %#llx slot %d/%d '%s' frame %.2f (+0xd4 %.3f +0xe8 %.3f +0x10c %.3f)",
               wsec(), who, (unsigned long long)asc, i, n, name, fr, f32_at(e + 0xd4),
               f32_at(e + 0xe8), f32_at(e + 0x10c));
    }
}

// Asks the actor's AS for the node name on slot (main+0x2022a0), as the game's own callers do: rate 1, no start frame,
// blend frames (-1: the node's own), and nothing when the slot plays that node already. 1 when the AS took it.
static int as_play(Cpu *c, uint64_t actor, const char *name, int slot, float blend) {
    static char keep[64];
    static uint8_t req[0x30] __attribute__((aligned(16)));
    snprintf(keep, sizeof keep, "%s", name);
    memset(req, 0, sizeof req);
    uint64_t text = (uint64_t)(uintptr_t)keep;
    float rate = 1, start = -1;
    uint32_t s = (uint32_t)slot;
    memcpy(req + 0x00, &actor, 8);
    memcpy(req + 0x08, &text, 8);
    memcpy(req + 0x10, &s, 4);
    memcpy(req + 0x14, &rate, 4);
    memcpy(req + 0x18, &start, 4);
    memcpy(req + 0x1c, &blend, 4);
    req[0x20] = 1;
    return (int)(rt_call(c, AS_PLAY, 1, (uint64_t)(uintptr_t)req) & 1);
}

// The seconds of the spike: the first actor (a pot) is asked for at 4 s and released at 20 s; the second (the pause
// menu's Link unless named) is asked for at 23 s, asked for two AS nodes at 30 s and released at 45 s. Each stands
// where it was asked for; its place and state are logged every second.
static char spike_names[2][64] = {"Obj_Pot_A_01", "PouchPlayer_Child"};   // spike:<first>:<second> names others
enum { SP_WAIT, SP_ASKED, SP_FOLLOW, SP_GONE };

static void spike_one(Cpu *c, uint64_t player, int k, double w, double ask, double play_at, double end) {
    Spawn *s = &P.spawn[k];
    int *st = &P.sp_state[k];
    uint64_t a = P.actor[k];
    if (*st == SP_WAIT) {
        if (w < ask) return;
        float pos[3], rot[9];
        place_ahead(player, 2.0f, pos, rot);
        spawn_request(c, s, spike_names[k], pos, rot);
        *st = SP_ASKED, P.asked_frame[k] = P.player_frames;
        return;
    }
    uint32_t in = P.player_frames - P.asked_frame[k];
    if (*st == SP_ASKED) {
        int r = spawn_take(c, s);
        if (r == SPAWN_MADE) {
            a = P.actor[k] = s->actor;
            char n1[64];
            rt_log("coop: puppet: w=%.2f %s made after %u frames: actor %#llx, instance name '%s', state %u, "
                   "pos %.2f %.2f %.2f", wsec(), s->text, in, (unsigned long long)a,
                   text_at(LD64(a + 0x20), n1, sizeof n1), LD32(a + A_STATE), f32_at(a + A_POS),
                   f32_at(a + A_POS + 4), f32_at(a + A_POS + 8));
            *st = SP_FOLLOW;
        } else if (r == SPAWN_FAILED || w >= end) {
            rt_log("coop: puppet: w=%.2f %s not made after %u frames", wsec(), s->text, in);
            spawn_release(c, s);
            *st = SP_GONE;
        }
        return;
    }
    if (*st == SP_FOLLOW) {
        if (!ok_ptr(a, 0x400) || (LD32(a + A_STATE) != 4 && LD32(a + A_STATE) != 3)) {
            rt_log("coop: puppet: w=%.2f %s's actor gone (state %u)", wsec(), s->text, ok_ptr(a, 0x400) ? LD32(a + A_STATE) : 0);
            *st = SP_GONE;
            return;
        }
        if (in % 60 == 0) {
            rt_log("coop: puppet: w=%.2f %s: state %u, +0x30 %#x, at %.2f %.2f %.2f; Link at %.2f %.2f %.2f", wsec(),
                   s->text, LD32(a + A_STATE), LD32(a + A_SLEEP), f32_at(a + A_POS), f32_at(a + A_POS + 4),
                   f32_at(a + A_POS + 8), f32_at(player + A_POS), f32_at(player + A_POS + 4), f32_at(player + A_POS + 8));
            if (k) log_as(c, "spike", a, 1, P.menu_anim);
        }
        if (k && !P.played && w >= play_at) {
            // the pause menu Link's AS knows its own nodes, the puppet's Link's: ask one of each
            P.played = 1;
            int r1 = as_play(c, a, "PouchWait", 0, -1.0f);
            int r2 = as_play(c, a, P.anim[0], 0, -1.0f);
            rt_log("coop: puppet: w=%.2f %s: play 'PouchWait' -> %d, play '%s' -> %d", wsec(), s->text, r1, P.anim[0], r2);
        }
        if (w >= end) {
            spawn_release(c, s);
            *st = SP_GONE, P.asked_frame[k] = P.player_frames;
        }
        return;
    }
    if (*st == SP_GONE && a && in < 120 && in % 10 == 0)
        rt_log("coop: puppet: w=%.2f after the release of %s: readable %d, state %u", wsec(), s->text,
               ok_ptr(a, 0x400), ok_ptr(a, 0x400) ? LD32(a + A_STATE) : 0);
}

static void spike(Cpu *c, uint64_t player) {
    double w = wsec();
    if (w < 0) return;
    spike_one(c, player, 0, w, 4, 0, 20);
    if (P.sp_state[0] == SP_GONE) spike_one(c, player, 1, w, 23, 30, 45);
}

// ---- a puppet: an actor this file asked for and poses every frame

typedef struct {
    Spawn s;
    int state;              // SP_WAIT (none yet), SP_ASKED, SP_FOLLOW (made: posed every frame), SP_GONE
    uint64_t actor;
    uint32_t asked_frame;   // P.frames at the request or the release
    char asked[SLOTS][64];  // the node each AS slot was asked for
    int play_ok[SLOTS];
    uint32_t seen_state;    // the actor's state last logged
} Puppet;

static void pose_of(Cpu *c, uint64_t actor, Pose *p) {
    memcpy(p->pos, (const void *)(uintptr_t)(actor + A_POS), 12);
    memcpy(p->rot, (const void *)(uintptr_t)(actor + A_ROT), 36);
    uint64_t asc = as_control(actor);
    int n = as_slots(asc);
    p->nslots = n < SLOTS ? n : SLOTS;
    for (int i = 0; i < p->nslots; i++) {
        uint64_t e = as_slot(asc, i);
        text_at(e ? LD64(e + 0x10) : 0, p->node[i], sizeof p->node[i]);
        p->frame[i] = e ? call_f(c, AS_FRAME_GET, asc, (uint64_t)i) : 0;
    }
}

static void as_frame_set(Cpu *c, uint64_t asc, int slot, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    c->v[0] = bits;
    rt_call(c, AS_FRAME_SET, 2, asc, (uint64_t)slot);
}

static void puppet_ask(Cpu *c, Puppet *p, const char *name, const float pos[3], const float rot[9]) {
    spawn_request(c, &p->s, name, pos, rot);
    p->state = SP_ASKED, p->actor = 0, p->asked_frame = P.frames, p->seen_state = 0;
    memset(p->asked, 0, sizeof p->asked);
}

// 1 while the actor is still the one made for the request (its pack name: a freed actor's memory may hold another)
// and alive (state 3 until the game has woken it, then 4).
static int puppet_alive(const Puppet *p) {
    char n[64];
    uint64_t a = p->actor;
    uint32_t st = a && ok_ptr(a, 0x400) ? LD32(a + A_STATE) : 0;
    return (st == 3 || st == 4) && !strcmp(text_at(LD64(a + A_DEF_NAME), n, sizeof n), p->s.text);
}

// A request until its actor is made and taken (given up after 20 s), then the actor while it lives. 1 when it can be
// posed.
static int puppet_step(Cpu *c, Puppet *p) {
    uint32_t in = P.frames - p->asked_frame;
    if (p->state == SP_ASKED) {
        int r = spawn_take(c, &p->s);
        if (r != SPAWN_MADE) {
            if (r == SPAWN_PENDING && in < 20 * 60) return 0;
            rt_log("coop: puppet: w=%.2f %s not made after %u frames", wsec(), p->s.text, in);
            spawn_release(c, &p->s);
            p->state = SP_GONE, p->asked_frame = P.frames;
            return 0;
        }
        uint64_t a = p->actor = p->s.actor;
        rt_log("coop: puppet: w=%.2f %s made after %u frames: actor %#llx, state %u, AS control %#llx with %d slots",
               wsec(), p->s.text, in, (unsigned long long)a, LD32(a + A_STATE), (unsigned long long)as_control(a),
               as_slots(as_control(a)));
        p->state = SP_FOLLOW;
    }
    if (p->state != SP_FOLLOW) return 0;
    if (!puppet_alive(p)) {   // the game removed it (a new area): it is no longer ours to delete
        rt_log("coop: puppet: w=%.2f %s's actor is gone", wsec(), p->s.text);
        p->state = SP_GONE, p->actor = p->s.actor = 0, p->asked_frame = P.frames;
        return 0;
    }
    return 1;
}

static void puppet_release(Cpu *c, Puppet *p) {
    if (p->state == SP_ASKED || (p->state == SP_FOLLOW && puppet_alive(p))) spawn_release(c, &p->s);
    p->state = SP_GONE, p->actor = p->s.actor = p->s.handle = 0, p->asked_frame = P.frames;
}

// Puts the puppet at pos turned as rot (row-major), and has each of its AS slots play the node anim names at anim's
// frame: asked for when it changes, the frame set every frame.
static void puppet_pose(Cpu *c, Puppet *p, const float pos[3], const float rot[9], const Pose *anim, int verbose) {
    uint64_t a = p->actor;
    uint32_t st = LD32(a + A_STATE);
    if (st != p->seen_state) {
        rt_log("coop: puppet: w=%.2f %s: actor state %u, +0x30 %#x after %u frames", wsec(), p->s.text, st,
               LD32(a + A_SLEEP), P.frames - p->asked_frame);
        p->seen_state = st;
    }
    put_actor(c, a, pos, rot);
    uint64_t asc = as_control(a);
    int n = as_slots(asc) < anim->nslots ? as_slots(asc) : anim->nslots;
    for (int i = 0; i < n; i++) {
        if (!anim->node[i][0]) continue;
        if (strcmp(anim->node[i], p->asked[i])) {
            snprintf(p->asked[i], sizeof p->asked[i], "%s", anim->node[i]);
            p->play_ok[i] = as_play(c, a, anim->node[i], i, -1.0f);
            if (verbose) rt_log("coop: puppet: w=%.2f %s slot %d: play '%s' at frame %.1f -> %d", wsec(), p->s.text, i,
                            anim->node[i], anim->frame[i], p->play_ok[i]);
        }
        if (p->play_ok[i]) as_frame_set(c, asc, i, anim->frame[i]);
    }
}

static void puppet_name(char *out, size_t cap, int adult) { snprintf(out, cap, "CoopPuppet_%s", adult ? "Adult" : "Child"); }

static int is_adult(uint64_t player) {
    char n[64];
    return strstr(text_at(LD64(player + A_DEF_NAME), n, sizeof n), "Adult") != NULL;
}

// ---- the shadow (NX2NAVI_COOP_PUPPET_TEST=shadow[:<actor>[:<side metres>]]): a puppet that is the local Link of one
// second ago, side metres to his left then (so that the two stay apart while he stands): his transform, and on each
// AS slot the node he played and its frame. It is asked for 3 s after Link is found and released at 75 s.

static Puppet shadow_p;

static void shadow(Cpu *c, uint64_t player) {
    double w = wsec();
    Pose *now = &P.ring[P.player_frames % RING];
    pose_of(c, player, now);
    const Pose *then = P.player_frames > DELAY ? &P.ring[(P.player_frames - DELAY) % RING] : now;
    float pos[3], rot[9];
    memcpy(rot, then->rot, sizeof rot);
    for (int k = 0; k < 3; k++) pos[k] = then->pos[k] + rot[k * 3] * P.side;   // the first column: his left
    Puppet *p = &shadow_p;
    if (p->state == SP_WAIT) {
        if (w < 3) return;
        char name[64];
        if (P.shadow_name[0]) snprintf(name, sizeof name, "%s", P.shadow_name);
        else puppet_name(name, sizeof name, is_adult(player));
        puppet_ask(c, p, name, pos, rot);
        return;
    }
    if (!puppet_step(c, p)) {
        uint32_t in = P.frames - p->asked_frame;
        if (p->state == SP_GONE && p->actor && in < 120 && in % 10 == 0)
            rt_log("coop: puppet: w=%.2f after the release of %s: readable %d, state %u", wsec(), p->s.text,
                   ok_ptr(p->actor, 0x400), ok_ptr(p->actor, 0x400) ? LD32(p->actor + A_STATE) : 0);
        return;
    }
    puppet_pose(c, p, pos, rot, then, 1);
    uint64_t a = p->actor;
    if ((P.frames - p->asked_frame) % 60 == 0) {
        rt_log("coop: puppet: w=%.2f %s at %.2f %.2f %.2f (Link %.2f %.2f %.2f, a second ago %.2f %.2f %.2f)", wsec(),
               p->s.text, f32_at(a + A_POS), f32_at(a + A_POS + 4), f32_at(a + A_POS + 8), now->pos[0], now->pos[1],
               now->pos[2], then->pos[0], then->pos[1], then->pos[2]);
        log_as(c, "puppet", a, 1, P.menu_anim);
    }
    if (w >= 75) {
        puppet_release(c, p);
        p->actor = a;   // for the lines after the release
    }
}

// ---- the other players of a co-op room. Each sends its Link's pose (POS, coop_proto.h) about 15 times a second while
// it is in a room with others; a pose received is kept with its arrival time (the coop thread), and every frame the
// game thread shows each other player whose Link is in this Link's area as a puppet at his pose of about 100 ms ago,
// between the two poses received around then (their senders' clocks put on ours by the smallest delay seen). A
// puppet goes when its player leaves the area, the room or the world, or goes silent for 3 s. A pose counts only above
// the last one taken from the same member in the same session (coop_pos_fresh): the session layer can deliver a
// message again (coop.h), and an old pose must not move a puppet back while the player's own keep arriving. After 3 s
// with none taken, the next is taken whatever its counter, so a relay that held a player's datagrams back that long can
// replay the player's recorded poses: the puppet walks an old path, and nothing else (progress, saves) follows from it.

#define HIST 8
#define SILENT_MS 3000
#define BEHIND_MS 100
typedef struct {
    CoopPos pose[HIST];     // the latest poses by pseq, newest at head
    uint64_t recv_ms[HIST];
    int n, head;
    int32_t offset;         // our clock minus the sender's, the smallest seen (rising 1 ms a message so it follows)
    CoopPosSeen seen;       // the sender's counter, per session
} Remote;

static struct {
    PlatMutex lock;         // around r and session
    Remote r[COOP_MAX_MEMBERS + 1];
    uint64_t session[COOP_MAX_MEMBERS + 1];   // each member's session, as the view lists it
    _Atomic int others;     // other members of the room this session is in (the view hook)
    _Atomic int self;
    _Atomic int alive;      // puppets the game thread holds
    // the game thread's
    Puppet puppet[COOP_MAX_MEMBERS + 1];
    uint32_t pseq;
} N;

static void on_pos(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    CoopPos p;
    if (from < 1 || from > COOP_MAX_MEMBERS || !coop_pos_parse(bytes, n, &p)) return;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&N.lock);
    Remote *r = &N.r[from];
    int fresh = coop_pos_fresh(&r->seen, N.session[from], p.pseq, now);
    if (fresh == 2) r->n = 0;   // another session's poses, or ones from before a silence: not this sender's now
    if (fresh) {   // a pose sent again or an older one is dropped
        int32_t off = (int32_t)((uint32_t)now - p.t_ms);
        r->offset = !r->n || off < r->offset ? off : r->offset + 1;
        r->head = (r->head + 1) % HIST;
        r->pose[r->head] = p, r->recv_ms[r->head] = now;
        if (r->n < HIST) r->n++;
    }
    plat_mutex_unlock(&N.lock);
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    int others = v->state == COOP_JOINED && v->n > 1 ? v->n - 1 : 0;
    N.others = others;
    N.self = v->self;
    plat_mutex_lock(&N.lock);
    if (!others) memset(N.r, 0, sizeof N.r);   // a new room starts from nothing
    memset(N.session, 0, sizeof N.session);
    for (int i = 0; i < v->n; i++)
        if (v->member[i].id >= 1 && v->member[i].id <= COOP_MAX_MEMBERS) N.session[v->member[i].id] = v->member[i].session;
    plat_mutex_unlock(&N.lock);
}

static float lerp_angle(float a, float b, float t) {
    float d = fmodf(b - a + 3.14159265f, 6.28318531f);
    if (d < 0) d += 6.28318531f;
    return a + (d - 3.14159265f) * t;
}

// The pose of r shown now: between the two poses around BEHIND_MS ago (sender's clock), else the nearest one.
static void remote_pose(const Remote *r, uint32_t now, CoopPos *out) {
    uint32_t t = now - (uint32_t)r->offset - BEHIND_MS;
    const CoopPos *newest = &r->pose[r->head];
    *out = *newest;
    if ((int32_t)(t - newest->t_ms) >= 0) return;
    for (int k = 0; k + 1 < r->n; k++) {
        const CoopPos *b = &r->pose[(r->head - k + HIST) % HIST], *a = &r->pose[(r->head - k - 1 + HIST) % HIST];
        if ((int32_t)(t - a->t_ms) < 0) {
            *out = *a;   // older than every pose kept: the oldest
            continue;
        }
        float f = b->t_ms == a->t_ms ? 1.0f : (float)(int32_t)(t - a->t_ms) / (float)(int32_t)(b->t_ms - a->t_ms);
        *out = f < 0.5f ? *a : *b;
        for (int i = 0; i < 3; i++) out->pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * f;
        out->yaw = lerp_angle(a->yaw, b->yaw, f);
        for (int i = 0; i < out->nslots && i < a->nslots && i < b->nslots; i++)
            if (!strcmp(a->slot[i].node, b->slot[i].node) && b->slot[i].frame >= a->slot[i].frame)
                out->slot[i].frame = a->slot[i].frame + (b->slot[i].frame - a->slot[i].frame) * f;
        return;
    }
}

static void yaw_rot(float yaw, float rot[9]) {
    float s = sinf(yaw), c = cosf(yaw);
    const float m[9] = {c, 0, s, 0, 1, 0, -s, 0, c};   // a turn about +y: the third column (forward) is (sin, 0, cos)
    memcpy(rot, m, sizeof m);
}

static void send_pose(Cpu *c, uint64_t player, const char *area) {
    CoopPos me;
    memset(&me, 0, sizeof me);
    me.pseq = ++N.pseq, me.t_ms = (uint32_t)plat_now_ms();
    if (player) {
        Pose p;
        pose_of(c, player, &p);
        me.flags = (uint8_t)(COOP_POS_IN_WORLD | (is_adult(player) ? COOP_POS_ADULT : 0));
        snprintf(me.area, sizeof me.area, "%s", area);
        memcpy(me.pos, p.pos, sizeof me.pos);
        me.yaw = atan2f(p.rot[2], p.rot[8]);
        me.nslots = (uint8_t)p.nslots;
        for (int i = 0; i < p.nslots; i++) {
            snprintf(me.slot[i].node, sizeof me.slot[i].node, "%s", p.node[i]);
            me.slot[i].frame = p.frame[i];
        }
    }
    uint8_t buf[COOP_MAX_PAYLOAD];
    size_t n = coop_pos_pack(buf, sizeof buf, &me);
    if (n) coop_send(COOP_K_POS, buf, n);
}

static void remotes(Cpu *c, uint64_t player, const char *area) {
    int others = N.others, self = N.self;
    if (others && P.frames % 4 == 0) send_pose(c, player, area);   // 15 a second
    static Remote r[COOP_MAX_MEMBERS + 1];
    plat_mutex_lock(&N.lock);
    memcpy(r, N.r, sizeof r);
    plat_mutex_unlock(&N.lock);
    uint64_t now = plat_now_ms();
    int alive = 0;
    uint32_t shown = coop_game_shown();   // members accepted and identified (coop_offer_shown)
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        Puppet *p = &N.puppet[id];
        CoopPos pose = {0};
        int trusted = shown >> id & 1;
        int heard = others && id != self && trusted && r[id].n && now - r[id].recv_ms[r[id].head] < SILENT_MS;
        if (heard) remote_pose(&r[id], (uint32_t)now, &pose);
        int want = heard && player && area[0] && (pose.flags & COOP_POS_IN_WORLD) && !strcmp(pose.area, area);
        char name[64] = "";
        if (want) puppet_name(name, sizeof name, pose.flags & COOP_POS_ADULT);
        if ((p->state == SP_ASKED || p->state == SP_FOLLOW) && (!want || strcmp(p->s.text, name))) {
            rt_log("coop: puppet: member %d's puppet goes (%s)", id,
                   !trusted ? "not accepted or not identified" : !heard ? "not heard" : !want ? "not in this area" : "other age");
            puppet_release(c, p);
        }
        if (want && p->state != SP_ASKED && p->state != SP_FOLLOW && (p->state == SP_WAIT || P.frames - p->asked_frame > 120)) {
            float pos[3], rot[9];
            memcpy(pos, pose.pos, sizeof pos);
            yaw_rot(pose.yaw, rot);
            rt_log("coop: puppet: member %d is in %s: asking for %s", id, area, name);
            puppet_ask(c, p, name, pos, rot);
        }
        if (puppet_step(c, p)) {
            float rot[9];
            yaw_rot(pose.yaw, rot);
            Pose anim = {.nslots = pose.nslots};
            for (int i = 0; i < pose.nslots && i < SLOTS; i++) {
                snprintf(anim.node[i], sizeof anim.node[i], "%s", pose.slot[i].node);
                anim.frame[i] = pose.slot[i].frame;
            }
            puppet_pose(c, p, pose.pos, rot, &anim, P.frames % 600 < 4);
            if ((P.frames - p->asked_frame) % 300 == 0)
                rt_log("coop: puppet: member %d's puppet at %.2f %.2f %.2f, slot 0 '%s' %.1f", id, pose.pos[0],
                       pose.pos[1], pose.pos[2], anim.node[0], anim.frame[0]);
        }
        alive += p->state == SP_ASKED || p->state == SP_FOLLOW;
    }
    N.alive = alive;
}

// ---- the hook

static void puppet_frame(Cpu *c) {
    rt_call(c, TIME_STEP, 1, c->x[0]);   // the replaced bl: x0 and s0 are the game's
    P.frames++;
    if (P.frames == 1) rt_log("coop: puppet: the frame hook runs (thread %u)", plat_thread_id());
    uint64_t player = player_actor();
    if (player != P.player) {
        char n[64];
        rt_log("coop: puppet: w=%.2f Link's actor %#llx (%s), state %u", wsec(),
               (unsigned long long)player, player ? text_at(LD64(player + A_DEF_NAME), n, sizeof n) : "-",
               player ? LD32(player + A_STATE) : 0);
        P.player = player, P.player_frames = 0;
    }
    char area[80] = "";
    if (player) {
        P.player_frames++;
        area_now(area, sizeof area);
        if (strcmp(area, P.area)) {
            rt_log("coop: puppet: w=%.2f area '%s'", wsec(), area);
            snprintf(P.area, sizeof P.area, "%s", area);
        }
        if (P.test && P.player_frames % 30 == 0) log_as(c, "Link", player, 0, P.anim);
        if (P.test == 1) spike(c, player);
        if (P.test == 2) shadow(c, player);
    }
    if ((P.remote && N.others) || N.alive) remotes(c, player, area);
}

void coop_puppet_present(void) {
    unsigned v = cfg_version();
    if (v != P.cfg_seen) {
        P.cfg_seen = v;
        const char *t = cfg_get("NX2NAVI_COOP_PUPPET_TEST");
        P.test = !t ? 0 : !strncmp(t, "spike", 5) ? 1 : !strncmp(t, "shadow", 6) ? 2 : 0;
        const char *r = cfg_get("NX2NAVI_COOP_PUPPETS");   // no row in the settings table: "" or unset is off
        P.remote = r && *r && strcmp(r, "0") != 0;
        if (P.test == 1 && t[5] == ':') {
            char a[64] = "", b[64] = "";
            if (sscanf(t + 6, "%63[^:]:%63s", a, b) >= 1 && a[0]) snprintf(spike_names[0], sizeof spike_names[0], "%s", a);
            if (b[0]) snprintf(spike_names[1], sizeof spike_names[1], "%s", b);
        }
        if (P.test == 2) {
            char a[64] = "";
            float side = 1.0f;
            if (t[6] == ':') sscanf(t + 7, "%63[^:]:%f", a, &side);
            snprintf(P.shadow_name, sizeof P.shadow_name, "%s", a);
            P.side = side;
        }
    }
    // on while there are others in the room to show (NX2NAVI_COOP_PUPPETS) or puppets to remove; off (the original
    // call) otherwise
    int want = P.test != 0 || (P.remote && N.others) || N.alive;
    if (want != P.want) {
        P.want = want;
        rt_patch_switch(P.set, want);
        rt_log("coop: puppet: the frame hook %s (present thread %u)", want ? "on" : "off", plat_thread_id());
    }
}

void coop_puppet_init(void) {
    plat_mutex_init(&N.lock);
    if (P.set < 0) return;
    coop_on_message(COOP_K_POS, on_pos, NULL);
    coop_on_view(on_view, NULL);
}

__attribute__((constructor)) static void puppet_register(void) {
    P.set = rt_patch_set("coop_puppet");
    P.cfg_seen = ~0u;
    if (P.set >= 0) rt_register_host(HOST_FRAME, puppet_frame);
}

#else
void coop_puppet_present(void) {}
void coop_puppet_init(void) {}
#endif
