// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 Peiikai (MIT; see
// THIRD_PARTY_NOTICES.md).
// Guest address space and the memory SVCs: heap, module data permissions, stack moves,
// QueryMemory, physical memory in the alias region, GetInfo memory ids, and the kernel-region allocator.
//
// Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages
// come back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are
// reserved write-watched (platform.h plat_mem_reserve): mem_written_pages reports the pages the CPU wrote, which
// the Vulkan back end uploads again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define LOW_SIZE (14 * GiB)        // stack + heap block: room for a Switch 2 heap (HEAP_CAP)
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (12 * GiB)        // heap region: a Switch 2 application pool is 9809 MiB (the 12 GB console arrangement)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
#define DEFAULT_POOL rt_app_pool_bytes  // retail application pool of the title (generated: NX2NAVI 0xCD500000, Switch 2 9809 MiB)
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define MAX_STACK_MAPS 4096
#define REEXEC_LIMIT 20

// Horizon memory states, permissions, attributes and results used here.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u   // misaligned address
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;   // fixed module window
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t heap_size, alias_bytes;
static uint8_t *alias_pages;        // 1 per mapped 4 KiB page of the alias region
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[MOD_COUNT];   // per data page of each module: perm | ATTR_PERMISSION_LOCKED

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int reserve_at(uint64_t at, uint64_t size, int watch) { return plat_mem_reserve(at, size, watch); }

