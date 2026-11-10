// Restart in place (game 1.1.1): the menu's Restart brings the player back where they stood, in a new process that
// has the changed settings, without the player pressing anything.
//
// The game's autosave request (found by disassembly of main):
// - The AutoSaveMgr is component 12 of the system at **[main+0x4c7e178] (main+0xba7880 returns it). main+0xba7730
//   (AutoSaveMgr, kind, instance id) runs the check main+0xba72d0 and, when it passes, sets bit 0 of byte 5 of
//   [AutoSaveMgr+0x130], the request the manager's update (main+0x32762b0) carries out. The kind is not stored: it
//   only chooses which tests the check makes (kind 2 alone also sets bit 4 of that byte).
// - The check refuses every kind when bit 2 or bit 3 of the word at AutoSaveMgr+0x158 is set (main+0xba72d0-0xba72dc).
//   Bit 3 is set when the manager starts (main+0x327615c) and by the sequence node at main+0x32e9a60, which sets or
//   clears it from its bool input; the modules that use the node SeqOneShotSetValidAutoSave are BeforeTitle,
//   DestroySceneCommon and Opening: autosaves are off from the title and through a scene's teardown until a module
//   turns them on again. Bit 2 is set when the manager shuts down (main+0x32761fc).
// - It refuses every kind while the game flag 0x7c138e1f is true (main+0xba733c-0xba73d8 end in a refusal whatever
//   the calls between return): the Bool "System_IsSaveTemporaryStorage" (the name's MurmurHash3 matches; not saved),
//   which the Defeat_Ganon event flow sets from the Ganondorf battle through the collapsing castle to the last battle
//   and EventToTitle, PlusMenuToTitle, Retry and AfterLoadSaveData clear.
// - With an instance id it refuses a second request of the same id (the saved UInt64 0x5e776346); this code passes 0.
// - The cooldown, a float at AutoSaveMgr+0x15c, is set to 1800 after the manager saved (main+0x327708c) and counted
//   down by 1 each update (main+0x3276b44-0x3276b5c). Kinds 1 and 3 are refused while it runs (main+0xba73e4). For
//   the others, while the object at [main+0x4e94840] holds something at +0x68 and a name at [+0xc0], every kind is
//   refused while it runs, kind 0 too; otherwise every kind but 0 is (main+0xba742c, the only test of kind 0 in the
//   check). So kind 0 differs from the event node's kind 6 in the cooldown alone.
// - Every kind is refused when the system's component 28 has both bits 0x08 and 0x40 of its halfword at +0x50 set
//   (main+0xba7430-0xba74d8).
// - The check looks at nothing else: not whether the player stands, falls, swims, talks, shops, plays a minigame or
//   fights a boss. The game asks for its autosaves only where its scripts choose to (the event node, the trigger
//   TriggerRequestAutoSave in shop, item, chest and arrival event flows, its own update's kind 10).
//
// The process Restart ends. While the menu is open the guest thread that polls the pad (resume_guest) asks the
// check for kind 6, the event node's own kind, and for kind 0; reads the cooldown; tests the condition the manager's
// own update waits for before its deferred kind-10 request ([main+0x4e94840] present, nothing at +0x68, an empty name
// at [+0xc0]: main+0x327644c-0x3276474); and follows the player's position. Restart then says it saves first only
// when kind 6 passes now, or when kind 0 passes and the cooldown ends within COOL_WAIT_MS (the only difference, read
// above), and only while that condition holds and the player has stood still for STAND_MS: the check itself would
// take a request in a fall or a conversation. Kind 0 itself is never requested. Confirmed, Restart makes the kind-6
// request once the game allows it and starts the new process when the game has written a progress.sav after
// Restart, a save Commit with changes has followed (fs_host.c fs_save_commits) and the save mount has been quiet for
// SETTLE_MS. When the game does not take the request, does not save within SAVE_WAIT_MS (or HARD_WAIT_MS while a
// save is being written), or the wait for the cooldown runs out, nothing restarts: the confirmation says the game
// cannot save there and offers the plain restart to the title. Cancel, and closing the menu by any way (ui/menu.cpp
// change_open: F1, Minus+Plus, Continue, B), cancel the restart until it is under way; a request the game has already
// taken still saves, as any autosave of the game would. Once the restart is under way the menu stays open until the
// window closes, so the process never restarts while the player is back in play.
//
// The process Restart starts. NX2NAVI_RESUME="<slot> <unix ms when Restart was confirmed> <unix ms of the hand-over>
// <old process id> <x> <y> <z>" carries the request over (gfx_present.c gfx_window_closed starts the new process with
// this environment). It is read from the environment alone, never from nx2navi.ini, and removed at this process's
// first look (boot_state), so a later restart starts plainly. It is ignored when the hand-over came more than
// HANDOVER_MAX_MS before this process started or lies more than HANDOVER_SLACK_MS after that start, when it names this
// process, or when that slot's progress.sav or caption.sav is missing or older than the confirmation. Its age counts
// from the hand-over, made once the save is committed, to this process's start: the wait for the cooldown before it
// and the shader preparation before the first look do not count, so no wait the confirmation offers makes it too old.
// Then, until the player stands in the world again, the movies end at their first sample (ov_movie2.c ov_decode), a
// box says the game is restarting (ui/menu.cpp menu_overlay_frame), the player's buttons and sticks do not reach the
// game (input.c input_poll), and A is pressed for PRESS_MS every PERIOD_MS until the game opens a slot's progress.sav
// for reading, for PRESS_MAX_MS at most.
//
// Why the presses land on Continue: the title lays its buttons out by whether a save exists (OpenTitle.module's
// SeqQueryIsExistSave before SeqOneShotSetSLinkPropertyTitleBtnArrangementType), and with one it has Continue first,
// under the cursor; Continue opens the game's load list with the newest save first, and A loads it. The player's own
// input cannot move the cursor off Continue meanwhile, as only the presses reach the game. The presses run only after
// this process found the slot the old one has just saved, so a save exists. New Game asks "Previous
// autosave data will be overwritten. Would you like to proceed?" (ConfirmationNewGame.module, Start / Cancel) and,
// unlike the confirmations that place their cursor (SeqOneShotMoveBoxCursorByButton in ConfirmationLoad and
// ConfirmationBackToTitle), opens it where the window puts it, on its first button, Start [INFERENCE: not seen]: A
// confirms it. If the title ever put New Game first while that slot exists (the game rejecting a save it wrote
// seconds before), the second press would start a new game, which overwrites the autosaves as it saves. Nothing here
// can see the title's cursor; the presses stop at the save's read, after PRESS_MAX_MS, and never start without the
// slot.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "input_map.h"
#include "layout.h"
#include "platform/platform.h"
#include "resume.h"
#include "ui/menu.h"

