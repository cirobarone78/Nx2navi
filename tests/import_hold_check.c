// runtime/import_hold.c: the bookkeeping of the import hold (vk_memory.c "import hold"). memory.c calls it under its
// frz_lock and hands it the device's queue lock; here the same two locks guard a model of guest memory, a state per
// page (committed or given back) and a word per page (what the game wrote), with no Vulkan and no game. Built and run by
// tests/import_hold_check.py (also under -fsanitize=thread and -fsanitize=address,undefined); prints each failed check
// and exits 1 on any.
//
// Part one, one thread: a decommit runs at once where no registered range lies and is queued where one does, also at
// a range's ends, under overlapping ranges and under a range much longer than the ranges that start after it; a
// release runs exactly the queued parts no other range still covers; a commit takes queued parts off the queue, under
// the queue lock, and they read zero; a release of an id that is not registered changes nothing; a release that has to
// wait for the queue lock waits while another thread holds it (and gives up at once with wait 0), and its pages stay
// committed until then; nothing is left queued after the last release, and nothing allocated after ih_reset.
//
// Part two, threads, as in the game: a submitting thread holds the queue lock while it "pins" every live import, and
// every page of each must stay committed from the start of the submission to its end (the condition the driver's
// submission fails without); a game thread maps and unmaps memory, makes imports over mapped memory, finalizes them
// (released at once after a wait, or handed to the completion thread) and unmaps their memory right after; a
// completion thread frees the handed imports and releases them without waiting, again later while a lock is held. At
// the end every page is committed exactly when the game has it mapped, and every page the game mapped read zero.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

// Every allocation of import_hold.c is counted: after ih_reset none may be live. fail_alloc makes realloc fail.
static int64_t live;
static int fail_alloc;
static void *count_realloc(void *p, size_t n) {
    if (fail_alloc) return NULL;
    void *q = realloc(p, n);
    if (q && !p) __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    return q;
}
static void count_free(void *p) {
    if (p) __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    free(p);
}
#define realloc count_realloc
#define free count_free
#include "import_hold.c"
#undef realloc
#undef free

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)

// ---- guest memory: PAGES pages from BASE
#define BASE 0x500000000ull
#define PAGE 0x1000ull
#define PAGES 8192u
#define A(off) (BASE + (uint64_t)(off))
static uint8_t pstate[PAGES];    // 1 committed
static uint32_t pword[PAGES];    // the game's word; 0 after a decommit
static uint32_t pg(uint64_t a) { return (uint32_t)((a - BASE) / PAGE); }

static int all_state(uint64_t lo, uint64_t hi, uint8_t want) {
    for (uint64_t a = lo; a < hi; a += PAGE)
        if (LOAD(pstate[pg(a)]) != want) return 0;
    return 1;
}
static int committed(uint64_t lo, uint64_t hi) { return all_state(lo, hi, 1); }
static int given_back(uint64_t lo, uint64_t hi) { return all_state(lo, hi, 0); }
static void write_words(uint64_t lo, uint64_t hi, uint32_t v) {
    for (uint64_t a = lo; a < hi; a += PAGE) STORE(pword[pg(a)], v);
}
static int words_are(uint64_t lo, uint64_t hi, uint32_t v) {
    for (uint64_t a = lo; a < hi; a += PAGE)
        if (LOAD(pword[pg(a)]) != v) return 0;
    return 1;
}

// ---- the two locks, as memory.c and vk_memory.c take them
static PlatMutex frz = PLAT_MUTEX_INIT;       // memory.c frz_lock
static PlatRMutex ql = PLAT_RMUTEX_INIT;      // the device's queue lock (gfx_present.c g_ql)
static __thread int q_mine;                   // this thread holds ql
static void q_lock(void) { plat_rmutex_lock(&ql), q_mine++; }
static int q_trylock(void) {
    if (!plat_rmutex_trylock(&ql)) return 0;
    q_mine++;
    return 1;
}
static void q_unlock(void) { q_mine--, plat_rmutex_unlock(&ql); }

