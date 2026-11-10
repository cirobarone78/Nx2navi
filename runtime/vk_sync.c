// Submission, fences and the completion thread of the Vulkan back end.
// One timeline semaphore (VkCtx.timeline) orders everything: each submission records its command handles into a
// primary command buffer (vk_exec_record) and signals the next timeline value, which is the NVN fence value
// the front end hands to syncs. The completion thread waits on the timeline and, in submit order, runs the CPU side
// effects queued for each value (event writes, counter reports, sync state), then frees the memory and texture objects whose users
// completed. A value counts as done only after its side effects ran, so a guest woken by fence_wait sees them.
//
// One timeline value per submission, signalled at ALL_COMMANDS; waits use vkWaitSemaphores, and a failed wait is
// treated as a lost device.
// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

#define WEAK __attribute__((weak))

typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } Pending;
typedef struct { VkCommandBuffer cb; uint64_t fence; } CmdSlot;   // reusable once the timeline reached fence

static PlatMutex start_lock = PLAT_MUTEX_INIT;
static int started;
static PlatThread *completion_thread;

// Submissions: one at a time, so timeline values are signalled in the order they are handed out.
static PlatMutex sub_lock = PLAT_MUTEX_INIT;
static VkCommandPool cmd_pool;
static CmdSlot *slots;
static int nslots;
static int cur_slot;          // the slot vk_sync_submit records into now (an index: take_slot may move slots)
static uint64_t cur_fence;    // the value of the submission being recorded

// Shared with the completion thread (st_lock).
static PlatMutex st_lock = PLAT_MUTEX_INIT;
static PlatCond cv_work = PLAT_COND_INIT;   // a submission, a callback or stop
static PlatCond cv_done = PLAT_COND_INIT;   // done advanced
static uint64_t issued, done;     // newest submitted value; newest value whose side effects ran
static uint64_t reserved;         // asynchronous replay: newest value handed out before its submission (>= issued)
static int stop;
static Pending *queue;            // side effects in the order they were added
static int qhead, qn, qcap;

// ---- submit batching
// Every game submission used to be its own vkQueueSubmit2, about 150 a frame, each a trip into the driver on the render
// thread. Now up to hold_max of them are held and go to the queue together, as one vkQueueSubmit2 with one
// VkSubmitInfo2 each, in the order they were made; a part of a long submission (vk_sync_split) goes at once, with what
// is held before it, since it was split off to start the GPU early. Queue order is what orders them with everything
// else, so held work goes to the queue before anything else does or is waited for:
// - every other submitter, and every idle wait, takes the queue lock through vk_queue_lock, which flushes first;
// - a fence wait for a value still held flushes (fence_wait_impl), as do vk_sync_finish and shutdown;
// - the completion thread flushes what has been held for HOLD_NS, so a guest that polls memory a held submission
//   writes, instead of waiting on its fence, still sees it soon; it only tries the lock, never waits for it.
// H is guarded by the queue lock. NX2NAVI_SUBMIT_BATCH=0 (or 1) hands each to the queue at once; =N holds up to N.
#define HOLD_CAP 16
#define HOLD_NS 500000ull
static struct {
    int n;
    VkSubmitInfo2 si[HOLD_CAP];
    VkCommandBufferSubmitInfo cbi[HOLD_CAP][2];
    VkSemaphoreSubmitInfo sig[HOLD_CAP][2];
    uint64_t last;                    // the newest timeline value a held submission signals, 0 none
    uint64_t batches, subs;           // vkQueueSubmit2 calls and the submissions they carried
} H;
static volatile int32_t held_n;       // H.n, for readers without the lock
static volatile uint64_t held_t0;     // when the oldest held submission was held (kern_now_ns)
static volatile uint64_t queued_v;    // the newest timeline value handed to the queue
static int hold_max = -1;

// ---- the executed timeline
// A submission's write-backs (vk_memory.c vk_mem_mirror_end: what its shaders wrote into VRAM mirrors goes back to guest
// memory) are the last thing in it, 3 to 4 ms of GPU time a frame in Ocarina, 60 MiB merged. The wait after a frame's
// first submission (nvn_api_core.c nvnQueueSubmitCommands) is there so that the GPU has run that submission's passes
// before the game rewrites what they read; it does not need the write-backs, and waited for them too: 5 ms of every
// frame with the render thread idle.
// exec_tl is a second timeline that reaches a submission's value when the GPU has executed its own commands: the
// replay ends the part before the write-backs (vk_exec.c, vk_sync_split_exec), that part signals exec_tl, and the
// write-backs follow in a part of their own, which signals the timeline as before. A submission without that split
// signals both at its end. vk_sync_exec_wait waits on exec_tl; every other wait, the fences the game asks for among
// them, still covers the write-backs. NX2NAVI_WB_SPLIT=0: no split, and the wait is the fence's.
static VkSemaphore exec_tl;
static uint64_t exec_sig;             // sub_lock: the newest value a submitted part signals on exec_tl
static int wb_split = -1;