#define AUTOSAVE_SYSTEM 0x4c7e178u    // GOT slot of the pointer to the system the AutoSaveMgr is a component of
#define AUTOSAVE_COMPONENT 0xba7880u  // (system, index) -> the component, NULL when absent
#define AUTOSAVE_CHECK 0xba72d0u      // (AutoSaveMgr, kind, 0, instance id) -> bool, no side effect with id 0
#define AUTOSAVE_REQUEST 0xba7730u    // (AutoSaveMgr, kind, instance id)
#define AUTOSAVE_INDEX 12
#define AUTOSAVE_KIND 6               // the event node OneShotAutoSaveMgrRequestAutoSave's (main+0xba7704)
#define AUTOSAVE_COOLDOWN 0x15c
#define SCRIPT_STATE 0x4e94840u       // the object whose +0x68 and name the manager's deferred request waits on
#define PROBE_WANT_MS 500             // the menu asked for an answer this recently
#define STAND_MS 1000                 // the player has stood still this long
#define STAND_M 0.02f
#define COOL_WAIT_MS 75000            // the longest wait for the cooldown (1800 updates at 30 a second, and some)
#define SAVE_WAIT_MS 10000
#define HARD_WAIT_MS 30000            // while a save is being written after SAVE_WAIT_MS
#define GUEST_WAIT_MS 5000            // no guest thread polled the pad after Restart
#define NO_PROBE_MS 2000              // the menu has been open this long and no guest thread has polled the pad
#define SETTLE_MS 500
// From the hand-over to the new process's start, which only the old process's end lies between (0.1 s and 5.0 s in two
// runs). The waits before the hand-over (COOL_WAIT_MS, HARD_WAIT_MS) do not count, so the two limits cannot contradict
// each other.
#define HANDOVER_MAX_MS 60000
#define HANDOVER_SLACK_MS 2000        // file times of a coarse file system, and a wall clock that stepped back
#define MOUNT_WAIT_MS 60000
#define PRESS_MS 100
#define PERIOD_MS 300
#define PRESS_MAX_MS 30000
#define LOAD_GIVE_UP_MS 60000

