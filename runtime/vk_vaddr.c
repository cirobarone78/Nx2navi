// GPU address history for a lost device (NX2NAVI_VA_REPORT=1, or 2 with call stacks). With
// VK_EXT_device_address_binding_report the driver reports, through a debug-utils messenger, every GPU address range it
// binds to a buffer, image or internal object and every range it unbinds again. Each report is kept in memory with the
// timeline values issued and completed at that moment (vk_sync.c) and, with 2, for ranges of STACK_MIN bytes or more,
// the host call stack that made it.
//
// vk_device_fault_report hands each faulting address to vk_vaddr_fault, which logs the reports whose ranges covered
// its page, newest first, or, when no range ever did, the nearest ranges below and above it. That tells a read of a
// freed object (its range was unbound before the fault) from a read past the end of a live one and from an address no
// object held. vk_vaddr_dump then writes the whole history to vaddr-history.txt beside the log, for the other pages
// the kernel log names. Off by default: each bind and unbind costs a callback and a locked append. The stack walks of
// 2 take the unwinder's global lock thousands of times in an area load, enough to hide a timing-dependent fault (two
// runs that faulted without the history ran clean with stacks), so 1 leaves them out.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define VA_MASK 0xffffffffffffull   // reported addresses are sign-extended; the kernel and the history use 48 bits
#define STACK_MIN (1ull << 20)
#define FRAMES 40   // the driver's and VMA's own frames come first; the runtime's caller sits about 15 deep

typedef struct {
    uint64_t base, size, handle, ns, issued, done;
    uint32_t type;   // VkObjectType
    uint32_t tid;    // the thread that bound or unbound (plat_thread_id)
    uint8_t unbind, internal;
    int32_t stack;   // index into V.stacks, -1: none
} Report;

typedef struct { uintptr_t pc[FRAMES]; int n; } Stack;

static struct {
    Report *r;
    size_t n, cap;
    Stack *stacks;
    size_t nstacks, cap_stacks;
    VkDebugUtilsMessengerEXT messenger;
    uint64_t t0;
    int on, want_stacks;
} V;
static PlatMutex va_lock = PLAT_MUTEX_INIT;

int vk_vaddr_wanted(void) { return cfg_int("NX2NAVI_VA_REPORT") > 0; }

static void *grow(void *p, size_t *cap, size_t elem) {
    size_t c = *cap ? *cap * 2 : 4096;
    void *q = realloc(p, c * elem);
    if (!q) rt_fatal("vk_vaddr: out of memory for %zu reports", c);
    *cap = c;
    return q;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL report_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                VkDebugUtilsMessageTypeFlagsEXT type,
                                                const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)sev, (void)user;
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT)) return VK_FALSE;
    const VkDeviceAddressBindingCallbackDataEXT *b = NULL;
    for (const VkBaseInStructure *s = d->pNext; s && !b; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) b = (const void *)s;
    if (!b) return VK_FALSE;
    Report e = {.base = b->baseAddress & VA_MASK, .size = b->size, .ns = plat_now_ns(), .issued = vk_sync_issued(),
                .done = vk_sync_fence_done(), .unbind = b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT,
                .internal = (b->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0, .stack = -1,
                .tid = plat_thread_id()};
    if (d->objectCount) e.handle = d->pObjects[0].objectHandle, e.type = (uint32_t)d->pObjects[0].objectType;
    Stack st = {.n = 0};
    if (V.want_stacks && e.size >= STACK_MIN) st.n = plat_stack(st.pc, FRAMES);
    plat_mutex_lock(&va_lock);
    if (st.n) {
        if (V.nstacks == V.cap_stacks) V.stacks = grow(V.stacks, &V.cap_stacks, sizeof *V.stacks);
        e.stack = (int32_t)V.nstacks;
        V.stacks[V.nstacks++] = st;
    }
    if (V.n == V.cap) V.r = grow(V.r, &V.cap, sizeof *V.r);
    V.r[V.n++] = e;
    plat_mutex_unlock(&va_lock);
    return VK_FALSE;
}

void vk_vaddr_start(VkInstance instance) {
    if (!vk_vaddr_wanted()) return;
    PFN_vkCreateDebugUtilsMessengerEXT f =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mi = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
        .pfnUserCallback = report_cb};
    if (!f || f(instance, &mi, NULL, &V.messenger) != VK_SUCCESS) {
        rt_log("vk_vaddr: NX2NAVI_VA_REPORT set but no debug-utils messenger (VK_EXT_debug_utils missing)");
        V.messenger = VK_NULL_HANDLE;
        return;
    }
    V.t0 = plat_now_ns();
    V.want_stacks = cfg_int("NX2NAVI_VA_REPORT") >= 2;
}

void vk_vaddr_device(int supported) {
    V.on = V.messenger && supported;
    rt_log("vk_vaddr: GPU address history %s%s",
           V.on ? "on (VK_EXT_device_address_binding_report): a lost device's report names the objects that held the "
                  "faulting address"
                : "off: the driver lacks VK_EXT_device_address_binding_report",
           V.on ? (V.want_stacks ? "; call stacks for ranges of 1 MiB or more" : "; no call stacks (NX2NAVI_VA_REPORT=2)")
                : "");
}

static const char *type_name(uint32_t t) {
    switch (t) {
    case VK_OBJECT_TYPE_DEVICE: return "driver";   // the driver's own allocations (RADV names the device)
    case VK_OBJECT_TYPE_COMMAND_BUFFER: return "command buffer";
    case VK_OBJECT_TYPE_DEVICE_MEMORY: return "memory";
    case VK_OBJECT_TYPE_BUFFER: return "buffer";
    case VK_OBJECT_TYPE_IMAGE: return "image";
    case VK_OBJECT_TYPE_QUERY_POOL: return "query pool";
    case VK_OBJECT_TYPE_PIPELINE: return "pipeline";
    case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return "descriptor pool";
    default: return "object";
    }
}