int vk_sync_wb_split(void) {
    if (wb_split < 0) wb_split = cfg_bool("NX2NAVI_WB_SPLIT");
    return wb_split && exec_tl != VK_NULL_HANDLE;
}

static void hold_flush_locked(VkCtx *c) {
    if (!H.n) return;
    VkResult r = vkQueueSubmit2(c->gfx_queue, (uint32_t)H.n, H.si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        vk_device_fault_report(r);
        rt_fatal("vk: vkQueueSubmit2 of %d held submissions up to value %llu failed (%d)", H.n,
                 (unsigned long long)H.last, (int)r);
    }
    if (H.last) __atomic_store_n(&queued_v, H.last, __ATOMIC_RELEASE);
    H.batches++, H.subs += (uint64_t)H.n;
    H.n = 0, H.last = 0;
    __atomic_store_n(&held_n, 0, __ATOMIC_RELEASE);
}

void vk_sync_flush_held(void) {
    if (!H.n) return;   // queue lock held: H is stable
    VkCtx *c = vk_ctx();
    if (c) hold_flush_locked(c);
}

// The replay's submission si (signalling value, 0 for a part without a signal), held or handed to the queue.
static void queue_submit(VkCtx *c, const VkSubmitInfo2 *si, uint64_t value) {
    if (hold_max < 0) {
        const char *e = cfg_get("NX2NAVI_SUBMIT_BATCH");
        int v = e && *e ? atoi(e) : 4;
        hold_max = v < 1 ? 1 : v > HOLD_CAP ? HOLD_CAP : v;
        rt_log("vk: up to %d submissions handed to the queue together (NX2NAVI_SUBMIT_BATCH)", hold_max);
    }
    // The present holds this lock for its whole flip. A slow flip (frame generation allocating, a swapchain remade) makes
    // the submitting thread wait here; counted as a present wait, so the slow-frame line no longer shows such a frame
    // with nothing blocked.
    uint64_t p0 = perf_begin();
    vk_queue_lock_raw();
    perf_end(PERF_FENCE, p0);
    if (H.n == HOLD_CAP) hold_flush_locked(c);
    int i = H.n++;
    H.si[i] = *si;
    if (si->commandBufferInfoCount)
        memcpy(H.cbi[i], si->pCommandBufferInfos, sizeof H.cbi[i][0] * si->commandBufferInfoCount);
    H.si[i].pCommandBufferInfos = H.cbi[i];
    if (si->signalSemaphoreInfoCount) {   // at most two: the timeline and the executed timeline (exec_tl)
        memcpy(H.sig[i], si->pSignalSemaphoreInfos, sizeof H.sig[i][0] * si->signalSemaphoreInfoCount);
        H.si[i].pSignalSemaphoreInfos = H.sig[i];
    }
    if (value) H.last = value;
    uint64_t now = kern_now_ns();
    if (i == 0) __atomic_store_n(&held_t0, now, __ATOMIC_RELEASE);
    __atomic_store_n(&held_n, H.n, __ATOMIC_RELEASE);
    // A part of a long submission (no value) goes at once with whatever is held: parts exist to start the GPU early.
    // So do held submissions that have waited HOLD_NS, and a full batch.
    if (H.n >= hold_max || !value || now - __atomic_load_n(&held_t0, __ATOMIC_ACQUIRE) >= HOLD_NS) hold_flush_locked(c);
    vk_queue_unlock();
}

// Hands the held submissions to the queue when value is among them (a waiter needs it) or they have waited HOLD_NS
// (aged). The completion thread passes try_only: it never waits for the queue lock, whose holders may wait for it.
static void hold_release(uint64_t value, int aged, int try_only) {
    if (!__atomic_load_n(&held_n, __ATOMIC_ACQUIRE)) return;
    int due = value > __atomic_load_n(&queued_v, __ATOMIC_ACQUIRE) ||
              (aged && kern_now_ns() - __atomic_load_n(&held_t0, __ATOMIC_ACQUIRE) >= HOLD_NS);
    if (!due) return;
    if (try_only) {
        if (!vk_queue_trylock_raw()) return;   // the holder's thread is submitting: it flushes or holds on
    } else {
        vk_queue_lock_raw();
    }
    vk_sync_flush_held();
    vk_queue_unlock();
}