static double uptime_s(void) { return plat_process_uptime_ns() / 1e9; }
static uint64_t unix_ms(void) { return plat_unix_time_ns() / 1000000; }
static uint64_t ms(uint64_t ns) { return ns / 1000000; }

static uint64_t read_u64(uint64_t a) {
    uint64_t v = 0;
    return a && plat_mem_read(a, &v, 8) ? v : 0;
}

// "slot_NN" of a save path "save:/slot_NN/progress.sav" into out; 0 when path is another file
static int progress_slot(const char *path, char *out, size_t cap) {
    const char *name = strrchr(path, '/');
    if (!name || strcmp(name, "/progress.sav")) return 0;
    const char *dir = name;
    while (dir > path && dir[-1] != '/' && dir[-1] != ':') dir--;
    snprintf(out, cap, "%.*s", (int)(name - dir), dir);
    return 1;
}

// ---- the game's answer (guest thread, resume_guest)
static _Atomic uint64_t p_want_ns;          // the menu's last request for an answer
static _Atomic int p_answer = RESUME_CHECKING, p_wait_s;
static _Atomic int p_busy;                  // one guest thread probes at a time
static _Atomic uint64_t p_seen_ns;          // the last probe
static _Atomic uint64_t p_open_ns;          // the menu opened (resume_probe_want)
static uint64_t p_from_ns, p_still_ns, p_cool0_ns;  // probe thread only (p_busy)
static float p_pos[3], p_cool0 = -1;
static char p_last[200];

static uint64_t autosave_mgr(struct Cpu *c) {
    uint64_t system = read_u64(read_u64(MOD_MAIN_BASE + AUTOSAVE_SYSTEM));
    return system ? rt_call(c, MOD_MAIN_BASE + AUTOSAVE_COMPONENT, 2, system, (uint64_t)AUTOSAVE_INDEX) : 0;
}

static int check(struct Cpu *c, uint64_t mgr, int kind) {
    return (int)(rt_call(c, MOD_MAIN_BASE + AUTOSAVE_CHECK, 4, mgr, (uint64_t)kind, (uint64_t)0, (uint64_t)0) & 1);
}

// The condition the manager's own update tests before its deferred request (main+0x327644c-0x3276474).
static int script_quiet(void) {
    uint64_t o = read_u64(MOD_MAIN_BASE + SCRIPT_STATE), name = o ? read_u64(o + 0xc0) : 0;
    char first = 1;
    return o && !read_u64(o + 0x68) && name && plat_mem_read(name, &first, 1) && !first;
}