static void log_report(const Report *e, uint64_t now) {
    rt_log("vk_vaddr:   %s %s %#llx (type %u%s): %#llx+%#llx (%.2f MiB), %.3f s before, timeline issued %llu done %llu, "
           "thread %u", e->unbind ? "unbind" : "bind", type_name(e->type), (unsigned long long)e->handle, e->type,
           e->internal ? ", internal" : "", (unsigned long long)e->base, (unsigned long long)e->size,
           e->size / 1048576.0, (now - e->ns) / 1e9, (unsigned long long)e->issued, (unsigned long long)e->done, e->tid);
    if (e->stack < 0) return;
    const Stack *s = &V.stacks[e->stack];
    for (int i = 0; i < s->n; i++) {
        char mod[128];
        uintptr_t base = 0;
        if (plat_module_of(s->pc[i], mod, sizeof mod, &base))
            rt_log("vk_vaddr:     #%d %s+%#llx", i, mod, (unsigned long long)(s->pc[i] - base));
        else
            rt_log("vk_vaddr:     #%d %#llx", i, (unsigned long long)s->pc[i]);
    }
}

void vk_vaddr_fault(uint64_t addr, uint64_t precision) {
    if (!V.on) return;
    uint64_t prec = precision ? precision : 1, lo = (addr & VA_MASK) & ~(prec - 1), hi = lo + prec, now = plat_now_ns();
    plat_mutex_lock(&va_lock);
    rt_log("vk_vaddr: fault range %#llx..%#llx; timeline issued %llu done %llu; %zu address reports kept",
           (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)vk_sync_issued(),
           (unsigned long long)vk_sync_fence_done(), V.n);
    int shown = 0;
    for (size_t i = V.n; i-- > 0 && shown < 8;) {
        const Report *e = &V.r[i];
        if (e->base < hi && lo < e->base + e->size) {
            if (!shown++) rt_log("vk_vaddr: reports whose range covers it, newest first:");
            log_report(e, now);
        }
    }
    if (!shown) {   // never held: the newest report of the nearest range on each side
        const Report *below = NULL, *above = NULL;
        for (size_t i = 0; i < V.n; i++) {
            const Report *e = &V.r[i];
            if (e->base + e->size <= lo && (!below || e->base + e->size >= below->base + below->size)) below = e;
            if (e->base >= hi && (!above || e->base <= above->base)) above = e;
        }
        rt_log("vk_vaddr: no reported range ever covered it; the nearest below%s and above%s:",
               below ? "" : " (none)", above ? "" : " (none)");
        if (below) {
            rt_log("vk_vaddr:   below, ending %#llx bytes short of it:", (unsigned long long)(lo - (below->base + below->size)));
            log_report(below, now);
        }
        if (above) log_report(above, now);
    }
    plat_mutex_unlock(&va_lock);
}

void vk_vaddr_dump(void) {
    if (!V.on) return;
    char path[PLAT_PATH_MAX];
    const char *log = rt_log_path(), *cut = log ? strrchr(log, '/') : NULL, *cut2 = log ? strrchr(log, '\\') : NULL;
    if (cut2 > cut) cut = cut2;
    snprintf(path, sizeof path, "%.*svaddr-history.txt", cut ? (int)(cut - log + 1) : 0, cut ? log : "");
    FILE *f = fopen(path, "w");
    if (!f) { rt_log("vk_vaddr: cannot write %s", path); return; }
    plat_mutex_lock(&va_lock);
    fprintf(f, "# seconds kind type handle base size issued done internal stack thread\n");
    for (size_t i = 0; i < V.n; i++) {
        const Report *e = &V.r[i];
        fprintf(f, "%.6f %s %u %#llx %#llx %#llx %llu %llu %u %d %u\n", (e->ns - V.t0) / 1e9, e->unbind ? "unbind" : "bind",
                e->type, (unsigned long long)e->handle, (unsigned long long)e->base, (unsigned long long)e->size,
                (unsigned long long)e->issued, (unsigned long long)e->done, e->internal, e->stack, e->tid);
    }
    for (size_t i = 0; i < V.nstacks; i++) {
        fprintf(f, "stack %zu:", i);
        for (int j = 0; j < V.stacks[i].n; j++) {
            char mod[128];
            uintptr_t base = 0;
            if (plat_module_of(V.stacks[i].pc[j], mod, sizeof mod, &base))
                fprintf(f, " %s+%#llx", mod, (unsigned long long)(V.stacks[i].pc[j] - base));
            else
                fprintf(f, " %#llx", (unsigned long long)V.stacks[i].pc[j]);
        }
        fputc('\n', f);
    }
    size_t n = V.n;
    plat_mutex_unlock(&va_lock);
    fclose(f);
    rt_log("vk_vaddr: %zu address reports written to %s", n, path);
}

void vk_vaddr_stop(VkInstance instance) {
    if (V.messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f) f(instance, V.messenger, NULL);
        size_t binds = 0;
        for (size_t i = 0; i < V.n; i++) binds += !V.r[i].unbind;
        rt_log("vk_vaddr: %zu address reports (%zu binds, %zu unbinds, %zu with a call stack)", V.n, binds, V.n - binds,
               V.nstacks);
    }
    free(V.r);
    free(V.stacks);
    memset(&V, 0, sizeof V);
}