// ---- frozen regions
// A 2 MiB region of the watched reservations that nothing wrote for a while can be made read-only (mem_freeze), so the
// back end's write-watch queries skip it: no page of a frozen region changes before a thaw. The first write faults and
// thaw_handler makes the region writable again before the write retries. A kernel write into guest memory (a file
// read) fails on a read-only page instead of faulting, so file reads thaw their buffer first (mem_thaw), and so does
// every commit, decommit and protection change here. A region stays thawed for FRZ_HOLD_MS after a mem_thaw: a file
// read still in flight writes its buffer later. frz_lock is a leaf lock: nothing writes guest memory under it.
#define FRZ_SHIFT 21
#define FRZ_SIZE (1ull << FRZ_SHIFT)
#define FRZ_HOLD_MS 5000u
static volatile uint8_t frz[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t thaw_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: GetTickCount of the last thaw request, 0 never
static volatile int32_t nfrz;
static PlatMutex frz_lock = PLAT_MUTEX_INIT;
static volatile int64_t frz_count[4];   // freezes; thaws by a write fault, by mem_thaw, by a mapping change

// Probed regions (NX2NAVI_WB_PROBE=1, a diagnosis). The back end copies what the GPU wrote into a mirrored pool back
// into guest memory after every submission, in case the CPU or a host path reads it; whether anything does is what
// the probe finds out. A 2 MiB region that lies wholly inside a range just written back is made inaccessible
// (mem_probe). Its first read or write faults: probe_fault gives the region back the protection it had (read-only
// when frozen, else read-write), notes the address, the instruction and the thread, and the access runs again. The
// bytes are in guest memory all along, so the probe only watches. A probed region is not frozen anew (freeze_locked
// finds pages that are not read-write), and every thaw, which precedes file reads into guest memory and mapping
// changes, ends the probe of its region first.
#define PRB_HITS 64
static volatile uint8_t prb[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static volatile int32_t nprb, prb_on;
static struct { uint64_t addr, ip; uint32_t tid; uint8_t write; } prb_hit[PRB_HITS];   // frz_lock: the first of each report
static int prb_nhit;                    // frz_lock
static uint64_t prb_arms, prb_ends[2];  // frz_lock: regions armed; probes ended by a read, by a write

static int watched(uint64_t addr, uint64_t size) {
    const RtGuestLayout *g = &L;
    return (addr >= g->aslr.base && size <= LOW_SIZE && addr - g->aslr.base <= LOW_SIZE - size) ||
           (addr >= g->alias.base && size <= ALIAS_SIZE && addr - g->alias.base <= ALIAS_SIZE - size);
}

// frz_lock held exclusively: the pages of a probed region get back what they had, read-only in a frozen region.
static void unprobe_locked(uint64_t r) {
    if (!prb[r]) return;
    int prot = frz[r] ? PLAT_PROT_READ : PLAT_PROT_RW;
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)   // every inaccessible run is the probe's: mem_probe took only regions without one
        if (plat_mem_state(a, end, &b) == PLAT_PROT_NONE && !plat_mem_protect(a, b - a, prot))
            rt_fatal("memory: ending the probe of %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
    __atomic_store_n(&prb[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nprb);
}

// Lazy write-back (NX2NAVI_WB_LAZY, default 1 on Windows and 0 elsewhere, vk_memory.c "lazy write-back"). It was off
// by default in 0.7.0: with it on, single frames came out with the whole scene's shadowing missing, 10 to 180 of 1,800
// presents standing in Hyrule Field with other programs keeping the CPU busy and none found with it off. That was
// not this code's doing: frames were drawn from copies taken before the game's signal that their data was written
// (vk_exec_xfer.c "events the CPU signals"), and with the write-backs gone the frames came faster and the signal fell
// into the replay more often. With that fixed, two such runs under eight busy threads showed none.
// What a shader wrote into a mirrored pool is
// copied back into guest memory at the end of every submission, in case the CPU reads it. Ocarina's CPU does not read
// most of it: standing in Hyrule Field, 66 MiB a frame went back over PCIe, 7 to 9 ms of each frame's 13 to 15 ms of
// GPU time, and the write-back probe found no access to it in 40 s. So a range that went back once is armed here: its
// whole pages are made inaccessible (mem_lazy_arm), and while it stays armed the back end leaves its later writes in
// the mirror (mem_lazy_skip marks it stale). The first read or write of an armed page faults: lazy_fault has the
// back end copy the range's bytes down now (lz_fetch, a GPU copy after everything submitted so far), gives the pages
// back and runs the access again, and the range stays out for a while (lz_hot: 1 s, doubling to 32 s while it keeps
// being touched), written back every submission as before. Every thaw (before the kernel writes or reads guest
// memory, and before mapping changes) releases the armed ranges of its regions the same way.
// A region with armed pages is not frozen (freeze_locked wants every page read-write), and a frozen one is not armed.
// A fetch brings what the GPU work handed over so far wrote. The part of a submission that is being recorded has left
// its writes out as well, and makes up for it when it finds a range it skipped no longer armed (vk_memory.c
// lazy_part_check). Between that check and the part's hand-over a release would be seen by neither side, so the
// replay opens a window there (mem_lazy_window) and a fault on an armed page waits for it to close: the fetch then
// has the part's writes. Only a fault of the replay thread itself, a wait of 20 ms, or a release by a thaw goes ahead
// inside the window (lz_gaps counts the faults): that part's writes to the range then reach guest memory with the
// range's next write-back.
typedef struct { uint64_t lo, hi, h0, h1; int64_t delta; uint8_t stale; } LzSeg;   // pages [lo, hi); buffer offset = address + delta
static LzSeg *lzs;                      // frz_lock
static int lz_n, lz_cap;                // frz_lock
static volatile uint8_t lzr[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // a segment reaches into the region
static uint32_t lz_hot[RT_ADDRESS_LIMIT >> FRZ_SHIFT];        // frz_lock: plat_now_ms() | 1 until which nothing is armed here
static uint8_t lz_back[RT_ADDRESS_LIMIT >> FRZ_SHIFT];        // frz_lock: seconds of the region's last hold
static volatile int32_t lz_on;
static int lz_want;                     // the setting, read in mem_init
static void (*lz_fetch)(uint64_t h0, uint64_t h1, uint64_t off, uint64_t size);
// The back end's test that the mirror holds every CPU write to [lo, hi) (vk_memory.c lazy_clean): a fetch copies the
// mirror's bytes over the whole range, so a CPU write the mirror does not have yet would be lost to it.
static int (*lz_clean)(uint64_t lo, uint64_t hi, void *ctx);
static uint64_t lz_unclean;             // frz_lock: arms refused since the last report because the mirror lacked a CPU write
// frz_lock: since the last report: faults by a read, by a write, other releases, fetches, their bytes and time
static uint64_t lz_faults[2], lz_other, lz_fetches, lz_fetch_bytes, lz_fetch_ns, lz_gaps;
static volatile uint32_t lz_win_tid;    // the replay thread's id while its part is between check and hand-over; 0 else

static void lazy_flags_locked(uint64_t lo, uint64_t hi) {
    for (uint64_t r = lo >> FRZ_SHIFT; r <= (hi - 1) >> FRZ_SHIFT; r++) {
        int any = 0;
        for (int j = 0; j < lz_n && !any; j++)
            any = lzs[j].lo < ((r + 1) << FRZ_SHIFT) && lzs[j].hi > (r << FRZ_SHIFT);
        __atomic_store_n(&lzr[r], (uint8_t)any, __ATOMIC_RELEASE);
    }
}

// frz_lock held exclusively. Segment i goes: its bytes come down first when the mirror's are newer, then its pages
// are read-write again. why: 0 a read fault, 1 a write fault (both hold the regions back), else a thaw or the mirror
// going away.
static void lazy_drop_locked(int i, int why) {
    LzSeg s = lzs[i];
    if (s.stale && lz_fetch) {
        uint64_t t0 = plat_now_ns();
        lz_fetch(s.h0, s.h1, (uint64_t)((int64_t)s.lo + s.delta), s.hi - s.lo);
        lz_fetches++, lz_fetch_bytes += s.hi - s.lo, lz_fetch_ns += plat_now_ns() - t0;
    }
    for (uint64_t a = s.lo, b; a < s.hi; a = b)
        if (plat_mem_state(a, s.hi, &b) == PLAT_PROT_NONE && !plat_mem_protect(a, b - a, PLAT_PROT_RW))
            rt_fatal("memory: giving back the lazily written pages %#llx+%#llx failed", (unsigned long long)a,
                     (unsigned long long)(b - a));
    lzs[i] = lzs[--lz_n];
    lazy_flags_locked(s.lo, s.hi);
    if (why > 1) { lz_other++; return; }
    lz_faults[why]++;
    uint32_t now = (uint32_t)plat_now_ms();
    for (uint64_t r = s.lo >> FRZ_SHIFT; r <= (s.hi - 1) >> FRZ_SHIFT; r++) {
        int again = lz_hot[r] && (int32_t)(now - lz_hot[r]) < 60000;   // touched again within a minute of its last hold
        uint8_t b = again ? (uint8_t)(lz_back[r] < 32 ? lz_back[r] * 2 : 32) : 1;
        lz_back[r] = b ? b : 1;
        lz_hot[r] = (now + (uint32_t)lz_back[r] * 1000u) | 1;
    }
}

// frz_lock held exclusively: every segment that reaches into region r goes.
static void lazy_drop_region_locked(uint64_t r, int why) {
    for (int i = lz_n; i-- > 0;)
        if (lzs[i].lo < ((r + 1) << FRZ_SHIFT) && lzs[i].hi > (r << FRZ_SHIFT)) lazy_drop_locked(i, why);
}

static int lazy_covered_locked(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < lz_n; i++)
        if (lzs[i].lo <= lo && lzs[i].hi >= hi) return 1;
    return 0;
}

// frz_lock held exclusively: [x, e), inside one region, becomes inaccessible when every committed page of it is
// read-write, or inaccessible as part of a segment already; anything else (a read-only page, a hole, a page the game
// made inaccessible itself) leaves it as it is. Then the back end checks that the mirror ctx belongs to holds every CPU
// write to [x, e) (lz_clean): after the protection no write can come in, so a write that came before it is in the
// write-watch or the page generations. When the mirror lacks one, the pages are given back and nothing is armed: the
// CPU wrote into the range after its write-back reached guest memory and before this arm, and the next sync of the
// mirror carries that write up; a fetch before it would have copied the mirror's older bytes over it (single frames
// with another frame's lighting constants, or a black scene).
static int lazy_protect_locked(uint64_t x, uint64_t e, void *ctx) {
    uint64_t run[16][2];
    int k = 0;
    for (uint64_t a = x, b; a < e; a = b) {
        int st = plat_mem_state(a, e, &b);
        if (st == PLAT_PROT_RW) {
            if (k == 16) return 0;
            run[k][0] = a, run[k][1] = b, k++;
        } else if (st != PLAT_PROT_NONE || !lazy_covered_locked(a, b)) return 0;
    }
    for (int i = 0; i < k; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_NONE)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            return 0;
        }
    if (lz_clean && !lz_clean(x, e, ctx)) {
        for (int i = 0; i < k; i++) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
        lz_unclean++;
        return 0;
    }
    return 1;
}

// Arms the whole pages of [a, a + n), which the back end has just written back from the buffer h0 into h1 at offset
// off: they fault on their next access. Returns the bytes armed now. Not in a frozen, probed, held or thaw-held region.
uint64_t mem_lazy_arm(uint64_t a, uint64_t n, uint64_t h0, uint64_t h1, uint64_t off, void *ctx) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || a >= RT_ADDRESS_LIMIT || !watched(a, n)) return 0;
    uint64_t lo = (a + PAGE - 1) & ~(PAGE - 1), hi = (a + n) & ~(PAGE - 1), armed = 0;
    if (hi <= lo) return 0;
    int64_t delta = (int64_t)off - (int64_t)a;
    plat_mutex_lock(&frz_lock);
    if (lazy_covered_locked(lo, hi)) {
        plat_mutex_unlock(&frz_lock);
        return 0;
    }
    uint32_t now = (uint32_t)plat_now_ms();
    for (uint64_t x = lo, e; x < hi; x = e) {
        uint64_t r = x >> FRZ_SHIFT;
        e = (r + 1) << FRZ_SHIFT;
        if (e > hi) e = hi;
        if (frz[r] || prb[r] || (lz_hot[r] && (int32_t)(lz_hot[r] - now) > 0) ||
            (thaw_ms[r] && (int32_t)(now - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) || !lazy_protect_locked(x, e, ctx))
            continue;
        uint64_t slo = x, shi = e;
        uint8_t stale = 0;
        for (int j = 0; j < lz_n; j++)   // one segment for what touches in the same buffer
            if (lzs[j].h0 == h0 && lzs[j].delta == delta && lzs[j].lo <= shi && lzs[j].hi >= slo) {
                if (lzs[j].lo < slo) slo = lzs[j].lo;
                if (lzs[j].hi > shi) shi = lzs[j].hi;
                stale |= lzs[j].stale;
                lzs[j] = lzs[--lz_n];
                j = -1;
            }
        if (lz_n == lz_cap) {
            lz_cap = lz_cap ? lz_cap * 2 : 256;
            lzs = realloc(lzs, sizeof *lzs * (size_t)lz_cap);
            if (!lzs) rt_fatal("memory: out of memory for the lazily written ranges");
        }
        lzs[lz_n++] = (LzSeg){slo, shi, h0, h1, delta, stale};
        __atomic_store_n(&lzr[r], 1, __ATOMIC_RELEASE);
        armed += e - x;
    }
    plat_mutex_unlock(&frz_lock);
    return armed;
}

// The armed parts of [a, a + n), sorted, into out (at most max); they count as stale from here on: the caller leaves
// the bytes it is about to write there in the mirror. Returns how many.
int mem_lazy_skip(uint64_t a, uint64_t n, uint64_t out[][2], int max) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || !n || a >= RT_ADDRESS_LIMIT || a + n > RT_ADDRESS_LIMIT) return 0;
    int any = 0, k = 0;
    for (uint64_t r = a >> FRZ_SHIFT; r <= (a + n - 1) >> FRZ_SHIFT && !any; r++) any = lzr[r];
    if (!any) return 0;
    plat_mutex_lock(&frz_lock);
    for (int i = 0; i < lz_n && k < max; i++) {
        uint64_t lo = lzs[i].lo > a ? lzs[i].lo : a, hi = lzs[i].hi < a + n ? lzs[i].hi : a + n;
        if (lo >= hi) continue;
        lzs[i].stale = 1;
        int at = k++;
        for (; at > 0 && out[at - 1][0] > lo; at--) out[at][0] = out[at - 1][0], out[at][1] = out[at - 1][1];
        out[at][0] = lo, out[at][1] = hi;
    }
    plat_mutex_unlock(&frz_lock);
    return k;
}

