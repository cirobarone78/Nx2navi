// runtime/resume.c, the menu's restart in place, with the game and the rest of the runtime stubbed: the clocks, the
// game's autosave check and request, the save files and the environment are the program's own. Built and run by
// tests/resume_check.py, one process per check, as resume.c reads the hand-over once per process. Prints each failed
// check and exits 1 on any; NX2NAVI_RESUME_CHECK_LOG=1 prints resume.c's log lines too.
//
// To force the two orders in which a Cancel met the guest thread's request, every store, exchange and
// compare-and-swap resume.c makes on its restart state first passes hook(), which can run the other side's step there.
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hook(const volatile void *p);
#undef atomic_store
#undef atomic_exchange
#undef atomic_compare_exchange_strong
#define atomic_store(P, V) (hook((const volatile void *)(P)), atomic_store_explicit(P, V, memory_order_seq_cst))
#define atomic_exchange(P, V) (hook((const volatile void *)(P)), atomic_exchange_explicit(P, V, memory_order_seq_cst))
#define atomic_compare_exchange_strong(P, E, D)                                                                       \
    (hook((const volatile void *)(P)),                                                                                \
     atomic_compare_exchange_strong_explicit(P, E, D, memory_order_seq_cst, memory_order_seq_cst))

#include "resume.c"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// ---- the clocks: monotonic, wall, and the time since this process started
static uint64_t now_ns = 100000000000ull, unix_ns = 1800000000000000000ull, up_ns = 60000000000ull;
static uint32_t pid = 100;
static int wall_stopped;   // the wall clock stands while the others run
static void advance_ms(uint64_t m) {
    now_ns += m * 1000000, up_ns += m * 1000000;
    if (!wall_stopped) unix_ns += m * 1000000;
}
uint64_t plat_now_ns(void) { return now_ns; }
uint64_t plat_unix_time_ns(void) { return unix_ns; }
uint64_t plat_process_uptime_ns(void) { return up_ns; }
uint32_t plat_process_id(void) { return pid; }
uint32_t plat_thread_id(void) { return 1; }

// ---- the runtime resume.c reaches
const int rt_game_version = 111;
static int show_log;
void rt_log(const char *fmt, ...) {
    if (!show_log) return;
    va_list a;
    va_start(a, fmt);
    printf("  log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
// resume.c reads the hand-over once per process, at its first look. The old process's look came before any hand-over
// and found none, so here it is left out: the look runs once the program plays the new process (pid 200).
void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg) {
    if (pid == 200 && !o->state) o->state = 1, fn(arg);
}
static char env_resume[256];
static int env_set;
int plat_setenv(const char *name, const char *value) {
    if (strcmp(name, "NX2NAVI_RESUME")) return 0;
    env_set = value != NULL;
    snprintf(env_resume, sizeof env_resume, "%s", value ? value : "");
    return 1;
}
size_t plat_getenv(const char *name, char *out, size_t cap) {
    if (strcmp(name, "NX2NAVI_RESUME") || !env_set) return 0;
    snprintf(out, cap, "%s", env_resume);
    return strlen(out);
}
static int restarts, commits;
static int64_t saved_unix_ms = -1;   // the slot's progress.sav and caption.sav were last written then
void gfx_window_request_restart(void) { restarts++; }
int fs_save_commits(void) { return commits; }
int fs_save_busy(void) { return 0; }
int fs_save_import_waiting(void) { return 0; }
int fs_save_path(const char *rel, char *out, size_t cap) { return snprintf(out, cap, "/save/%s", rel) < (int)cap; }
int64_t plat_file_mtime_ms(const char *path) { return strstr(path, "/slot_00/") ? saved_unix_ms : -1; }
int frame_player_pos(float xyz[3]) {
    xyz[0] = 12.5f, xyz[1] = 3, xyz[2] = -40;
    return 1;
}

// ---- the game: its autosave manager and the check and request resume.c calls
#define SYSTEM_SLOT 0x10000ull
#define SYSTEM 0x20000ull
#define MGR 0x30000ull
#define REQ 0x40000ull
#define SCRIPTS 0x50000ull
#define SCRIPT_NAME 0x60000ull
static int ok6, ok0;          // what the game's check answers for kind 6 and kind 0
static float cooldown;        // AutoSaveMgr+0x15c
static uint8_t req_bits;      // byte 5 of the request: bit 0 once the game took it
static void (*on_request)(void);
int plat_mem_read(uint64_t a, void *out, size_t n) {
    uint64_t v = 0;
    if (a == MOD_MAIN_BASE + AUTOSAVE_SYSTEM) v = SYSTEM_SLOT;
    else if (a == SYSTEM_SLOT) v = SYSTEM;
    else if (a == MGR + 0x130) v = REQ;
    else if (a == MGR + AUTOSAVE_COOLDOWN) return memcpy(out, &cooldown, n), 1;
    else if (a == REQ + 5) v = req_bits;
    else if (a == MOD_MAIN_BASE + SCRIPT_STATE) v = SCRIPTS;
    else if (a == SCRIPTS + 0xc0) v = SCRIPT_NAME;
    else if (a == SCRIPTS + 0x68 || a == SCRIPT_NAME) v = 0;
    else return 0;
    memcpy(out, &v, n);
    return 1;
}
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    (void)c;
    va_list a;
    va_start(a, n);
    uint64_t arg[4] = {0};
    for (int i = 0; i < n && i < 4; i++) arg[i] = va_arg(a, uint64_t);
    va_end(a);
    if (fn == MOD_MAIN_BASE + AUTOSAVE_COMPONENT) return arg[0] == SYSTEM && arg[1] == AUTOSAVE_INDEX ? MGR : 0;
    if (fn == MOD_MAIN_BASE + AUTOSAVE_CHECK) return arg[1] == AUTOSAVE_KIND ? ok6 : arg[1] == 0 ? ok0 : 0;
    if (fn == MOD_MAIN_BASE + AUTOSAVE_REQUEST) {
        if (ok6) req_bits |= 1;
        if (on_request) on_request();
    }
    return 0;
}