static int64_t outside_q;   // queued parts run, or committed parts zeroed, without the queue lock

typedef struct { int ok; uint64_t parts, bytes; } Run;
// memory.c hold_decommit: thaw, then plat_mem_decommit
static void run_decommit(uint64_t lo, uint64_t hi, uint64_t queued, void *ctx) {
    Run *r = ctx;
    if (queued && !q_mine) __atomic_add_fetch(&outside_q, 1, __ATOMIC_RELAXED);
    for (uint64_t a = lo; a < hi; a += PAGE) STORE(pstate[pg(a)], 0), STORE(pword[pg(a)], 0);
    r->parts++, r->bytes += hi - lo;
}
// memory.c hold_zero: plat_mem_decommit and plat_mem_commit
static void run_zero(uint64_t lo, uint64_t hi, uint64_t queued, void *ctx) {
    Run *r = ctx;
    (void)queued;
    if (!q_mine) __atomic_add_fetch(&outside_q, 1, __ATOMIC_RELAXED);
    for (uint64_t a = lo; a < hi; a += PAGE) STORE(pword[pg(a)], 0), STORE(pstate[pg(a)], 1);
    r->parts++, r->bytes += hi - lo;
}

// memory.c decommit (with the hold on): returns the bytes kept
static uint64_t g_decommit(uint64_t lo, uint64_t hi) {
    plat_mutex_lock(&frz);
    Run r = {1, 0, 0};
    uint64_t kept = 0;
    int ok = ih_decommit(lo, hi, run_decommit, &r, &kept);
    plat_mutex_unlock(&frz);
    CHECK(ok, "decommit %#llx+%#llx failed", (unsigned long long)lo, (unsigned long long)(hi - lo));
    return kept;
}
// memory.c commit: queued parts inside go, then plat_mem_commit over the whole range. Returns the bytes zeroed.
static uint64_t g_commit(uint64_t lo, uint64_t hi) {
    plat_mutex_lock(&frz);
    Run r = {1, 0, 0};
    int ok = ih_commit(lo, hi, run_zero, &r);
    for (uint64_t a = lo; a < hi; a += PAGE) STORE(pstate[pg(a)], 1);
    plat_mutex_unlock(&frz);
    CHECK(ok, "commit %#llx+%#llx failed", (unsigned long long)lo, (unsigned long long)(hi - lo));
    return r.bytes;
}
static uint32_t g_add(uint64_t lo, uint64_t hi) {
    plat_mutex_lock(&frz);
    uint32_t id = ih_add(lo, hi);
    plat_mutex_unlock(&frz);
    return id;
}
// memory.c mem_import_release: wait 0 only tries both locks. *ran: the bytes decommitted.
static int g_release(uint32_t id, int wait, uint64_t *ran) {
    if (wait) plat_mutex_lock(&frz);
    else if (!plat_mutex_trylock(&frz)) return 0;
    Run r = {1, 0, 0};
    int ok = ih_release(id, wait, run_decommit, &r);
    plat_mutex_unlock(&frz);
    if (ran) *ran = r.bytes;
    return ok;
}
static int g_overlaps(uint64_t lo, uint64_t hi) {
    plat_mutex_lock(&frz);
    int o = ih_overlaps(lo, hi);
    plat_mutex_unlock(&frz);
    return o;
}
static IhStats g_stats(void) {
    IhStats s;
    plat_mutex_lock(&frz);
    ih_stats(&s);
    plat_mutex_unlock(&frz);
    return s;
}

// ---- part one helpers: a thread that holds the queue lock until told, and one that releases with wait 1
static int holder_has, holder_go;
static void holder(void *arg) {
    (void)arg;
    q_lock();
    __atomic_store_n(&holder_has, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&holder_go, __ATOMIC_ACQUIRE)) plat_yield();
    q_unlock();
}
static uint32_t waiter_id;
static int waiter_done;
static uint64_t waiter_ran;
static void waiter(void *arg) {
    (void)arg;
    int r = g_release(waiter_id, 1, &waiter_ran);
    __atomic_store_n(&waiter_done, r, __ATOMIC_RELEASE);
}

