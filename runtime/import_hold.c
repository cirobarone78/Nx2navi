// The bookkeeping of the import hold (import_hold.h; why it exists: vk_memory.c "import hold").
//
// Registered ranges are kept sorted by their start. A range that reaches an address a starts at or after a minus the
// longest range ever registered (maxlen), so a lookup searches from there and stops at the first range that starts at
// or after the end of what it looks for.
//
// Queued parts are few (the pools finalized since the last submission completed) and kept unsorted. Each lies wholly
// under registered ranges when it is queued, and stays queued only while that is so: coverage shrinks only when a
// range is released, and a release checks again exactly the parts the released range touched. So the last release
// leaves nothing queued.
#include <stdlib.h>
#include <string.h>

#include "import_hold.h"
#include "platform/platform.h"

typedef struct { uint64_t lo, hi; uint32_t id; } Range;
typedef struct { uint64_t lo, hi, t; } Part;   // t: when it was queued
typedef struct { uint64_t lo, hi; } Span;

static Range *rs;                 // sorted by lo
static int nr, cr;
static uint64_t maxlen;           // the longest range ever registered
static uint32_t last_id;
static Part *qs, *qs2;            // the queue; ih_release builds the next one in qs2
static int nq, cq, cq2;
static Span *cov;                 // scratch: covered pieces (covered)
static int ccov;
static Part *runs;                // scratch: the parts a release decommits
static int cruns;
static IhQueue queue;
static IhStats st;                // parts and part_bytes are counted in ih_stats

static int reserve(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return 1;
    int c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q) return 0;
    *p = q, *cap = c;
    return 1;
}

void ih_set_queue(const IhQueue *q) { queue = *q; }

// The index of the first range that can reach lo.
static int first_reaching(uint64_t lo) {
    uint64_t from = lo > maxlen ? lo - maxlen : 0;
    int a = 0, b = nr;
    while (a < b) {
        int m = a + (b - a) / 2;
        if (rs[m].lo < from) a = m + 1;
        else b = m;
    }
    return a;
}

// The parts of [lo, hi) that registered ranges other than rs[skip] cover, sorted and merged, into cov. Returns how
// many; -1 when out of memory.
static int covered(uint64_t lo, uint64_t hi, int skip) {
    int n = 0;
    for (int i = first_reaching(lo); i < nr && rs[i].lo < hi; i++) {
        if (i == skip || rs[i].hi <= lo) continue;
        uint64_t a = rs[i].lo > lo ? rs[i].lo : lo, b = rs[i].hi < hi ? rs[i].hi : hi;
        if (n && a <= cov[n - 1].hi) {   // the starts come in order: overlapping and touching pieces merge
            if (b > cov[n - 1].hi) cov[n - 1].hi = b;
            continue;
        }
        if (!reserve((void **)&cov, &ccov, n + 1, sizeof *cov)) return -1;
        cov[n++] = (Span){a, b};
    }
    return n;
}

uint32_t ih_add(uint64_t lo, uint64_t hi) {
    if (lo >= hi || !reserve((void **)&rs, &cr, nr + 1, sizeof *rs)) return 0;
    int a = 0, b = nr;   // after every range that starts at or before lo: imports mostly come in rising order
    while (a < b) {
        int m = a + (b - a) / 2;
        if (rs[m].lo <= lo) a = m + 1;
        else b = m;
    }
    memmove(rs + a + 1, rs + a, sizeof *rs * (size_t)(nr - a));
    if (!++last_id) last_id = 1;
    rs[a] = (Range){lo, hi, last_id};
    nr++;
    if (hi - lo > maxlen) maxlen = hi - lo;
    st.ranges++, st.range_bytes += hi - lo;
    return last_id;
}

static void queued_add(uint64_t bytes) {
    st.part_bytes += bytes;
    if (st.part_bytes > st.part_bytes_max) st.part_bytes_max = st.part_bytes;
}

int ih_decommit(uint64_t lo, uint64_t hi, IhRun run, void *ctx, uint64_t *kept) {
    *kept = 0;
    if (lo >= hi) return 1;
    int n = covered(lo, hi, -1);
    if (n < 0 || !reserve((void **)&qs, &cq, nq + n, sizeof *qs)) return 0;
    uint64_t at = lo, now = n ? plat_now_ns() : 0;
    for (int k = 0; k < n; k++) {
        if (cov[k].lo > at) run(at, cov[k].lo, 0, ctx);
        qs[nq++] = (Part){cov[k].lo, cov[k].hi, now};
        *kept += cov[k].hi - cov[k].lo;
        at = cov[k].hi;
    }
    if (at < hi) run(at, hi, 0, ctx);
    queued_add(*kept);
    st.queued_bytes += *kept;
    return 1;
}