// [a, a + n) is armed as a whole still.
int mem_lazy_armed(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    plat_mutex_lock(&frz_lock);
    int ok = lazy_covered_locked(a, a + n);
    plat_mutex_unlock(&frz_lock);
    return ok;
}

// Releases what is armed in [a, a + n): before the buffers a fetch would read go away.
void mem_lazy_release(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || !n) return;
    plat_mutex_lock(&frz_lock);
    for (int i = lz_n; i-- > 0;)
        if (lzs[i].lo < a + n && lzs[i].hi > a) lazy_drop_locked(i, 4);
    plat_mutex_unlock(&frz_lock);
}

// The back end is ready to fetch: arming starts. 0 when the setting is off (or the probe is on, which watches the
// same pages).
int mem_lazy_enable(void (*fetch)(uint64_t h0, uint64_t h1, uint64_t off, uint64_t size),
                    int (*clean)(uint64_t lo, uint64_t hi, void *ctx)) {
    if (!lz_want) return 0;
    lz_fetch = fetch, lz_clean = clean;
    __atomic_store_n(&lz_on, 1, __ATOMIC_RELEASE);
    return 1;
}

// Opens (under frz_lock, so no release is half done) or closes the window of the replay's part.
void mem_lazy_window(int open) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    if (!open) {
        __atomic_store_n(&lz_win_tid, 0, __ATOMIC_RELEASE);
        return;
    }
    plat_mutex_lock(&frz_lock);
    __atomic_store_n(&lz_win_tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_mutex_unlock(&frz_lock);
}