static void part_one(void) {
    uint64_t ran;
    g_commit(A(0), A((uint64_t)PAGES * PAGE));
    write_words(A(0), A((uint64_t)PAGES * PAGE), 7);

    // ranges a and c; a decommit over both and the space around them
    uint32_t a = g_add(A(0x10000), A(0x30000)), c = g_add(A(0x50000), A(0x60000));
    CHECK(a && c && a != c, "ih_add: ids %u and %u", a, c);
    uint64_t kept = g_decommit(A(0), A(0x80000));
    CHECK(kept == 0x30000, "decommit over two ranges: %#llx kept, want 0x30000", (unsigned long long)kept);
    CHECK(given_back(A(0), A(0x10000)) && given_back(A(0x30000), A(0x50000)) && given_back(A(0x60000), A(0x80000)),
          "decommit over two ranges: the pages outside them were not given back at once");
    CHECK(committed(A(0x10000), A(0x30000)) && committed(A(0x50000), A(0x60000)) && words_are(A(0x10000), A(0x30000), 7),
          "decommit over two ranges: pages under them did not stay committed as they were");
    CHECK(g_stats().parts == 2 && g_stats().part_bytes == 0x30000, "two parts queued: %llu parts, %#llx bytes",
          (unsigned long long)g_stats().parts, (unsigned long long)g_stats().part_bytes);

    // a decommit outside every range runs at once
    CHECK(g_decommit(A(0x100000), A(0x110000)) == 0 && given_back(A(0x100000), A(0x110000)),
          "a decommit outside the ranges did not run at once");

    // the ends of a range: touching it is not overlapping it
    uint32_t d = g_add(A(0x200000), A(0x210000));
    CHECK(g_decommit(A(0x210000), A(0x220000)) == 0 && given_back(A(0x210000), A(0x220000)),
          "a decommit that starts where a range ends was held");
    CHECK(g_decommit(A(0x1f0000), A(0x200000)) == 0 && given_back(A(0x1f0000), A(0x200000)),
          "a decommit that ends where a range starts was held");
    CHECK(g_decommit(A(0x20f000), A(0x210000)) == 0x1000 && committed(A(0x20f000), A(0x210000)),
          "the last page of a range was not held");
    CHECK(g_decommit(A(0x200000), A(0x201000)) == 0x1000 && committed(A(0x200000), A(0x201000)),
          "the first page of a range was not held");
    CHECK(g_overlaps(A(0x20f000), A(0x210000)) && !g_overlaps(A(0x210000), A(0x211000)) &&
              !g_overlaps(A(0x1ff000), A(0x200000)),
          "ih_overlaps at a range's ends");

    // release a: its part runs, c's stays
    CHECK(g_release(a, 1, &ran) == 1 && ran == 0x20000 && given_back(A(0x10000), A(0x30000)),
          "release of a: %#llx bytes ran, want 0x20000", (unsigned long long)ran);
    CHECK(committed(A(0x50000), A(0x60000)), "release of a gave back c's part");

    // the game maps half of c's part again: those pages come off the queue and read zero; the rest stays queued
    uint64_t zeroed = g_commit(A(0x50000), A(0x58000));
    CHECK(zeroed == 0x8000 && committed(A(0x50000), A(0x58000)) && words_are(A(0x50000), A(0x58000), 0),
          "commit over a queued part: %#llx zeroed, want 0x8000, and they must read 0", (unsigned long long)zeroed);
    CHECK(words_are(A(0x58000), A(0x60000), 7) && committed(A(0x58000), A(0x60000)),
          "commit over a queued part touched the rest of it");
    write_words(A(0x50000), A(0x58000), 9);
    CHECK(g_release(c, 1, &ran) == 1 && ran == 0x8000 && given_back(A(0x58000), A(0x60000)),
          "release of c: %#llx bytes ran, want 0x8000", (unsigned long long)ran);
    CHECK(committed(A(0x50000), A(0x58000)) && words_are(A(0x50000), A(0x58000), 9),
          "release of c gave back the pages the game had mapped again");

    // a commit inside a queued part splits it in three
    uint32_t s = g_add(A(0x300000), A(0x340000));
    CHECK(g_decommit(A(0x300000), A(0x340000)) == 0x40000, "the split part was not queued whole");
    CHECK(g_commit(A(0x310000), A(0x320000)) == 0x10000 && g_stats().parts == 4,
          "commit inside a queued part: %llu parts queued, want 4 (d's two and the split's two)",
          (unsigned long long)g_stats().parts);
    write_words(A(0x310000), A(0x320000), 11);
    CHECK(g_release(s, 1, &ran) == 1 && ran == 0x30000 && given_back(A(0x300000), A(0x310000)) &&
              given_back(A(0x320000), A(0x340000)) && words_are(A(0x310000), A(0x320000), 11),
          "release of the split part: %#llx ran, want 0x30000", (unsigned long long)ran);

    // overlapping ranges: the pages under both wait for both
    uint32_t e = g_add(A(0x400000), A(0x480000)), f = g_add(A(0x440000), A(0x4c0000));
    CHECK(g_decommit(A(0x400000), A(0x4c0000)) == 0xc0000, "decommit under two overlapping ranges");
    CHECK(g_release(e, 1, &ran) == 1 && ran == 0x40000 && given_back(A(0x400000), A(0x440000)) &&
              committed(A(0x440000), A(0x4c0000)),
          "release of the first of two overlapping ranges: %#llx ran, want 0x40000", (unsigned long long)ran);
    CHECK(g_release(f, 1, &ran) == 1 && ran == 0x80000 && given_back(A(0x440000), A(0x4c0000)),
          "release of the second: %#llx ran, want 0x80000", (unsigned long long)ran);

    // a long range and short ones that start after it: a decommit near the long one's end finds it
    uint32_t g = g_add(A(0x1000000), A(0x1800000));
    uint32_t h1 = g_add(A(0x1100000), A(0x1101000)), h2 = g_add(A(0x1700000), A(0x1701000));
    CHECK(g_decommit(A(0x17f0000), A(0x1800000)) == 0x10000 && committed(A(0x17f0000), A(0x1800000)),
          "a decommit near the end of a long range was not held");
    CHECK(g_overlaps(A(0x17ff000), A(0x1800000)) && !g_overlaps(A(0x1800000), A(0x1801000)),
          "ih_overlaps near the end of a long range");
    CHECK(g_release(h1, 1, &ran) == 1 && ran == 0 && g_release(h2, 1, &ran) == 1 && ran == 0 &&
              committed(A(0x17f0000), A(0x1800000)),
          "releasing short ranges gave back the long one's part");
    CHECK(g_release(g, 1, &ran) == 1 && ran == 0x10000 && given_back(A(0x17f0000), A(0x1800000)),
          "release of the long range: %#llx ran, want 0x10000", (unsigned long long)ran);

    // an id that is not registered
    IhStats before = g_stats();
    CHECK(g_release(a, 1, &ran) == 1 && ran == 0 && g_release(0x7fffffff, 0, &ran) == 1 && ran == 0,
          "release of an id that is not registered");
    CHECK(g_stats().ranges == before.ranges && g_stats().parts == before.parts, "it changed the bookkeeping");

    // the queue lock: while another thread holds it, wait 0 gives up and wait 1 waits; the pages stay committed
    uint32_t j = g_add(A(0x600000), A(0x610000));
    CHECK(g_decommit(A(0x600000), A(0x610000)) == 0x10000, "j's part was not queued");
    PlatThread *th = plat_thread_new(holder, NULL, "holder");
    while (!__atomic_load_n(&holder_has, __ATOMIC_ACQUIRE)) plat_yield();
    CHECK(g_release(j, 0, &ran) == 0 && ran == 0 && committed(A(0x600000), A(0x610000)) && g_overlaps(A(0x600000), A(0x601000)),
          "a release without waiting went ahead while another thread held the queue lock");
    waiter_id = j;
    PlatThread *tw = plat_thread_new(waiter, NULL, "waiter");
    plat_sleep_ms(50);
    CHECK(!__atomic_load_n(&waiter_done, __ATOMIC_ACQUIRE) && committed(A(0x600000), A(0x610000)),
          "a release went ahead while another thread held the queue lock (a submission in the driver)");
    __atomic_store_n(&holder_go, 1, __ATOMIC_RELEASE);
    plat_thread_join(th);
    plat_thread_join(tw);
    CHECK(__atomic_load_n(&waiter_done, __ATOMIC_ACQUIRE) == 1 && waiter_ran == 0x10000 &&
              given_back(A(0x600000), A(0x610000)),
          "the waiting release did not run once the queue lock was free");

    // d is the last range: after it nothing is registered or queued
    CHECK(g_release(d, 1, &ran) == 1 && ran == 0x2000, "release of d: %#llx ran, want 0x2000", (unsigned long long)ran);
    IhStats st1 = g_stats();
    CHECK(st1.ranges == 0 && st1.range_bytes == 0 && st1.parts == 0 && st1.part_bytes == 0,
          "after the last release: %llu ranges, %llu parts, %#llx bytes queued", (unsigned long long)st1.ranges,
          (unsigned long long)st1.parts, (unsigned long long)st1.part_bytes);
    CHECK(st1.cancelled_bytes == 0x18000, "cancelled: %#llx, want 0x18000", (unsigned long long)st1.cancelled_bytes);
    CHECK(!LOAD(outside_q), "%lld parts ran or were zeroed without the queue lock", (long long)LOAD(outside_q));

    // out of memory: nothing is registered
    ih_reset();
    CHECK(LOAD(live) == 0, "after ih_reset %lld allocations are live", (long long)LOAD(live));
    fail_alloc = 1;
    CHECK(g_add(A(0), A(0x1000)) == 0 && g_stats().ranges == 0, "ih_add without memory registered a range");
    fail_alloc = 0;
    printf("import_hold_check: one thread: queued, run at release, split, zeroed on commit, waited for the queue lock: "
           "%s\n", fails ? "FAILED" : "ok");
}