int ih_commit(uint64_t lo, uint64_t hi, IhRun zero, void *ctx) {
    int m = 0;
    for (int i = 0; i < nq; i++) m += qs[i].lo < hi && qs[i].hi > lo;
    if (!m) return 1;
    if (!reserve((void **)&qs, &cq, nq + m, sizeof *qs)) return 0;   // a part split in two leaves one more
    queue.lock();
    for (int i = 0; i < nq;) {
        Part p = qs[i];
        if (p.hi <= lo || p.lo >= hi) {
            i++;
            continue;
        }
        uint64_t a = p.lo > lo ? p.lo : lo, b = p.hi < hi ? p.hi : hi;
        zero(a, b, p.t, ctx);
        st.part_bytes -= b - a, st.cancelled_bytes += b - a;
        if (p.lo < a && p.hi > b) qs[i++] = (Part){p.lo, a, p.t}, qs[nq++] = (Part){b, p.hi, p.t};
        else if (p.lo < a) qs[i++] = (Part){p.lo, a, p.t};
        else if (p.hi > b) qs[i++] = (Part){b, p.hi, p.t};
        else qs[i] = qs[--nq];   // the part that moves here is looked at next
    }
    queue.unlock();
    return 1;
}

int ih_release(uint32_t id, int wait, IhRun run, void *ctx) {
    int at = -1;
    for (int i = 0; i < nr && at < 0; i++)
        if (rs[i].id == id) at = i;
    if (at < 0) return 1;
    if (wait) queue.lock();
    else if (!queue.trylock()) return 0;
    uint64_t rlo = rs[at].lo, rhi = rs[at].hi;
    // The next queue and the parts to run are built first, so that running out of memory changes nothing.
    int n2 = 0, nrun = 0;
    for (int i = 0; i < nq; i++) {
        Part p = qs[i];
        if (p.hi <= rlo || p.lo >= rhi) {   // untouched by the release: still wholly covered
            if (!reserve((void **)&qs2, &cq2, n2 + 1, sizeof *qs2)) goto oom;
            qs2[n2++] = p;
            continue;
        }
        int n = covered(p.lo, p.hi, at);
        if (n < 0) goto oom;
        uint64_t x = p.lo;
        for (int k = 0; k <= n; k++) {
            uint64_t e = k < n ? cov[k].lo : p.hi;
            if (e > x) {
                if (!reserve((void **)&runs, &cruns, nrun + 1, sizeof *runs)) goto oom;
                runs[nrun++] = (Part){x, e, p.t};
            }
            if (k == n) break;
            if (!reserve((void **)&qs2, &cq2, n2 + 1, sizeof *qs2)) goto oom;
            qs2[n2++] = (Part){cov[k].lo, cov[k].hi, p.t};
            x = cov[k].hi;
        }
    }
    memmove(rs + at, rs + at + 1, sizeof *rs * (size_t)(nr - at - 1));
    nr--;
    st.ranges--, st.range_bytes -= rhi - rlo;
    Part *t = qs;
    int c = cq;
    qs = qs2, cq = cq2, nq = n2;
    qs2 = t, cq2 = c;
    for (int i = 0; i < nrun; i++) {
        run(runs[i].lo, runs[i].hi, runs[i].t, ctx);
        uint64_t b = runs[i].hi - runs[i].lo, w = plat_now_ns() - runs[i].t;   // until it is given back
        st.part_bytes -= b, st.waited_parts++, st.waited_bytes += b;
        if (w > st.wait_max_ns) st.wait_max_ns = w;
    }
    queue.unlock();
    return 1;
oom:
    queue.unlock();
    return -1;
}

int ih_overlaps(uint64_t lo, uint64_t hi) {
    for (int i = first_reaching(lo); i < nr && rs[i].lo < hi; i++)
        if (rs[i].hi > lo) return 1;
    return 0;
}

void ih_stats(IhStats *s) {
    *s = st;
    s->parts = (uint64_t)nq;
}

void ih_reset(void) {
    free(rs), free(qs), free(qs2), free(cov), free(runs);
    rs = NULL, qs = qs2 = runs = NULL, cov = NULL;
    nr = cr = nq = cq = cq2 = ccov = cruns = 0;
    maxlen = 0;
    memset(&st, 0, sizeof st);
}