static int lazy_fault(uint64_t a, int write) {
    if (a >= RT_ADDRESS_LIMIT || !__atomic_load_n(&lzr[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE)) return 0;
    uint32_t me = plat_thread_id();
    for (int tries = 0;; tries++) {
        plat_mutex_lock(&frz_lock);
        int i = 0;
        while (i < lz_n && !(a >= lzs[i].lo && a < lzs[i].hi)) i++;
        if (i == lz_n) {
            plat_mutex_unlock(&frz_lock);
            return 0;
        }
        uint32_t w = __atomic_load_n(&lz_win_tid, __ATOMIC_ACQUIRE);
        if (w && w != me && tries < 400) {   // up to 20 ms: after the hand-over the fetch has that part's writes
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(50000);
            continue;
        }
        if (w) lz_gaps++;
        lazy_drop_locked(i, write != 0);
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
}

// Test (NX2NAVI_WB_LAZY_TOUCH): the first addresses of up to max armed segments, another set each call, for a caller
// that reads them to make the faults a game's access would.
int mem_lazy_pick(uint64_t *addr, int max) {
    static int next;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    int k = 0;
    plat_mutex_lock(&frz_lock);
    for (; k < max && k < lz_n; k++) addr[k] = lzs[(next + k) % lz_n].lo;
    next += k;
    plat_mutex_unlock(&frz_lock);
    return k;
}

// One log line since the last one; skipped: the bytes the back end left in the mirror since then.
void mem_lazy_report(uint64_t frame, uint64_t skipped) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&frz_lock);
    uint64_t bytes = 0, fr = lz_faults[0], fw = lz_faults[1], other = lz_other, nf = lz_fetches, fb = lz_fetch_bytes,
             fns = lz_fetch_ns, gaps = lz_gaps, unclean = lz_unclean;
    int n = lz_n;
    for (int i = 0; i < lz_n; i++) bytes += lzs[i].hi - lzs[i].lo;
    lz_faults[0] = lz_faults[1] = lz_other = lz_fetches = lz_fetch_bytes = lz_fetch_ns = lz_gaps = lz_unclean = 0;
    plat_mutex_unlock(&frz_lock);
    rt_log("mem: lazy write-back at frame %llu: %d ranges armed, %.1f MiB; since the last line %.1f MiB left in the "
           "mirror, %llu read and %llu write faults (%llu inside a part's window), %llu other releases, %llu fetches of "
           "%.1f MiB in %.2f ms, %llu arms refused (a CPU write not in the mirror yet)", (unsigned long long)frame, n,
           bytes / 1048576.0, skipped / 1048576.0, (unsigned long long)fr, (unsigned long long)fw,
           (unsigned long long)gaps, (unsigned long long)other, (unsigned long long)nf, fb / 1048576.0, fns / 1e6,
           (unsigned long long)unclean);
}