// ---- the interleaving: hook_fn runs once, at the next write to the restart state on the thread that armed it
static void (*hook_fn)(void);
static pthread_t hook_thread;
static void arm(void (*fn)(void)) { hook_thread = pthread_self(), hook_fn = fn; }
static void hook(const volatile void *p) {
    if (p != (const volatile void *)&r_state || !hook_fn || !pthread_equal(pthread_self(), hook_thread)) return;
    void (*fn)(void) = hook_fn;
    hook_fn = NULL;
    fn();
}

// ---- one frame of the old process with the menu open: the menu, the guest thread that polls the pad, the present
static void frame(uint64_t step_ms) {
    advance_ms(step_ms);
    resume_probe_want();
    resume_guest(NULL);
    resume_present();
}

// The player stands still with the menu open until the confirmation has the game's answer
static int answer_after_standing(void) {
    for (int i = 0; i < 12; i++) frame(100);
    int wait_s;
    return resume_answer(&wait_s);
}

// The game writes the slot and commits it; then the present path waits SETTLE_MS before it hands over
static void game_saves(void) {
    saved_unix_ms = (int64_t)(unix_ns / 1000000);
    resume_save_file("save:/slot_00/progress.sav", 1);
    commits++;
    for (int i = 0; i < 8; i++) frame(100);
}

// The new process: its id, how long after the hand-over it started, and how long it has run at its first look
static int new_process(int64_t started_after_ms, uint64_t running_ms) {
    pid = 200;
    unix_ns += (uint64_t)((started_after_ms + (int64_t)running_ms) * 1000000);
    up_ns = running_ms * 1000000;
    now_ns += running_ms * 1000000;
    return resume_booting();
}

// ---- the checks
// Restart while the game's cooldown runs for 70 s more (the box offers to wait up to COOL_WAIT_MS): the game saves at
// the end of the wait; the old process takes 5 s to end and the new one 13 s to its first look. The resume holds.
static void check_wait70(void) {
    ok6 = 0, ok0 = 1, cooldown = 2136;   // 30 a second
    int a = 0;
    for (int i = 0; i < 12; i++) cooldown -= 3, frame(100), a = resume_answer(&(int){0});
    CHECK(a == RESUME_WAIT, "the confirmation offers to wait for the cooldown (answer %d)", a);
    resume_restart();
    for (int s = 0; s < 70 && cooldown > 0; s++) {
        for (int i = 0; i < 10; i++) cooldown -= 3, frame(100);
    }
    cooldown = 0, ok6 = 1;
    frame(100);
    CHECK(atomic_load(&r_state) == R_SAVING, "the game took the request when its cooldown ended (state %d)",
          atomic_load(&r_state));
    game_saves();
    CHECK(restarts == 1 && env_set, "the old process handed over and restarted (%d restarts)", restarts);
    int booting = new_process(5000, 13000);
    CHECK(booting, "the new process takes the hand-over of a restart after a 70 s wait for the cooldown");
}

// The same hand-over when the new process starts 70 s after it: ignored.
static void check_stale(void) {
    ok6 = 1, ok0 = 0, cooldown = 0;
    CHECK(answer_after_standing() == RESUME_CAN, "the game takes its autosave here");
    resume_restart();
    frame(16);
    game_saves();
    CHECK(restarts == 1, "the old process restarted");
    CHECK(!new_process(70000, 13000), "a hand-over made 70 s before the new process started is ignored");
}

// The wall clock stepped back between the two processes: the hand-over lies 1 s after the new process's start and
// after its first look. Within HANDOVER_SLACK_MS it holds.
static void check_future1(void) {
    ok6 = 1, ok0 = 0, cooldown = 0;
    CHECK(answer_after_standing() == RESUME_CAN, "the game takes its autosave here");
    uint64_t wall = unix_ns;
    wall_stopped = 1;   // the confirmation, the save and the hand-over at the same wall time
    resume_restart();
    frame(16);
    game_saves();
    CHECK(restarts == 1, "the old process restarted");
    pid = 200, up_ns = 0, unix_ns = wall - 1000000000ull;   // the first look at the start, 1 s before the hand-over
    CHECK(resume_booting(), "a hand-over 1 s in the future is taken");
}