// ---- GPU time (NX2NAVI_TRACE=perf): each submission part (vk_sync_split) writes a timestamp when it starts and when it
// ends, into a ring of query pairs. The completion thread reads a pair once the part's value is reached and adds the
// part's interval to the busy time, counting overlap with the previous part once. Off (one log line) when the queue
// family has no timestamp bits or the period is 0. Upload batches (vk_memory.c) are not timed.
#define TS_PAIRS 1024
static VkQueryPool ts_pool;
static int ts_state;                       // 0 not set up, 1 on, -1 off
static double ts_period;                   // ns per tick
static uint64_t ts_mask;                   // valid bits of a timestamp
static uint32_t ts_next;                   // submitting thread (sub_lock): next pair
static volatile uint8_t ts_busy[TS_PAIRS]; // written until the completion thread read the pair
static int ts_cur = -1;                    // the pair of the part being recorded, -1 untimed
static uint64_t ts_last_end;               // completion thread: end of the latest part read (ticks)
static volatile int64_t ts_busy_ns, ts_parts, ts_missed;
// The same durations, never reset: dynamic resolution differences it across presented frames (the perf window
// consumes ts_busy_ns, so a reader that sampled it would steal from the perf line and see a reset every window).
static volatile int64_t ts_busy_all;
// GPU idle (with calibrated timestamps): the gap before each timed part, split by whether the part was submitted after
// the previous one had ended (the GPU waited for the CPU: starved) or before (it waited for untimed work or a wait).
static uint64_t ts_sub_ns[TS_PAIRS];       // CPU time the part was handed to vkQueueSubmit2
// A pair to read at its value. mirror: the copies of a mirror prologue (vk_sync.c prologue), counted apart as well.
typedef struct { uint32_t pair; int mirror; uint64_t fence; } TsRead;
static TsRead *ts_unsent[4];               // closed and not yet submitted (sub_lock)
static int ts_n_unsent;
static volatile int64_t ts_mirror_ns;     // of ts_busy_ns, the mirror prologues' copies
static uint64_t ts_last_end_ns;            // completion thread: ts_last_end on the CPU clock
static volatile int64_t ts_idle_ns, ts_starved_ns, ts_gaps, ts_max_gap_ns, ts_queued_ns;
static int64_t cal_tick, cal_ns, cal_at;   // completion thread: a calibrated (GPU tick, CPU ns) pair and when it was taken
static PFN_vkGetCalibratedTimestampsKHR cal_fn;

// On the first submission that wants timing (perf_gpu_timing: NX2NAVI_TRACE=perf or the full performance overlay).
static void ts_setup(VkCtx *c) {
    ts_state = -1;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    if (!qp) return;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    free(qp);
    float period = c->props.limits.timestampPeriod;
    if (!bits || period <= 0) {
        rt_log("vk: no GPU timestamps on this queue (valid bits %u, period %.3f ns): the perf line shows no GPU time", bits, period);
        return;
    }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * TS_PAIRS};
    VkResult r = vkCreateQueryPool(c->device, &qi, NULL, &ts_pool);
    if (r != VK_SUCCESS) {
        rt_log("vk: timestamp query pool failed (%d): the perf line shows no GPU time", (int)r);
        return;
    }
    ts_period = period;
    ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    ts_state = 1;
}

// Completion thread: refresh the GPU tick / CPU clock pair every 250 ms; 0 when there is no calibration. The host
// domain is the clock plat_now_ns reads (QPC on Windows, CLOCK_MONOTONIC elsewhere), plat_counter_hz ticks a second.
static int cal_refresh(VkCtx *c) {
    if (!c->has_calibrated) return 0;
    if (!cal_fn) {
        cal_fn = (PFN_vkGetCalibratedTimestampsKHR)vk_dpa(c->device, c->has_calibrated == 2 ? "vkGetCalibratedTimestampsKHR"
                                                                                                        : "vkGetCalibratedTimestampsEXT");
        if (!cal_fn) { c->has_calibrated = 0; return 0; }
    }
    int64_t now = (int64_t)plat_now_ns();
    if (cal_at && now - cal_at < 250000000) return 1;
    VkCalibratedTimestampInfoKHR ci[2] = {{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
#ifdef _WIN32
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR}};
#else
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}};
#endif
    uint64_t v[2], dev;
    if (cal_fn(c->device, 2, ci, v, &dev) != VK_SUCCESS) {
        if (!cal_at) {   // the domain pair is not supported: no split
            rt_log("vk: calibrated timestamps failed: the perf line shows no GPU idle split");
            c->has_calibrated = 0;
        }
        return cal_at != 0;
    }
    cal_tick = (int64_t)(v[0] & ts_mask), cal_ns = (int64_t)((double)v[1] * 1e9 / (double)plat_counter_hz()), cal_at = now;
    return 1;
}