// ---- part two: threads
#define SLOTS 48
// S_LIVE, S_RETIRING: its memory object exists (the driver names it). S_FREED: freed, the completion thread releases
// it; S_GOING: freed, the game thread releases it itself (vk_pool_fini of a used pool).
enum { S_FREE, S_LIVE, S_RETIRING, S_FREED, S_GOING };
static struct { int state; uint64_t lo, hi; uint32_t id; } slot[SLOTS];
static int stop;
static uint8_t gmap[PAGES];          // game thread: mapped
static uint64_t subs, pinned_pages, torn, nonzero, retries, sync_releases, handed;

static uint64_t rnd(uint64_t *s) {
    *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
    return *s;
}

// The driver's submission: every page of every import whose memory object exists stays committed throughout.
static void submitter(void *arg) {
    (void)arg;
    uint64_t lo[SLOTS], hi[SLOTS];
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        q_lock();
        int n = 0;
        for (int i = 0; i < SLOTS; i++) {
            int s = __atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE);
            if (s == S_LIVE || s == S_RETIRING) lo[n] = LOAD(slot[i].lo), hi[n] = LOAD(slot[i].hi), n++;
        }
        for (int round = 0; round < 8; round++) {   // in the driver for a while: pinned at the start, used to the end
            for (int k = 0; k < n; k++) {
                if (!committed(lo[k], hi[k])) __atomic_add_fetch(&torn, 1, __ATOMIC_RELAXED);
                pinned_pages += (hi[k] - lo[k]) / PAGE;
            }
            plat_yield();
        }
        q_unlock();
        subs++;
        plat_yield();
    }
}