// Race (a): Cancel reads R_ASK, the guest thread's change to R_CALL lands before Cancel's own change, and the guest is
// in the request call meanwhile. Two threads, each stopped at the step where the other runs.
static sem_t guest_go, cancel_go, guest_on;
static void cancel_waits(void) { sem_post(&guest_go), sem_wait(&cancel_go); }
static void guest_in_call(void) { sem_post(&cancel_go), sem_wait(&guest_on); }
static void *cancel_thread(void *arg) {
    (void)arg;
    arm(cancel_waits);   // at Cancel's first change of the state, after it read R_ASK
    resume_cancel();
    return NULL;
}
static void *guest_thread(void *arg) {
    (void)arg;
    sem_wait(&guest_go);
    on_request = guest_in_call;
    resume_guest(NULL);
    on_request = NULL;
    return NULL;
}
static void check_race_a(void) {
    ok6 = 1, ok0 = 0, cooldown = 0;
    CHECK(answer_after_standing() == RESUME_CAN, "the game takes its autosave here");
    resume_restart();
    sem_init(&guest_go, 0, 0), sem_init(&cancel_go, 0, 0), sem_init(&guest_on, 0, 0);
    advance_ms(16);
    pthread_t c, g;
    pthread_create(&g, NULL, guest_thread, NULL);
    pthread_create(&c, NULL, cancel_thread, NULL);
    pthread_join(c, NULL);   // Cancel has returned while the guest is still in the request call
    sem_post(&guest_on);
    pthread_join(g, NULL);
    game_saves();   // the request the game took still saves
    CHECK(restarts == 0 && atomic_load(&r_state) == R_OFF,
          "Cancel that met the guest thread's change to R_CALL cancels the restart (%d restarts, state %d)", restarts,
          atomic_load(&r_state));
}

// Race (b): the guest thread is back from the request call and has found no Cancel; Cancel then comes before the
// guest's change to R_SAVING.
static void cancel_now(void) { resume_cancel(); }
static void arm_cancel(void) { arm(cancel_now); }
static void check_race_b(void) {
    ok6 = 1, ok0 = 0, cooldown = 0;
    CHECK(answer_after_standing() == RESUME_CAN, "the game takes its autosave here");
    resume_restart();
    on_request = arm_cancel;   // at the guest's first change of the state after the call
    frame(16);
    on_request = NULL;
    game_saves();
    CHECK(restarts == 0 && atomic_load(&r_state) == R_OFF,
          "Cancel after the request call and before the guest's change to R_SAVING cancels the restart (%d restarts, "
          "state %d)", restarts, atomic_load(&r_state));
}

// No guest thread polls the pad while the menu is open: after a second the answer still is "checking", after about
// two the confirmation offers the restart to the title.
static void check_no_guest(void) {
    int wait_s;
    for (int i = 0; i < 10; i++) advance_ms(100), resume_probe_want();
    CHECK(resume_answer(&wait_s) == RESUME_CHECKING, "after 1 s without a guest thread the answer is still checking");
    for (int i = 0; i < 15; i++) advance_ms(100), resume_probe_want();
    int a = resume_answer(&wait_s);
    CHECK(a == RESUME_PLAIN, "after 2.5 s without a guest thread Restart is the plain restart (answer %d, 1 = checking "
                             "with Restart disabled)", a);
}

// A guest thread that polls for half a second after the menu opened (the answer is still "checking": the player has
// not stood for a second yet) and then stops: the confirmation must not keep Restart disabled either.
static void check_guest_stops(void) {
    int wait_s;
    for (int i = 0; i < 5; i++) frame(100);
    CHECK(resume_answer(&wait_s) == RESUME_CHECKING, "after 0.5 s of polling the answer is still checking");
    for (int i = 0; i < 50; i++) advance_ms(100), resume_probe_want();
    int a = resume_answer(&wait_s);
    CHECK(a == RESUME_PLAIN, "5 s after the guest stopped polling Restart is the plain restart (answer %d, 1 = "
                             "checking with Restart disabled)", a);
}

static const struct { const char *name; void (*run)(void); } checks[] = {
    {"wait70", check_wait70}, {"stale", check_stale}, {"future1", check_future1},
    {"race_a", check_race_a}, {"race_b", check_race_b}, {"no_guest", check_no_guest},
    {"guest_stops", check_guest_stops},
};

int main(int argc, char **argv) {
    const char *log = getenv("NX2NAVI_RESUME_CHECK_LOG");
    show_log = log && *log == '1';
    if (argc < 2) {
        for (size_t i = 0; i < sizeof checks / sizeof *checks; i++) printf("%s\n", checks[i].name);
        return 0;
    }
    for (size_t i = 0; i < sizeof checks / sizeof *checks; i++)
        if (!strcmp(argv[1], checks[i].name)) {
            checks[i].run();
            return fails != 0;
        }
    printf("unknown check %s\n", argv[1]);
    return 2;
}