static int64_t tick_ns(uint64_t t) { return cal_ns + (int64_t)((double)((int64_t)t - cal_tick) * ts_period); }

static void max64(volatile int64_t *m, int64_t v) {
    int64_t o = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > o && !__atomic_compare_exchange_n(m, &o, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void ts_read_cb(void *arg);

// Just before vkQueueSubmit2 (sub_lock held): the parts closed since the last submit leave the CPU now. Their reads
// are queued in the order the GPU runs them: a mirror prologue is closed after the part it was made for but runs
// first. Read after its part, its copies counted neither as busy time nor as a gap of their own: they showed up as
// idle time with work queued, 7-8 ms a frame over PCIe 3.0 in Kokiri Forest, and the frame as CPU-bound.
static void ts_submitted(void) {
    uint64_t now = plat_now_ns();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ts_n_unsent; i++) {
            TsRead *t = ts_unsent[i];
            if (!t->mirror != pass) continue;   // the prologues first
            ts_sub_ns[t->pair] = now;
            vk_completion_add(t->fence, ts_read_cb, t);
        }
    ts_n_unsent = 0;
}

static void ts_read_cb(void *arg) {   // completion thread, in submission order
    TsRead *t = arg;
    VkCtx *c = vk_ctx();
    uint64_t v[2];
    if (vkGetQueryPoolResults(c->device, ts_pool, 2 * t->pair, 2, sizeof v, v, sizeof v[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        uint64_t s = v[0] & ts_mask, e = v[1] & ts_mask;
        if (e >= s) {
            if (cal_refresh(c)) {
                int64_t sn = tick_ns(s), en = tick_ns(e), sub = (int64_t)ts_sub_ns[t->pair];
                if (ts_last_end_ns && sn > ts_last_end_ns) {
                    int64_t gap = sn - ts_last_end_ns, late = sub - ts_last_end_ns;
                    plat_add64(&ts_idle_ns, gap);
                    if (late > 0) plat_add64(&ts_starved_ns, late < gap ? late : gap);
                    if (gap > 500000) plat_inc64(&ts_gaps);
                    max64(&ts_max_gap_ns, gap);
                }
                if (sub && sn > sub) plat_add64(&ts_queued_ns, sn - sub);
                if (en > ts_last_end_ns) ts_last_end_ns = en;
            }
            uint64_t from = s > ts_last_end ? s : ts_last_end;
            if (e > from) {
                int64_t ns = (int64_t)((double)(e - from) * ts_period);
                plat_add64(&ts_busy_ns, ns);
                if (t->mirror) plat_add64(&ts_mirror_ns, ns);
                plat_add64(&ts_busy_all, ns);
                perf_gpu_busy((uint64_t)ns);
            }
            if (e > ts_last_end) ts_last_end = e;
            plat_inc64(&ts_parts);
        } else plat_inc64(&ts_missed);   // the counter wrapped
    } else plat_inc64(&ts_missed);
    __atomic_store_n(&ts_busy[t->pair], 0, __ATOMIC_RELEASE);
    free(t);
}

// At the start of a command buffer (sub_lock held): the pair it times, -1 untimed.
static int ts_open(VkCommandBuffer cb) {
    if (ts_state != 1 || !perf_gpu_timing()) {
        if (ts_state || !perf_gpu_timing()) return -1;
        ts_setup(vk_ctx());   // the overlay turned full: timestamps from now on
        if (ts_state != 1) return -1;
    }
    uint32_t p = ts_next % TS_PAIRS;
    if (__atomic_load_n(&ts_busy[p], __ATOMIC_ACQUIRE)) { plat_inc64(&ts_missed); return -1; }
    ts_next++;
    ts_busy[p] = 1;
    vkCmdResetQueryPool(cb, ts_pool, 2 * p, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool, 2 * p);
    return (int)p;
}

// At its end, before vkEndCommandBuffer; fence: the value whose completion covers it. The read is queued at the
// submit (ts_submitted).
static void ts_close(VkCommandBuffer cb, int pair, uint64_t fence, int mirror) {
    if (pair < 0) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool, 2 * (uint32_t)pair + 1);
    ts_sub_ns[pair] = 0;
    TsRead *t = malloc(sizeof *t);
    if (!t) rt_fatal("vk: out of memory");
    *t = (TsRead){(uint32_t)pair, mirror, fence};
    if (ts_n_unsent < 4) ts_unsent[ts_n_unsent++] = t;
    else vk_completion_add(fence, ts_read_cb, t);   // more than a part and its prologue between two submits
}

// The pair of the part being recorded.
static void ts_begin(VkCommandBuffer cb) { ts_cur = ts_open(cb); }

static void ts_end(VkCommandBuffer cb, uint64_t fence) {
    ts_close(cb, ts_cur, fence, 0);
    ts_cur = -1;
}

// An empty submission records no command buffer: its pair is never written.
static void ts_drop(void) {
    if (ts_cur < 0) return;
    __atomic_store_n(&ts_busy[ts_cur], 0, __ATOMIC_RELEASE);
    ts_cur = -1;
}

uint64_t vk_gpu_busy_all(void) { return (uint64_t)__atomic_load_n(&ts_busy_all, __ATOMIC_RELAXED); }

static void gpu_stats(PerfGpu *g) {
    g->have_gpu = ts_state == 1;
    g->gpu_ns = (uint64_t)plat_xchg64(&ts_busy_ns, 0);
    g->gpu_parts = (uint64_t)plat_xchg64(&ts_parts, 0);
    g->gpu_missed = (uint64_t)plat_xchg64(&ts_missed, 0);
    g->have_vram = vk_mem_vram(&g->vram_used, &g->vram_budget, &g->vram_exact);
    g->have_idle = vk_ctx()->has_calibrated != 0 && cal_at != 0;
    g->idle_ns = (uint64_t)plat_xchg64(&ts_idle_ns, 0);
    g->starved_ns = (uint64_t)plat_xchg64(&ts_starved_ns, 0);
    g->queued_ns = (uint64_t)plat_xchg64(&ts_queued_ns, 0);
    g->idle_gaps = (uint64_t)plat_xchg64(&ts_gaps, 0);
    g->max_gap_ns = (uint64_t)plat_xchg64(&ts_max_gap_ns, 0);
    g->mirror_ns = (uint64_t)plat_xchg64(&ts_mirror_ns, 0);
}

// Weak stand-in for vk_exec.c's replay: only empty recordings ({OP_BEGIN, OP_END}) can be submitted.
WEAK int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    (void)cb, (void)fence;
    for (int i = 0; i < n; i++) {
        const op_hdr *h = handles[i] ? nvn_op_first(handles[i]) : NULL;
        if (!h || h->op != OP_BEGIN || h->size < sizeof *h || nvn_op_next(h)->op != OP_END)
            rt_fatal("vk: op replay is not linked");
    }
    return 0;
}

static void completion_main(void *arg) {
    VkCtx *c = arg;
    plat_mutex_lock(&st_lock);
    for (;;) {
        while (!stop && done == issued && !(qn && queue[qhead].fence <= done))
            plat_cond_wait(&cv_work, &st_lock);
        if (stop) break;
        uint64_t reached = done;
        if (issued > done) {
            uint64_t want = done + 1;
            plat_mutex_unlock(&st_lock);
            // Held submissions that have waited long enough go to the queue; while some are held, the wait is short so
            // their age is checked again (submit batching).
            hold_release(0, 1, 1);
            uint64_t tmo = __atomic_load_n(&held_n, __ATOMIC_ACQUIRE) ? HOLD_NS : 100000000ull;
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &c->timeline, .pValues = &want};
            VkResult r = vkWaitSemaphores(c->device, &wi, tmo);   // 100 ms: stop is checked between waits
            if (r != VK_SUCCESS && r != VK_TIMEOUT) {
                vk_device_fault_report(r);
                rt_fatal("vk: vkWaitSemaphores failed (%d; device lost?)", (int)r);
            }
            vkGetSemaphoreCounterValue(c->device, c->timeline, &reached);
            plat_mutex_lock(&st_lock);
            if (reached > issued) reached = issued;
        }
        while (qn && queue[qhead].fence <= reached) {
            Pending p = queue[qhead++];
            qn--;
            plat_mutex_unlock(&st_lock);
            p.fn(p.arg);
            plat_mutex_lock(&st_lock);
        }
        if (reached > done) {
            // The waiters go first (a fence wait, the frame's first submission, the acquire): freeing what the
            // finished submissions held (vk_mem_collect: upload batches, destroyed images, views and buffers, each
            // checked against the timeline itself) is nothing they read, and it takes cmd_lock, which the upload
            // path holds, so done waited behind uploads.
            __atomic_store_n(&done, reached, __ATOMIC_RELEASE);
            plat_cond_broadcast(&cv_done);
            plat_mutex_unlock(&st_lock);
            vk_mem_collect();
            plat_mutex_lock(&st_lock);
        }
    }
    plat_mutex_unlock(&st_lock);
    return;
}