static void probe(struct Cpu *c) {
    uint64_t now = plat_now_ns(), prev = atomic_exchange(&p_seen_ns, now);
    if (!prev || ms(now - prev) > 2 * PROBE_WANT_MS) p_from_ns = now, p_still_ns = 0, p_cool0 = -1;
    float pos[3];
    int answer, wait_s = 0, in_play = RT_V111 && frame_player_pos(pos);
    if (!in_play || fs_save_import_waiting()) {
        answer = RESUME_PLAIN;
        snprintf(p_last, sizeof p_last, "%s", in_play ? "an imported save waits" : "not in play");
    } else {
        float d = sqrtf((pos[0] - p_pos[0]) * (pos[0] - p_pos[0]) + (pos[1] - p_pos[1]) * (pos[1] - p_pos[1]) +
                        (pos[2] - p_pos[2]) * (pos[2] - p_pos[2]));
        if (!p_still_ns || d > STAND_M) p_still_ns = now, memcpy(p_pos, pos, sizeof p_pos);
        int standing = ms(now - p_still_ns) >= STAND_MS;
        uint64_t mgr = autosave_mgr(c);
        int ok6 = mgr && check(c, mgr, AUTOSAVE_KIND), ok0 = mgr && !ok6 && check(c, mgr, 0), quiet = script_quiet();
        float cool = -1;
        if (mgr && !plat_mem_read(mgr + AUTOSAVE_COOLDOWN, &cool, 4)) cool = -1;
        // the cooldown's pace from what it did since the first reading (the game's updates per second, less while it
        // pauses it); 60 a second until a second has passed
        if (cool <= 0 || cool > p_cool0) p_cool0 = cool, p_cool0_ns = now;
        float span = (float)((now - p_cool0_ns) / 1e9), rate = span >= 1 ? (p_cool0 - cool) / span : 60.0f;
        float secs = cool > 0 ? cool / (rate > 1 ? rate : 1.0f) : 0;
        if (!mgr) answer = RESUME_NO;
        else if (!standing && ms(now - p_from_ns) < STAND_MS) answer = RESUME_CHECKING;
        else if (!standing || !quiet) answer = RESUME_NO;
        else if (ok6) answer = RESUME_CAN;
        else if (ok0 && secs * 1000 <= COOL_WAIT_MS) answer = RESUME_WAIT, wait_s = (int)ceilf(secs);
        else answer = RESUME_NO;
        snprintf(p_last, sizeof p_last,
                 "kind %d %s, kind 0 %s, cooldown %.0f (%.1f s), scripts %s, the player %s %.1f s at %.2f %.2f %.2f",
                 AUTOSAVE_KIND, ok6 ? "passes" : "refused", ok6 ? "not asked" : ok0 ? "passes" : "refused", cool, secs,
                 quiet ? "quiet" : "busy", standing ? "still for" : "moved within", ms(now - p_still_ns) / 1e3, pos[0],
                 pos[1], pos[2]);
    }
    static const char *const names[] = {"restart plainly", "checking", "save and continue",
                                        "wait for the cooldown, save and continue", "cannot save here", "pending"};
    if (answer != atomic_load(&p_answer))
        rt_log("restart: the game's answer: %s (%s)", names[answer], p_last);
    atomic_store(&p_wait_s, wait_s);
    atomic_store(&p_answer, answer);
}

// ---- the process Restart ends
// R_CANCEL: Cancel came while the guest thread was in the request call; only that thread leaves it, to R_OFF. Every
// other change is a compare-and-swap, so a Cancel and the guest's step cannot overwrite each other.
enum { R_OFF, R_ASK, R_CALL, R_CANCEL, R_SAVING, R_GONE };
static _Atomic int r_state;
static _Atomic int r_failed;     // the last attempt did not save: the confirmation says so until the menu closes
static _Atomic int r_wrote = -1; // fs_save_commits when the game opened a progress.sav for writing after Restart
static _Atomic uint64_t r_ask_ns, r_took_ns;
static uint64_t r_ask_unix, r_saved_ns;   // r_ask_unix before the R_ASK store; r_saved_ns present thread only
static float r_pos[3];
static PlatMutex r_lock = PLAT_MUTEX_INIT;
static char r_slot[32];   // the slot whose progress.sav the game opened for writing last (r_lock)

// 1 when it ended the attempt, 0 when the state had moved on (a Cancel came first)
static int fail(int from, const char *why) {
    if (!atomic_compare_exchange_strong(&r_state, &from, R_OFF)) return 0;
    atomic_store(&r_failed, 1);
    rt_log("restart: %s %.0f ms after Restart; nothing restarts, the confirmation offers the restart to the title",
           why, ms(plat_now_ns() - atomic_load(&r_ask_ns)) * 1.0);
    return 1;
}

// The menu has been open NO_PROBE_MS and no guest thread has polled the pad for that long (a long load or a stall, as
// far as known): the answer would stay "checking" with Restart disabled, so the confirmation offers the restart to the
// title instead. A probe that came after the menu opened does not count for ever: a guest that stops polling later
// is the same case.
static int no_probe(void) {
    uint64_t open = atomic_load(&p_open_ns), now = plat_now_ns();
    if (!open || ms(now - open) <= NO_PROBE_MS) return 0;
    return now - NO_PROBE_MS * 1000000ull > atomic_load(&p_seen_ns);   // now is past open by more than that: no wrap
}