// The completion thread: frees what the game handed over and releases it without waiting; again later when a lock
// was held.
static void completion(void *arg) {
    (void)arg;
    for (;;) {
        int busy = 0, end = __atomic_load_n(&stop, __ATOMIC_ACQUIRE);
        for (int i = 0; i < SLOTS; i++) {
            int s = __atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE);
            if (s == S_RETIRING) __atomic_store_n(&slot[i].state, s = S_FREED, __ATOMIC_RELEASE);   // the memory object goes
            if (s != S_FREED) continue;
            if (g_release(slot[i].id, 0, NULL) == 1) __atomic_store_n(&slot[i].state, S_FREE, __ATOMIC_RELEASE);
            else busy = 1, retries++;
        }
        if (end && !busy) {
            int left = 0;
            for (int i = 0; i < SLOTS; i++) left |= __atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE) >= S_RETIRING;
            if (!left) return;
        }
        plat_yield();
    }
}

// A run of n pages from a random page, all mapped (want 1) or all unmapped (want 0) for the game; 0 when none found.
static int pick_run(uint64_t *seed, uint32_t n, uint8_t want, uint32_t *at) {
    for (int tries = 0; tries < 16; tries++) {
        uint32_t p = (uint32_t)(rnd(seed) % (PAGES - n));
        uint32_t k = 0;
        while (k < n && gmap[p + k] == want) k++;
        if (k == n) return *at = p, 1;
    }
    return 0;
}