void vk_sync_start(void) {
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&start_lock);
    if (!started) {
        if (!vk_init()) rt_fatal("vk: no usable Vulkan device (see the vk: lines above; NX2NAVI_GPU=null runs without a GPU)");
        VkCtx *c = vk_ctx();
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: submission command pool");
        if (!ts_state && perf_gpu_timing()) ts_setup(c);
        perf_gpu_stats = gpu_stats;
        uint64_t now = 0;
        vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
        if (issued > now) {   // a new device after vk_shutdown: values keep increasing for the front end
            VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline,
                                        .value = issued};
            if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) rt_fatal("vk: timeline restart at %llu", (unsigned long long)issued);
            now = issued;
        }
        plat_mutex_lock(&st_lock);
        issued = done = now;
        if (reserved < now) reserved = now;
        stop = 0;
        plat_mutex_unlock(&st_lock);
        // the executed timeline starts where the timeline is; without it every exec wait is a fence wait
        VkSemaphoreTypeCreateInfo et = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = now};
        VkSemaphoreCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &et};
        if (vkCreateSemaphore(c->device, &ei, NULL, &exec_tl) != VK_SUCCESS) exec_tl = VK_NULL_HANDLE;
        exec_sig = now;
        vk_mem_note_submit(now);
        completion_thread = plat_thread_new(completion_main, c, "vk completion");
        if (!completion_thread) rt_fatal("vk: cannot start the completion thread");
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        vk_astc_mode();   // decides NX2NAVI_ASTC and builds the ASTC decoder now, not at the first ASTC upload
    }
    plat_mutex_unlock(&start_lock);
}