int resume_answer(int *wait_s) {
    int s = atomic_load(&r_state);
    *wait_s = 0;
    if (s == R_ASK || s == R_CALL || s == R_SAVING) {
        if (s == R_ASK && atomic_load(&p_answer) == RESUME_WAIT) *wait_s = atomic_load(&p_wait_s);
        return RESUME_PENDING;
    }
    if (s == R_GONE) return RESUME_GONE;
    if (!RT_V111) return RESUME_PLAIN;
    if (s == R_CANCEL) return RESUME_CHECKING;   // the cancelled request call still ends on the guest thread
    int a = atomic_load(&p_answer);
    if (atomic_load(&r_failed) && a != RESUME_PLAIN) return RESUME_NO;
    if (a == RESUME_CHECKING && no_probe()) return RESUME_PLAIN;
    if (a == RESUME_WAIT) *wait_s = atomic_load(&p_wait_s);
    return a;
}

// The menu is open, each frame. After the menu was closed the last answer and the note that the game did not save are
// old: the probe starts again.
void resume_probe_want(void) {
    if (!RT_V111) return;
    uint64_t now = plat_now_ns(), prev = atomic_exchange(&p_want_ns, now);
    if (prev && ms(now - prev) <= PROBE_WANT_MS) return;
    atomic_store(&r_failed, 0);
    atomic_store(&p_answer, RESUME_CHECKING);
    atomic_store(&p_open_ns, now);   // the probes this opening lets through read the clock after this
}

// The menu's Restart (and its test hook) on the present path, one call at a time. Nothing else leaves R_OFF.
void resume_restart(void) {
    if (atomic_load(&r_state) != R_OFF) return;   // already on its way, or a cancelled request call still ends
    int wait_s, a = resume_answer(&wait_s);
    if (a != RESUME_CAN && a != RESUME_WAIT) {
        rt_log("restart: %s; the game starts again at its title",
               !RT_V111 ? "restart in place needs game 1.1.1"
               : a == RESUME_PLAIN && no_probe() ? "no guest thread has polled the pad for 2 s"
               : a == RESUME_PLAIN ? "not in play, or an imported save waits to be loaded"
                                   : "the game cannot save here");
        atomic_store(&r_state, R_GONE);
        gfx_window_request_restart();
        return;
    }
    if (!frame_player_pos(r_pos)) memset(r_pos, 0, sizeof r_pos);
    r_ask_unix = unix_ms();
    r_saved_ns = 0;
    atomic_store(&r_wrote, -1);
    atomic_store(&r_ask_ns, plat_now_ns());
    atomic_store(&r_state, R_ASK);
    rt_log("restart: Restart confirmed at +%.2f s with the player at %.2f %.2f %.2f; %s", uptime_s(), r_pos[0],
           r_pos[1], r_pos[2], a == RESUME_CAN ? "asking the game for its autosave" : "waiting for the game's cooldown");
}

// Any thread. 1 when nothing restarts now; 0 when the restart is under way (R_GONE).
static int cancel(const char *why) {
    for (;;) {
        int s = atomic_load(&r_state);
        if (s == R_GONE) return 0;
        if (s != R_ASK && s != R_CALL && s != R_SAVING) return 1;
        int to = s == R_CALL ? R_CANCEL : R_OFF;   // R_CALL: resume_guest ends it after its call
        if (!atomic_compare_exchange_strong(&r_state, &s, to)) continue;   // the state moved meanwhile: look again
        rt_log("restart: cancelled (%s) %.0f ms after Restart%s", why,
               ms(plat_now_ns() - atomic_load(&r_ask_ns)) * 1.0,
               s == R_CALL     ? " during the game's autosave request"
               : s == R_SAVING ? "; the autosave the game took goes on as any of its autosaves"
                               : "");
        return 1;
    }
}

void resume_cancel(void) { cancel("Cancel"); }

int resume_menu_close(void) { return cancel("the menu closed"); }