// frz_lock held exclusively. why: 1 a write fault, 2 mem_thaw (holds the region thawed), 3 a mapping change here.
static void thaw_locked(uint64_t r, int why) {
    if (why == 2) thaw_ms[r] = (uint32_t)plat_now_ms() | 1;
    if (lzr[r]) lazy_drop_region_locked(r, why + 1);   // not a fault on an armed page: no hold
    if (prb[r]) unprobe_locked(r);
    if (!frz[r]) return;
    // The read-only runs only: freeze_locked left reserved holes (heap pages given back) as they were. Every committed
    // page of a frozen region was read-write at the freeze, and mapping changes thaw before they run, so these are all
    // ours.
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)
        if (plat_mem_state(a, end, &b) == PLAT_PROT_READ && !plat_mem_protect(a, b - a, PLAT_PROT_RW))
            rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
    __atomic_store_n(&frz[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nfrz);
    plat_inc64(&frz_count[why]);
}

// frz_lock held exclusively.
static void thaw_range_locked(uint64_t a, uint64_t n, int why) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) thaw_locked(r, why);
}

void mem_thaw(uint64_t a, uint64_t n) {
    plat_mutex_lock(&frz_lock);   // also when nothing is frozen: the hold keeps a read's buffer writable
    thaw_range_locked(a, n, 2);
    plat_mutex_unlock(&frz_lock);
}

// frz_lock held exclusively. Makes the committed pages of region r read-only when every one of them is read-write;
// reserved holes (heap pages given back) stay as they are, and thaw_locked finds the read-only runs again.
static int freeze_locked(uint64_t r) {
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, run[16][2];
    int n = 0;
    for (uint64_t x = a, y; x < end; x = y) {
        int st = plat_mem_state(x, end, &y);
        if (st >= PLAT_PROT_NONE) {
            if (st != PLAT_PROT_RW || n == 16) return 0;
            run[n][0] = x, run[n][1] = y, n++;
        } else if (st != PLAT_MEM_RESERVED) return 0;
    }
    for (int i = 0; i < n; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_READ)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            return 0;
        }
    return n > 0;
}

int mem_freeze(uint64_t a) {
    if ((a & (FRZ_SIZE - 1)) || !watched(a, FRZ_SIZE)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    int ok = 0;
    plat_mutex_lock(&frz_lock);
    if (frz[r]) ok = 1;
    else if (thaw_ms[r] && (int32_t)((uint32_t)plat_now_ms() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) ok = 0;   // |1 may be ahead
    else if (freeze_locked(r)) {
        __atomic_store_n(&frz[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nfrz);
        plat_inc64(&frz_count[0]);
        ok = 1;
    }
    plat_mutex_unlock(&frz_lock);
    return ok;
}

int mem_frozen(uint64_t a) { return a < RT_ADDRESS_LIMIT && __atomic_load_n(&frz[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE); }

// Probes the 2 MiB regions that lie wholly inside [a, a + n): makes them inaccessible until their first access.
// Returns how many it armed. Not a region a file read may still be writing into (the thaw hold), nor one with a page
// that is not plainly read-write (read-only when frozen): those are left alone.
int mem_probe(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&prb_on, __ATOMIC_RELAXED) || n < FRZ_SIZE || a >= RT_ADDRESS_LIMIT || !watched(a, n)) return 0;
    uint64_t r0 = (a + FRZ_SIZE - 1) >> FRZ_SHIFT, r1 = (a + n) >> FRZ_SHIFT;   // [r0, r1)
    int armed = 0;
    plat_mutex_lock(&frz_lock);
    for (uint64_t r = r0; r < r1; r++) {
        if (prb[r]) continue;
        if (thaw_ms[r] && (int32_t)((uint32_t)plat_now_ms() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) continue;
        int want = frz[r] ? PLAT_PROT_READ : PLAT_PROT_RW, k = 0, ok = 1;
        uint64_t run[16][2];
        for (uint64_t x = r << FRZ_SHIFT, end = x + FRZ_SIZE, y; x < end && ok; x = y) {
            int st = plat_mem_state(x, end, &y);
            if (st >= PLAT_PROT_NONE) {
                if (st != want || k == 16) ok = 0;
                else run[k][0] = x, run[k][1] = y, k++;
            } else if (st != PLAT_MEM_RESERVED) ok = 0;
        }
        if (!ok || !k) continue;
        for (int i = 0; i < k && ok; i++)
            if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_NONE)) {
                while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], want);
                ok = 0;
            }
        if (!ok) continue;
        __atomic_store_n(&prb[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nprb);
        prb_arms++, armed++;
    }
    plat_mutex_unlock(&frz_lock);
    return armed;
}