// The index of a command buffer the GPU no longer uses, claimed for value `fence`. Called with sub_lock held; may move
// slots[].
static int claim_slot(VkCtx *c, uint64_t fence) {
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    for (int i = 0; i < nslots; i++)
        if (slots[i].fence <= gpu) {
            slots[i].fence = fence;
            return i;
        }
    CmdSlot *n = realloc(slots, sizeof *n * (size_t)(nslots + 1));
    if (!n) rt_fatal("vk: out of memory (command buffers)");
    slots = n;
    CmdSlot *s = &slots[nslots];
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    s->fence = fence;
    return nslots++;
}

static CmdSlot *take_slot(VkCtx *c, uint64_t fence) {
    cur_slot = claim_slot(c, fence);
    return &slots[cur_slot];
}

// The copies the replay's first accesses to VRAM mirrors queued (vk_memory.c), in a command buffer of their own that is
// submitted right before the part that needs them and timed like a part; VK_NULL_HANDLE when there are none. After the
// part was recorded, sub_lock held; may move slots[].
// First brings the shadow storage up to date with the CPU's writes, which needs no command buffer.
static VkCommandBuffer prologue(VkCtx *c, uint64_t fence) {
    vk_mem_shadow_sync();   // shadow storage: the CPU's writes go in before the part is submitted
    if (!vk_mem_mirror_pending()) return VK_NULL_HANDLE;
    int i = claim_slot(c, fence);   // before reading slots: the claim may move it
    VkCommandBuffer cb = slots[i].cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    int pair = ts_open(cb);
    vk_mem_mirror_prologue(cb);
    ts_close(cb, pair, fence, 1);
    vkEndCommandBuffer(cb);
    return cb;
}

static VkCommandBuffer split_part(int exec);
VkCommandBuffer vk_sync_split(void) { return split_part(0); }
// The part that ends here is the last with the submission's own commands: it signals the executed timeline.
VkCommandBuffer vk_sync_split_exec(void) { return split_part(1); }