static int registered_over(uint64_t lo, uint64_t hi) {   // game thread: a slot not yet released overlaps [lo, hi)
    for (int i = 0; i < SLOTS; i++) {
        int s = __atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE);
        if (s != S_FREE && slot[i].lo < hi && slot[i].hi > lo) return 1;
    }
    return 0;
}

static void game(uint64_t ops) {
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    uint32_t stamp = 100;
    for (uint64_t op = 0; op < ops; op++) {
        uint64_t r = rnd(&seed);
        uint32_t n = 1 + (uint32_t)(r >> 8) % 24, p;
        switch (r % 8) {
        case 0: case 1:   // map: the pages read zero, then the game writes them
            if (!pick_run(&seed, n, 0, &p)) break;
            g_commit(A((uint64_t)p * PAGE), A((uint64_t)(p + n) * PAGE));
            if (!words_are(A((uint64_t)p * PAGE), A((uint64_t)(p + n) * PAGE), 0)) nonzero++;
            write_words(A((uint64_t)p * PAGE), A((uint64_t)(p + n) * PAGE), ++stamp);
            memset(gmap + p, 1, n);
            break;
        case 2: case 3: {   // an import over mapped memory
            int i = (int)((r >> 32) % SLOTS);
            if (__atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE) != S_FREE || !pick_run(&seed, n, 1, &p)) break;
            STORE(slot[i].lo, A((uint64_t)p * PAGE)), STORE(slot[i].hi, A((uint64_t)(p + n) * PAGE));
            slot[i].id = g_add(slot[i].lo, slot[i].hi);
            __atomic_store_n(&slot[i].state, S_LIVE, __ATOMIC_RELEASE);
            break;
        }
        case 4: {   // finalize an import, then unmap its memory at once, as the game does
            int i = (int)((r >> 32) % SLOTS);
            if (__atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE) != S_LIVE) break;
            uint64_t lo = slot[i].lo, hi = slot[i].hi;
            if (r & 0x100000) {   // used: freed and released on this thread, waiting for the locks
                __atomic_store_n(&slot[i].state, S_GOING, __ATOMIC_RELEASE);
                CHECK(g_release(slot[i].id, 1, NULL) == 1, "a release with wait 1 did not complete");
                __atomic_store_n(&slot[i].state, S_FREE, __ATOMIC_RELEASE);
                sync_releases++;
            } else {   // not used: the completion thread frees it after the next submission
                __atomic_store_n(&slot[i].state, S_RETIRING, __ATOMIC_RELEASE);
                handed++;
            }
            uint32_t a = pg(lo), b = pg(hi);
            for (uint32_t k = a; k < b; k++)
                if (gmap[k]) {
                    uint32_t e = k;
                    while (e < b && gmap[e]) e++;
                    g_decommit(A((uint64_t)k * PAGE), A((uint64_t)e * PAGE));
                    memset(gmap + k, 0, e - k);
                    k = e;
                }
            break;
        }
        default: {   // unmap memory, under a live import or not
            if (!pick_run(&seed, n, 1, &p)) break;
            uint64_t lo = A((uint64_t)p * PAGE), hi = A((uint64_t)(p + n) * PAGE);
            int held = registered_over(lo, hi);
            memset(gmap + p, 0, n);
            g_decommit(lo, hi);
            if (!held) CHECK(given_back(lo, hi), "a decommit no import covered did not run at once");
            break;
        }
        }
    }
}

