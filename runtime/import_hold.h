// Guest memory that host-memory imports cover, and the decommits that wait for those imports (import_hold.c; why:
// vk_memory.c "import hold"). Own header so tests/import_hold_check.c checks the bookkeeping without Vulkan or a game.
#pragma once

#include <stdint.h>

// The device's queue lock (vk_queue_lock_raw, vk_queue_trylock_raw, vk_queue_unlock). Every submission goes to the
// driver while it is held, so while this side holds it no submission is in the driver.
typedef struct {
    void (*lock)(void);
    int (*trylock)(void);   // 1 when acquired
    void (*unlock)(void);
} IhQueue;

// A part [lo, hi) of guest memory; queued: when it was queued (plat_now_ns), 0 for a part that never waited.
typedef void (*IhRun)(uint64_t lo, uint64_t hi, uint64_t queued, void *ctx);

typedef struct {
    uint64_t ranges, range_bytes;                // registered now
    uint64_t parts, part_bytes, part_bytes_max;  // queued now; the most bytes queued at once
    uint64_t queued_bytes;                       // ever queued
    uint64_t waited_parts, waited_bytes;         // ran after waiting
    uint64_t wait_max_ns;                        // the longest wait of a part that ran
    uint64_t cancelled_bytes;                    // committed again by the guest before their decommit ran
} IhStats;

// None of these takes a lock of its own: the caller serialises them (memory.c: frz_lock). ih_release and ih_commit
// take the queue lock inside that.
void ih_set_queue(const IhQueue *q);
// Registers the import of [lo, hi) (whole pages). Returns its id (never 0); 0 when out of memory.
uint32_t ih_add(uint64_t lo, uint64_t hi);
// The guest gives [lo, hi) back. run() decommits the pages no registered range covers now (queued 0); the others are
// queued, still committed, until ih_release. *kept: the bytes queued. 0 when out of memory (nothing run or queued).
int ih_decommit(uint64_t lo, uint64_t hi, IhRun run, void *ctx, uint64_t *kept);
// The guest commits [lo, hi) again. Queued parts inside it leave the queue: their pages are still committed with the
// bytes they had, so zero() gives them back and commits them again, under the queue lock (no submission in the driver
// sees them inaccessible). 0 when out of memory (nothing changed).
int ih_commit(uint64_t lo, uint64_t hi, IhRun zero, void *ctx);
// The import id's memory object has been freed. Under the queue lock, so that no submission that may still name the
// import is in the driver, id leaves the registry and run() decommits the queued parts no other registered range
// covers (their queued time given). wait 0 only tries the queue lock: 0 when it is held elsewhere (nothing changed;
// call again later). 1 done (also for an id not registered); -1 out of memory (nothing changed).
int ih_release(uint32_t id, int wait, IhRun run, void *ctx);
// A registered range overlaps [lo, hi).
int ih_overlaps(uint64_t lo, uint64_t hi);
void ih_stats(IhStats *s);
// Forgets every range and queued part (their pages stay as they are) and frees the bookkeeping.
void ih_reset(void);