static VkCommandBuffer split_part(int exec) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    vk_gpu_passes_part(cb, 1);
    ts_end(cb, cur_fence);
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);   // every command of the part is in cb, ended by its recorder
    vk_upload_flush(0);   // the uploads this part asked for precede it in queue order
    VkCommandBuffer pro = prologue(c, cur_fence);
    VkCommandBufferSubmitInfo cbi[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = pro},
                                        {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb}};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = pro ? 2 : 1,
                        .pCommandBufferInfos = pro ? cbi : cbi + 1};
    VkSemaphoreSubmitInfo esig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = exec_tl,
                                  .value = cur_fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (exec && exec_tl && exec_sig < cur_fence)
        si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &esig, exec_sig = cur_fence;
    ts_submitted();
    queue_submit(c, &si, 0);         // held or handed to the queue (submit batching)
    vk_mem_note_submit(cur_fence);   // objects destroyed from here on may be in use by the part just submitted
    CmdSlot *s = take_slot(c, cur_fence);   // the submitted part keeps its slot until the value is reached
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    vk_crumb(s->cb, VK_CRUMB_SUBMIT, cur_fence);
    ts_begin(s->cb);
    rec_begin(s->cb, cur_fence);
    vk_gpu_passes_part(s->cb, 0);
    return s->cb;
}

// Asynchronous replay (vk_backend.c): the value the next queued submission or present will signal. Values are handed
// out in queue order and the replay thread submits them in that order, so each vk_sync_submit gets exactly its value.
uint64_t vk_sync_reserve(void) {
    vk_sync_start();
    plat_mutex_lock(&st_lock);
    if (reserved < issued) reserved = issued;
    uint64_t v = ++reserved;
    plat_mutex_unlock(&st_lock);
    return v;
}

uint64_t vk_sync_submit(const uint64_t *handles, int n) {
    vk_sync_start();
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&sub_lock);
    vk_mem_dirty_submit();   // NX2NAVI_TRACE=perf: what a mirror would upload before this submit
    uint64_t fence = __atomic_load_n(&issued, __ATOMIC_ACQUIRE) + 1;
    cur_fence = fence;
    CmdSlot *s = take_slot(c, fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    // NX2NAVI_REPLAY_TIME: the replaying thread's CPU time over the replay too (perf_replay_cpu), against its wall time
    static int cpu_on = -1;
    if (cpu_on < 0) cpu_on = cfg_bool("NX2NAVI_REPLAY_TIME");
    uint32_t me = plat_thread_id();
    uint64_t t0 = perf_begin(), cpu0 = cpu_on ? plat_thread_cpu_ns(me) : 0;
    vkBeginCommandBuffer(s->cb, &bi);
    vk_crumb(s->cb, VK_CRUMB_SUBMIT, fence);
    ts_begin(s->cb);
    rec_begin(s->cb, fence);
    vk_gpu_passes_part(s->cb, 0);
    int recorded = vk_exec_record(s->cb, handles, n, fence);
    s = &slots[cur_slot];   // the last part (vk_sync_split)
    vk_gpu_passes_part(s->cb, 1);
    if (recorded) ts_end(s->cb, fence);
    else ts_drop();
    // The recorder is done with the part: the end-of-submission crumb, then the end, by the recorder when it records.
    if (!rec_end_cb(s->cb, VK_CRUMB_SUBMIT_END, fence)) vkEndCommandBuffer(s->cb);
    perf_replay(t0);
    if (cpu0) perf_replay_cpu(plat_thread_cpu_ns(me) - cpu0);
    // After the recording: the uploads it asked for (and every earlier one) precede it in queue order.
    vk_upload_flush(0);
    VkCommandBuffer pro = prologue(c, fence);
    s = &slots[cur_slot];
    VkCommandBufferSubmitInfo cbi[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = pro},
                                        {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb}};
    // the timeline, and the executed timeline unless a part of this submission signalled it already (split_part)
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = c->timeline,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = exec_tl,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    int both = exec_tl && exec_sig < fence;
    if (both) exec_sig = fence;
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = (pro ? 1u : 0u) + (recorded ? 1u : 0u),
                        .pCommandBufferInfos = pro ? cbi : cbi + 1, .signalSemaphoreInfoCount = both ? 2u : 1u,
                        .pSignalSemaphoreInfos = sig};
    ts_submitted();
    queue_submit(c, &si, fence);   // held or handed to the queue (submit batching)
    s->fence = recorded ? fence : 0;
    vk_mem_note_submit(fence);
    plat_mutex_lock(&st_lock);
    __atomic_store_n(&issued, fence, __ATOMIC_RELEASE);
    plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
    static unsigned subs;
    if (++subs % 4096 == 0) {
        rec_stats_log();
        if (subs % (4096 * 8) == 4096)   // statistics only: read without the queue lock
            rt_log("vk: submit batching: %llu submissions in %llu queue calls (%.2f each)", (unsigned long long)H.subs,
                   (unsigned long long)H.batches, H.batches ? (double)H.subs / (double)H.batches : 0.0);
    }
    plat_mutex_unlock(&sub_lock);
    return fence;
}