// Pages of guest memory may be inaccessible (the probe, or the lazy write-back): a kernel read of them has to thaw first.
int mem_probing(void) { return __atomic_load_n(&prb_on, __ATOMIC_RELAXED) || __atomic_load_n(&lz_on, __ATOMIC_RELAXED); }

// The platform's access-fault hook while the probe is on: the first access of a probed region ends its probe and is
// noted; 1 runs the access again. A read that faulted in a region another thread gave back meanwhile retries too; a
// write goes on to thaw_fault, and any other fault to the crash handler.
static int probe_fault(uint64_t a, int write, uint64_t ip) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    plat_mutex_lock(&frz_lock);
    int was = prb[r];
    if (was) {
        unprobe_locked(r);
        prb_ends[write != 0]++;
        if (prb_nhit < PRB_HITS) prb_hit[prb_nhit++] = (typeof(prb_hit[0])){a, ip, plat_thread_id(), (uint8_t)(write != 0)};
    }
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    if (write) return 0;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) >= PLAT_PROT_READ;
}

// The platform's access-fault hook: an armed page of the lazy write-back first, then the probe. A read that faulted on
// a page another thread gave back meanwhile runs again; a write goes on to thaw_fault.
static int access_fault(uint64_t a, int write, uint64_t ip) {
    if (__atomic_load_n(&lz_on, __ATOMIC_RELAXED) && lazy_fault(a, write)) return 1;
    if (__atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return probe_fault(a, write, ip);
    if (write || a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) >= PLAT_PROT_READ;
}

// One log block for the probe since the last one: how many regions were armed, how many probes a read or a write
// ended, and the first accesses by instruction (an address inside the executable as nx2navi+offset, for the linker
// map or nm). Nothing while the probe is off.
void mem_probe_report(uint64_t frame) {
    if (!__atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return;
    typeof(prb_hit[0]) hit[PRB_HITS];
    plat_mutex_lock(&frz_lock);
    uint64_t arms = prb_arms, reads = prb_ends[0], writes = prb_ends[1];
    int n = prb_nhit;
    memcpy(hit, prb_hit, sizeof hit[0] * (size_t)n);
    prb_arms = prb_ends[0] = prb_ends[1] = 0, prb_nhit = 0;
    plat_mutex_unlock(&frz_lock);
    rt_log("mem: write-back probe at frame %llu: %llu regions of 2 MiB armed, probes ended by %llu reads and %llu writes "
           "since the last line; %d probed now", (unsigned long long)frame, (unsigned long long)arms,
           (unsigned long long)reads, (unsigned long long)writes, (int)__atomic_load_n(&nprb, __ATOMIC_RELAXED));
    uintptr_t exe = plat_exe_base();
    for (int i = 0, lines = 0; i < n && lines < 12; i++) {
        if (!hit[i].ip) continue;   // counted with an earlier access of the same instruction
        int same = 1;
        for (int j = i + 1; j < n; j++)
            if (hit[j].ip == hit[i].ip && hit[j].write == hit[i].write) same++, hit[j].ip = 0;
        rt_log("mem:   %d x a %s at %#llx.. from %s%#llx (thread %u)", same, hit[i].write ? "write" : "read",
               (unsigned long long)hit[i].addr, hit[i].ip >= exe && hit[i].ip - exe < (1ull << 32) ? "nx2navi+" : "",
               (unsigned long long)(hit[i].ip >= exe && hit[i].ip - exe < (1ull << 32) ? hit[i].ip - exe : hit[i].ip),
               hit[i].tid);
        lines++;
    }
}

void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws) {
    *frozen = (uint64_t)__atomic_load_n(&nfrz, __ATOMIC_RELAXED);
    *freezes = (uint64_t)plat_xchg64(&frz_count[0], 0);
    *fault_thaws = (uint64_t)plat_xchg64(&frz_count[1], 0);
    *other_thaws = (uint64_t)plat_xchg64(&frz_count[2], 0) + (uint64_t)plat_xchg64(&frz_count[3], 0);
}

// The first write into a frozen region thaws it and retries. A fault in a region another thread thawed meanwhile
// retries too; any other fault goes on to the crash handler.
// The platform's write-fault hook (plat_mem_fault_hook): 1 retries the write.
static int thaw_fault(uint64_t a) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    plat_mutex_lock(&frz_lock);
    int was = frz[a >> FRZ_SHIFT];
    thaw_locked(a >> FRZ_SHIFT, 1);
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) == PLAT_PROT_RW;
}