static void part_two(uint64_t ops) {
    ih_reset();
    memset(pstate, 0, sizeof pstate), memset(pword, 0, sizeof pword);
    PlatThread *ts = plat_thread_new(submitter, NULL, "submitter"), *tc = plat_thread_new(completion, NULL, "completion");
    if (!ts || !tc) { printf("FATAL: no thread\n"); exit(3); }
    game(ops);
    for (int i = 0; i < SLOTS; i++)   // the game ends: every live import is finalized; the completion thread frees them
        if (__atomic_load_n(&slot[i].state, __ATOMIC_ACQUIRE) == S_LIVE)
            __atomic_store_n(&slot[i].state, S_RETIRING, __ATOMIC_RELEASE);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    plat_thread_join(ts);
    plat_thread_join(tc);
    IhStats s = g_stats();
    uint32_t wrong = 0;
    for (uint32_t k = 0; k < PAGES; k++) wrong += LOAD(pstate[k]) != gmap[k];
    CHECK(!LOAD(torn), "%llu times a submission saw a page of an import it named given back", (unsigned long long)torn);
    CHECK(!nonzero, "%llu times pages the game mapped did not read zero", (unsigned long long)nonzero);
    CHECK(!wrong, "at the end %u pages are committed where the game has them unmapped or the other way", wrong);
    CHECK(s.ranges == 0 && s.parts == 0 && s.part_bytes == 0, "at the end %llu ranges, %llu parts (%#llx bytes) are left",
          (unsigned long long)s.ranges, (unsigned long long)s.parts, (unsigned long long)s.part_bytes);
    CHECK(!LOAD(outside_q), "%lld parts ran or were zeroed without the queue lock", (long long)LOAD(outside_q));
    CHECK(s.waited_parts > 0 && s.cancelled_bytes > 0 && retries > 0 && subs > 0,
          "the run did not reach every path: %llu parts waited, %#llx bytes cancelled, %llu retries, %llu submissions",
          (unsigned long long)s.waited_parts, (unsigned long long)s.cancelled_bytes, (unsigned long long)retries,
          (unsigned long long)subs);
    printf("import_hold_check: threads: %llu operations, %llu submissions pinning %llu pages, %llu imports released at "
           "once and %llu by the completion thread (%llu retries), %.1f MiB queued, %llu parts run after waiting (longest "
           "%.1f ms), %.1f MiB committed again before their turn: %s\n",
           (unsigned long long)ops, (unsigned long long)subs, (unsigned long long)pinned_pages,
           (unsigned long long)sync_releases, (unsigned long long)handed, (unsigned long long)retries,
           s.queued_bytes / 1048576.0, (unsigned long long)s.waited_parts, s.wait_max_ns / 1e6,
           s.cancelled_bytes / 1048576.0, fails ? "FAILED" : "ok");
    ih_reset();
    CHECK(LOAD(live) == 0, "after ih_reset %lld allocations are live", (long long)LOAD(live));
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    double scale = argc > 1 ? atof(argv[1]) : 1.0;   // 1 by default; the sanitizer runs pass a smaller one
    if (scale <= 0 || scale > 1) scale = 1.0;
    IhQueue q = {q_lock, q_trylock, q_unlock};
    ih_set_queue(&q);
    part_one();
    part_two((uint64_t)(400000 * scale));
    printf("import_hold_check: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