void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    plat_mutex_lock(&st_lock);
    if (qhead + qn == qcap) {
        if (qhead) {
            memmove(queue, queue + qhead, sizeof *queue * (size_t)qn);
            qhead = 0;
        } else {
            int cap = qcap ? qcap * 2 : 256;
            Pending *q = realloc(queue, sizeof *q * (size_t)cap);
            if (!q) rt_fatal("vk: out of memory (completion queue)");
            queue = q, qcap = cap;
        }
    }
    queue[qhead + qn++] = (Pending){fence, fn, arg};
    // The completion thread sleeps only while nothing is issued beyond done: a callback for a later value (the replay's,
    // for the submission it records: counter reports, freed resources) becomes due through the submit, which wakes it.
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns);

int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns) {
    uint64_t p0 = perf_begin();
    int ok = fence_wait_impl(value, timeout_ns);
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    return ok;
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    hold_release(value, 0, 0);   // a held submission the waiter needs goes to the queue now
    plat_mutex_lock(&st_lock);
    int ok;
    if (value > issued && value > reserved) {
        ok = 0;   // never submitted nor queued: never reached (as in the null back end)
    } else {
        uint64_t t0 = kern_now_ns();
        while (done < value) {
            if (timeout_ns < (1ull << 62)) {
                uint64_t el = kern_now_ns() - t0;
                if (el >= timeout_ns) break;
                plat_cond_wait_ns(&cv_done, &st_lock, timeout_ns - el);
            } else {
                plat_cond_wait(&cv_done, &st_lock);
            }
        }
        ok = done >= value;
    }
    plat_mutex_unlock(&st_lock);
    return ok;
}

// Waits until the GPU has executed submission value's own commands (the executed timeline); its write-backs and the
// CPU side effects of its fence may still be to come. 1 when reached.
int vk_sync_exec_wait(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    if (!vk_sync_wb_split()) return vk_sync_fence_wait(value, timeout_ns);
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value) return 1;
    uint64_t p0 = perf_begin();
    hold_release(value, 0, 0);   // a held submission the waiter needs goes to the queue now
    VkCtx *c = vk_ctx();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &exec_tl, .pValues = &value};
    VkResult r = vkWaitSemaphores(c->device, &wi, timeout_ns);
    if (r != VK_SUCCESS && r != VK_TIMEOUT) {
        vk_device_fault_report(r);
        rt_fatal("vk: the wait for a submission's commands failed (%d; device lost?)", (int)r);
    }
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    return r == VK_SUCCESS;
}

uint64_t vk_sync_fence_done(void) { return __atomic_load_n(&done, __ATOMIC_ACQUIRE); }

uint64_t vk_sync_issued(void) { return __atomic_load_n(&issued, __ATOMIC_ACQUIRE); }

void vk_sync_finish(void) {
    vk_sync_start();
    vk_sync_fence_wait(__atomic_load_n(&issued, __ATOMIC_ACQUIRE), UINT64_MAX);
    vk_upload_flush(1);
}

void vk_sync_shutdown(void) {
    plat_mutex_lock(&start_lock);
    if (started) {
        vk_sync_finish();
        plat_mutex_lock(&st_lock);
        stop = 1;
        plat_cond_broadcast(&cv_work);
        plat_mutex_unlock(&st_lock);
        plat_thread_join(completion_thread);
        completion_thread = NULL;
        plat_mutex_lock(&st_lock);
        qhead = qn = 0;   // callbacks of values never submitted
        plat_mutex_unlock(&st_lock);
        plat_mutex_lock(&sub_lock);
        VkCtx *c = vk_ctx();
        if (c && cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
        if (c && exec_tl) vkDestroySemaphore(c->device, exec_tl, NULL);
        exec_tl = VK_NULL_HANDLE;
        perf_gpu_stats = NULL;
        if (c && ts_pool) vkDestroyQueryPool(c->device, ts_pool, NULL);
        ts_pool = VK_NULL_HANDLE;
        ts_state = 0;
        ts_last_end = 0;
        memset((void *)ts_busy, 0, sizeof ts_busy);
        cmd_pool = VK_NULL_HANDLE;
        free(slots);
        slots = NULL;
        nslots = 0;
        plat_mutex_unlock(&sub_lock);
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&start_lock);
}