// Mapping changes thaw the regions they touch and run under frz_lock, so no freeze sees them half done.
static void commit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_commit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: commit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void decommit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_decommit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: decommit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void protect(uint64_t a, uint64_t n, int prot, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_protect(a, n, prot);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: protect %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

// The process is re-run when a region is taken: Windows places DLLs, heaps and thread stacks at random addresses,
// and a fresh process gets a fresh layout.
static void reexec_or_fail(const char *what) {
    const char *e = cfg_get("NX2NAVI_REEXEC");
    int n = e ? atoi(e) : 0;
    if (n >= REEXEC_LIMIT) rt_fatal("memory: %s is not free after %d attempts", what, n);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    cfg_setenv("NX2NAVI_REEXEC", buf);
    plat_reexec();
}

void mem_init(void) {
    if (L.pool) return;
    // Fixed window: modules, kernel region, host-function addresses.
    mods_lo = MOD_MAIN_BASE;
    mods_hi = align_up(rt_layout[rt_layout_count - 1].base + rt_layout[rt_layout_count - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) reexec_or_fail("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) reexec_or_fail("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) reexec_or_fail("the host-function window");

    // Stack and heap block, then the alias region above it, both searched below the module window.
    uint64_t low = 0, alias = 0;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, 1)) low = at;
    if (!low) reexec_or_fail("a 14 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, 1)) alias = at;
    if (!alias) reexec_or_fail("a 64 GiB alias region below 2^39");

    plat_mem_fault_hook(thaw_fault);   // before the crash handler
    alias_pages = calloc(ALIAS_PAGES, 1);
    if (!alias_pages) rt_fatal("memory: cannot allocate the alias page map");
    // nx2navi.ini is read here, once the address space is claimed and before the first setting: the pool size below
    // depends on NX2NAVI_APP_POOL and on the resolution settings (resolution.c decides them once, on this first call).
    cfg_init();
    if (cfg_bool("NX2NAVI_WB_PROBE")) {   // a diagnosis: off, no access fault reaches probe_fault and nothing is probed
        plat_mem_access_hook(access_fault);
        __atomic_store_n(&prb_on, 1, __ATOMIC_RELEASE);
    } else if (cfg_bool("NX2NAVI_WB_LAZY")) {   // armed only once the back end can fetch (mem_lazy_enable)
        plat_mem_access_hook(access_fault);
        lz_want = 1;
    }
    const char *pool = cfg_get("NX2NAVI_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL + res_heap_extra_bytes(),   // resolution.c
    };
    if (!L.pool) rt_fatal("memory: NX2NAVI_APP_POOL is 0");
}

const RtGuestLayout *mem_layout(void) { return &L; }

// A range inside one watched reservation (stack and heap block, or alias region) only.
static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    return plat_mem_written(addr, size, pages, max, reset);
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 1);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 0);
}

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

// ---- 0x01 SetHeapSize
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    plat_mutex_lock(&lock);
    if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > L.pool) {
        plat_mutex_unlock(&lock);
        return R_INVALID_STATE;
    }
    if (size > heap_size) commit(L.heap.base + heap_size, size - heap_size, "heap");
    if (size < heap_size) decommit(L.heap.base + size, heap_size - size, "heap");
    heap_size = size;
    plat_mutex_unlock(&lock);
    if (addr) *addr = L.heap.base;
    return R_OK;
}