void resume_guest(struct Cpu *c) {
    int s = atomic_load_explicit(&r_state, memory_order_relaxed);
    uint64_t want = atomic_load_explicit(&p_want_ns, memory_order_relaxed);
    if (s != R_ASK && (!want || ms(plat_now_ns() - want) > PROBE_WANT_MS)) return;
    int idle = 0;
    if (!atomic_compare_exchange_strong(&p_busy, &idle, 1)) return;
    probe(c);
    s = atomic_load(&r_state);
    int a = atomic_load(&p_answer), ask = R_ASK;
    if (s == R_ASK && a == RESUME_CAN && atomic_compare_exchange_strong(&r_state, &ask, R_CALL)) {
        uint64_t mgr = autosave_mgr(c), req = mgr ? read_u64(mgr + 0x130) : 0;
        uint8_t bits = 0;
        if (req) rt_call(c, MOD_MAIN_BASE + AUTOSAVE_REQUEST, 3, mgr, (uint64_t)AUTOSAVE_KIND, (uint64_t)0);
        int took = req && plat_mem_read(req + 5, &bits, 1) && (bits & 1);
        rt_log("restart: the game %s the autosave request (kind %d) on thread %llu, %.0f ms after Restart",
               took ? "took" : "refused", AUTOSAVE_KIND, (unsigned long long)plat_thread_id(),
               ms(plat_now_ns() - atomic_load(&r_ask_ns)) * 1.0);
        if (took) atomic_store(&r_took_ns, plat_now_ns());   // before the state that publishes it
        // R_CALL is left by compare-and-swap only: a Cancel during the call moved it to R_CANCEL, and then the attempt
        // ends here
        int call = R_CALL;
        if (!(took ? atomic_compare_exchange_strong(&r_state, &call, R_SAVING)
                   : fail(R_CALL, "the game refused its autosave request"))) {
            atomic_store(&r_state, R_OFF);   // from R_CANCEL, which nothing else leaves
            rt_log("restart: the cancelled restart ends after the request call%s",
                   took ? "; the game's autosave goes on" : "");
        }
    } else if (s == R_ASK && a != RESUME_WAIT && a != RESUME_CHECKING) {
        fail(R_ASK, "the game cannot save here now");
    } else if (s == R_ASK && ms(plat_now_ns() - atomic_load(&r_ask_ns)) > COOL_WAIT_MS) {
        fail(R_ASK, "the game's cooldown did not end in time");
    }
    atomic_store(&p_busy, 0);
}

// Hands NX2NAVI_RESUME to the new process and closes the window: gfx_present.c gfx_window_closed then starts it with
// this environment.
static void restart_now(void) {
    char slot[32];
    plat_mutex_lock(&r_lock);
    snprintf(slot, sizeof slot, "%s", r_slot);
    plat_mutex_unlock(&r_lock);
    char v[160];
    // The hand-over's own time: the new process measures the hand-over's age from it, the file times from r_ask_unix
    snprintf(v, sizeof v, "%s %llu %llu %u %.3f %.3f %.3f", slot, (unsigned long long)r_ask_unix,
             (unsigned long long)unix_ms(), plat_process_id(), r_pos[0], r_pos[1], r_pos[2]);
    plat_setenv("NX2NAVI_RESUME", v);
    rt_log("restart: the game saved %s %.0f ms after Restart; restarting to continue there", slot,
           ms(plat_now_ns() - atomic_load(&r_ask_ns)) * 1.0);
    gfx_window_request_restart();
}

// R_ASK and R_SAVING, each present: the new process starts once the game wrote a progress.sav after Restart, a Commit
// with changes followed and the save mount has been quiet for SETTLE_MS (the game writes several files of a save
// before it commits).
static void restart_tick(void) {
    int s = atomic_load_explicit(&r_state, memory_order_relaxed);
    if (s != R_ASK && s != R_SAVING) return;
    uint64_t now = plat_now_ns(), since = ms(now - atomic_load(&r_ask_ns));
    if (s == R_ASK) {   // resume_guest decides; this only notices that no guest thread probes any more
        if (since > GUEST_WAIT_MS && ms(now - atomic_load(&p_seen_ns)) > GUEST_WAIT_MS)
            fail(R_ASK, "no guest thread made the autosave request");
        return;
    }
    int wrote = atomic_load(&r_wrote);
    if (wrote >= 0 && fs_save_commits() > wrote && !fs_save_busy()) {
        if (!r_saved_ns) r_saved_ns = now;
        if (ms(now - r_saved_ns) >= SETTLE_MS && atomic_compare_exchange_strong(&r_state, &s, R_GONE)) restart_now();
        return;
    }
    r_saved_ns = 0;
    uint64_t took = ms(now - atomic_load(&r_took_ns));   // the limits count from the request the game took
    if (took > HARD_WAIT_MS) fail(R_SAVING, "the game's save did not end");
    else if (took > SAVE_WAIT_MS && !fs_save_busy()) fail(R_SAVING, "the game took the request but did not save");
}

// ---- the process Restart starts
enum { B_OFF, B_CHECK, B_PRESS, B_LOADING, B_DONE };
static _Atomic int b_state = B_OFF;
static _Atomic uint64_t b_press0_ns, b_load_ns;
static uint64_t b_ask_unix, b_start_ns;   // set inside plat_once
static float b_pos[3];
static char b_slot[32];
static _Atomic int b_presses;
static double b_load_at;

static void boot_init(void *arg) {
    (void)arg;
    char v[160];
    if (!plat_getenv("NX2NAVI_RESUME", v, sizeof v)) return;
    plat_setenv("NX2NAVI_RESUME", NULL);   // a later restart from this process starts plainly
    unsigned long long ask = 0, gave = 0;
    unsigned pid = 0;
    // This process's start on the wall clock: the hand-over's age is what lay between the hand-over and this start,
    // the old process's end. The shader preparation before this first look does not count.
    uint64_t now = unix_ms(), up = ms(plat_process_uptime_ns()), start = now > up ? now - up : 0;
    char why[96] = "";
    if (sscanf(v, "%31s %llu %llu %u %f %f %f", b_slot, &ask, &gave, &pid, &b_pos[0], &b_pos[1], &b_pos[2]) != 7 ||
        strncmp(b_slot, "slot_", 5) || strpbrk(b_slot, "/\\:."))
        snprintf(why, sizeof why, "is not a hand-over");
    else if (!RT_V111) snprintf(why, sizeof why, "needs game 1.1.1");
    else if (gave > start + HANDOVER_SLACK_MS)
        snprintf(why, sizeof why, "lies %.1f s after this process started", (double)(gave - start) / 1000.0);
    else if (start > gave && start - gave > HANDOVER_MAX_MS)
        snprintf(why, sizeof why, "came %.1f s before this process started", (double)(start - gave) / 1000.0);
    else if (pid == plat_process_id()) snprintf(why, sizeof why, "names this process");
    if (*why) {
        rt_log("restart: NX2NAVI_RESUME '%s' %s; ignored, the game starts at its title", v, why);
        return;
    }
    b_ask_unix = ask;
    b_start_ns = plat_now_ns();
    atomic_store(&b_state, B_CHECK);
    rt_log("restart: hand-over from process %u %.1f s before this process started, Restart confirmed %.1f s before "
           "the hand-over (%s, the player at %.2f %.2f %.2f); checking the save",
           pid, ((double)start - (double)gave) / 1000.0, ((double)gave - (double)ask) / 1000.0, b_slot, b_pos[0],
           b_pos[1], b_pos[2]);
}

// B_CHECK: the presses start only when the slot the old process saved is there, written after Restart.
static int boot_check(void) {
    char p[1200], c[1200], rel[64];
    snprintf(rel, sizeof rel, "%s/progress.sav", b_slot);
    int mounted = fs_save_path(rel, p, sizeof p);
    snprintf(rel, sizeof rel, "%s/caption.sav", b_slot);
    mounted = mounted && fs_save_path(rel, c, sizeof c);
    int s = B_CHECK;
    if (!mounted) {
        if (ms(plat_now_ns() - b_start_ns) > MOUNT_WAIT_MS && atomic_compare_exchange_strong(&b_state, &s, B_OFF))
            rt_log("restart: the game mounted no save; the hand-over is ignored");
        return atomic_load(&b_state);
    }
    int64_t tp = plat_file_mtime_ms(p), tc = plat_file_mtime_ms(c), oldest = tp < tc ? tp : tc;
    int ok = tp >= 0 && tc >= 0 && oldest + HANDOVER_SLACK_MS >= (int64_t)b_ask_unix;
    if (atomic_compare_exchange_strong(&b_state, &s, ok ? B_PRESS : B_OFF)) {
        if (ok) rt_log("restart: %s was saved %.1f s after Restart; the movies end at once and A is pressed until the "
                       "game opens it", b_slot, (double)(oldest - (int64_t)b_ask_unix) / 1000.0);
        else rt_log("restart: %s %s; the hand-over is ignored, the game starts at its title", b_slot,
                    tp < 0 || tc < 0 ? "has no progress.sav or caption.sav" : "was saved before Restart");
    }
    return atomic_load(&b_state);
}