// ---- module data permissions (0x02, 0x03) and module pages for QueryMemory
static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {   // under lock
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {   // under lock
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

// The module whose data segment holds [a, a+n), or -1.
static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            plat_mutex_unlock(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PLAT_PROT_RW : perm == PERM_R ? PLAT_PROT_READ : PLAT_PROT_NONE, "module data");
    plat_mutex_unlock(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

// The only module attribute the SDK sets is the irreversible RELRO lock.
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    plat_mutex_unlock(&lock);
    return R_OK;
}

// ---- 0x04/0x05 MapMemory, UnmapMemory: thread stacks move from heap or alias into the stack region by copy; the
// source is inaccessible while moved and gets the contents back on unmap.
static int alias_all_mapped(uint64_t a, uint64_t n) {   // under lock
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias) return R_INVALID_REGION;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PLAT_PROT_RW, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PLAT_PROT_NONE, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

// ---- 0x2C/0x2D MapPhysicalMemory, UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range and
// leaves mapped pages and their contents alone; NX2navi's heaps grow by re-mapping ranges they partly own.
static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {   // under lock
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) commit(a, n, "physical memory");
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n) return R_INVALID_ADDRESS;
    n = (n + PAGE - 1) & ~(uint64_t)(PAGE - 1);   // the SDK asks for byte ranges; a mapping is whole pages
    if ((a | n) & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;   // the SDK maps LimitReached to its recoverable out-of-memory result
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    plat_mutex_unlock(&lock);
    if (r) rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)", unmap ? "Un" : "",
                  (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                  (unsigned long long)heap_size);
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

// ---- 0x06 QueryMemory
// The region around a that no known region covers: everything else below 2^39 belongs to the host and is reported
// as taken (Code, RX), so the SDK never picks it.
static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + MOD_COUNT + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {   // first index of the run that ends at i
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

static void query_locked(uint64_t a, RtMemoryInfo *out);

static int readable_locked(uint64_t a, uint64_t n) {
    for (uint64_t end = a + n; a < end;) {
        RtMemoryInfo r;
        query_locked(a, &r);
        if (r.state == ST_FREE || r.state == ST_INACCESSIBLE || !(r.perm & PERM_R) || r.base + r.size <= a) return 0;
        a = r.base + r.size;
    }
    return 1;
}

// Whether every byte of [a, a + n) can be read: no free, inaccessible or no-access page (mem_query's regions). For
// host reads of guest memory the guest itself may never make, such as a texture upload of a pool slot no shader
// samples (vk_exec_draw.c, texture pools: stale slots point at unmapped memory).
int mem_readable(uint64_t a, uint64_t n) {
    plat_mutex_lock_shared(&lock);
    int ok = readable_locked(a, n);
    plat_mutex_unlock_shared(&lock);
    return ok;
}

// mem_readable for a read that takes a while on another thread than the guest's: with 1 the range stays mapped until
// mem_read_release, as unmapping it (UnmapPhysicalMemory, a smaller heap, a moved stack) waits for the lock. A pool
// slot's storage the game has just released could otherwise be decommitted halfway through the copy (a texture
// upload faulted that way in tex_bl_copy). In between, the caller takes no lock of this file (mem_query, mem_readable
// and the mapping calls would wait for themselves). 0: not readable, and nothing held.
int mem_read_hold(uint64_t a, uint64_t n) {
    plat_mutex_lock_shared(&lock);
    if (readable_locked(a, n)) return 1;
    plat_mutex_unlock_shared(&lock);
    return 0;
}

void mem_read_release(void) { plat_mutex_unlock_shared(&lock); }

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    plat_mutex_lock_shared(&lock);
    query_locked(a, out);
    plat_mutex_unlock_shared(&lock);
    return R_OK;
}

static void query_locked(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        taken_gap(a, &r.base, &r.size);
        r.state = ST_CODE;
        r.perm = PERM_RX;
    }
    // Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them
    // are clipped.
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    if (out) *out = r;
}

// ---- GetInfo memory ids
int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        plat_mutex_lock_shared(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        plat_mutex_unlock_shared(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;   // system resource in use: our bookkeeping
    case 28: v = 0; break;   // AliasRegionExtraSize: no kernel-owned extension
    default: return 0;
    }
    *out = v;
    return 1;
}

// ---- kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit.
#define KCLASSES 29          // 64 B .. 256 MiB
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static PlatMutex klock = PLAT_MUTEX_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    plat_mutex_lock(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    plat_mutex_unlock(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    // A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
    // wastes the rest.
    plat_mutex_lock(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    plat_mutex_unlock(&klock);
}

// NX2NAVI_TRACE=perf: how many bytes of each guest region are committed, so a process that has grown says which region
// holds the memory. A region is reserved whole at start-up and committed page by page, so the walk reads each run of
// pages of the same state; the alias window is the largest and costs the most steps when it is full.
void mem_guest_census(uint64_t frame) {
    const RtGuestLayout *g = mem_layout();
    const struct { const char *name; RtRange r; } reg[] = {
        {"aslr", g->aslr}, {"stack", g->stack}, {"heap", g->heap},
        {"alias", g->alias}, {"kernel", g->kernel}, {"hostfn", g->hostfn},
    };
    char line[400];
    int o = snprintf(line, sizeof line, "mem: guest regions at frame %llu (MiB committed):",
                     (unsigned long long)frame);
    uint64_t all = 0;
    for (unsigned i = 0; i < sizeof reg / sizeof *reg && o < (int)sizeof line - 40; i++) {
        uint64_t x = reg[i].r.base, end = reg[i].r.base + reg[i].r.size, committed = 0, b = 0;
        while (x < end) {
            int st = plat_mem_state(x, end, &b);
            if (b <= x) break;
            if (st >= 0) committed += b - x;   // a PLAT_PROT_* state: these pages are committed
            x = b;
        }
        all += committed;
        o += snprintf(line + o, sizeof line - o, " %s %.0f", reg[i].name, committed / 1048576.0);
    }
    rt_log("%s; all %.0f", line, all / 1048576.0);
}