static int boot_state(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, boot_init, NULL);
    int s = atomic_load_explicit(&b_state, memory_order_acquire);
    return s == B_CHECK ? boot_check() : s;
}

int resume_booting(void) {
    int s = boot_state();
    return s == B_PRESS || s == B_LOADING;
}

// Any thread that polls the pad: a press follows from the time since the first one, so callers share no state.
uint64_t resume_buttons(void) {
    if (boot_state() != B_PRESS) return 0;
    uint64_t now = plat_now_ns(), p0 = 0;
    if (atomic_compare_exchange_strong(&b_press0_ns, &p0, now)) {
        rt_log("restart: pressing A from +%.2f s", uptime_s());
        p0 = now;
    }
    if (ms(now - p0) > PRESS_MAX_MS) {
        int s = B_PRESS;
        if (atomic_compare_exchange_strong(&b_state, &s, B_DONE))
            rt_log("restart: no save opened %d s after the first press; the presses stop and the game plays on as after "
                   "a plain restart", PRESS_MAX_MS / 1000);
        return 0;
    }
    return ms(now - p0) % PERIOD_MS < PRESS_MS ? 1ull << B_A : 0;
}

void resume_save_file(const char *path, int write) {
    char slot[32];
    if (!path || !progress_slot(path, slot, sizeof slot)) return;
    if (write) {
        plat_mutex_lock(&r_lock);
        snprintf(r_slot, sizeof r_slot, "%s", slot);
        plat_mutex_unlock(&r_lock);
        int s = atomic_load_explicit(&r_state, memory_order_relaxed);
        if (s != R_ASK && s != R_CALL && s != R_SAVING) return;
        atomic_store(&r_wrote, fs_save_commits());
        rt_log("restart: the game writes %s %.0f ms after Restart", path,
               ms(plat_now_ns() - atomic_load(&r_ask_ns)) * 1.0);
        return;
    }
    if (boot_state() != B_PRESS) return;
    uint64_t now = plat_now_ns(), p0 = atomic_load(&b_press0_ns);
    atomic_store(&b_load_ns, now);   // before the state that publishes it
    b_load_at = uptime_s();
    atomic_store(&b_presses, p0 ? (int)(ms(now - p0) / PERIOD_MS) + 1 : 0);
    int s = B_PRESS;
    if (!atomic_compare_exchange_strong(&b_state, &s, B_LOADING)) return;
    rt_log("restart: the game loads %s at +%.2f s after %d A presses; the presses stop%s", path, b_load_at,
           atomic_load(&b_presses), strcmp(slot, b_slot) ? "; not the slot the old process saved" : "");
}

static void boot_tick(void) {
    int s = boot_state();
    if (s != B_PRESS && s != B_LOADING) return;
    float p[3];
    if (frame_player_pos(p)) {
        if (!atomic_compare_exchange_strong(&b_state, &s, B_DONE)) return;
        double d = sqrt((p[0] - b_pos[0]) * (p[0] - b_pos[0]) + (p[1] - b_pos[1]) * (p[1] - b_pos[1]) +
                        (p[2] - b_pos[2]) * (p[2] - b_pos[2])),
               up = uptime_s(), since = ((double)unix_ms() - (double)b_ask_unix) / 1000.0;
        rt_log("restart: back in play %.2f s after Restart was confirmed (this process started %.2f s after it, the "
               "save opened at +%.2f s, the player stands at +%.2f s); the player at %.2f %.2f %.2f, %.2f m from where "
               "Restart was confirmed",
               since, since - up, b_load_at, up, p[0], p[1], p[2], d);
        return;
    }
    if (s == B_LOADING && ms(plat_now_ns() - atomic_load(&b_load_ns)) > LOAD_GIVE_UP_MS &&
        atomic_compare_exchange_strong(&b_state, &s, B_DONE))
        rt_log("restart: no player %d s after the save opened; the box closes", LOAD_GIVE_UP_MS / 1000);
}

void resume_present(void) {
    restart_tick();
    boot_tick();
}
