// Drive the runtime logger and its fatal path in fresh processes. The log directory is supplied by crash_log_check.py.
#ifndef __x86_64__
#define __builtin_cpu_init() ((void)0)
#define __builtin_cpu_supports(feature) 0
#endif
#define main game_main
#include "../runtime/main.c"
#undef main
#include <sys/stat.h>

int plat_stderr_path(char *out, size_t cap) { return 0; }
int plat_exe_path(char *out, size_t cap) { return snprintf(out, cap, "./game"); }
uint64_t plat_counter_hz(void) { return 1000000000; }
void plat_file_mkdir_p(const char *path) {}   // the test creates logs/
int plat_file_exists(const char *path) { struct stat s; return !stat(path, &s); }
int plat_file_replace(const char *from, const char *to, int durable) {
    if (!plat_file_exists(from)) return 0;
    remove(to);
    return rename(from, to) == 0;
}
void plat_file_delete(const char *path) { remove(path); }
PlatAppend *plat_append_open(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f) setvbuf(f, NULL, _IONBF, 0);
    return (PlatAppend *)f;
}
int plat_append_write(PlatAppend *f, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)f) == n; }
void plat_commit_info(double *process, double *used, double *limit) { *process = *used = *limit = 0; }
void kern_free_cores_note(void) {}
void prof_sampler_write(void) {}
int rt_linked_modules(RtModule *out, int max) { return 0; }
Cpu *kern_cpu(void) { return NULL; }
const char *ldr_describe(uint64_t a, char *buf, int n) { return "guest"; }
int plat_mem_readable(uint64_t addr, uint64_t n) { return 0; }
uintptr_t plat_exe_base(void) { return 0; }
// Leave the log thread unscheduled so that all queued lines are still pending when the fatal path drains them.
const char *cfg_get(const char *key) { return getenv(key); }
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) { return 1; }
void plat_mutex_lock(PlatMutex *m) {}
int plat_mutex_trylock(PlatMutex *m) { return 1; }
void plat_mutex_unlock(PlatMutex *m) {}
void plat_cond_wait(PlatCond *c, PlatMutex *m) { abort(); }
void plat_cond_signal(PlatCond *c) {}
void plat_sleep_ms(uint32_t ms) {}
uint64_t plat_now_ns(void) { return 0; }
void (*plat_exit_hook)(void);
void rtss_limit_restore(void) {}
void plat_exit(int code) { _Exit(code); }

int main(int argc, char **argv) {
    log_open(1);
    rt_log("run %s", argv[1]);
    if (!strcmp(argv[1], "clean")) { rt_log_sync(); return 0; }
    g_log_bytes = LOG_CAP;
    rt_log("ordinary trace beyond the cap");
    if (!strcmp(argv[1], "fallback")) g_crash_file = NULL;   // the companion could not be opened
    int previous = rt_log_crash(1);
    rt_log("GPU fault breadcrumb");
    rt_log_crash(previous);
    rt_log("ordinary trace after the breadcrumb");
    if (!strcmp(argv[1], "exception")) {
        PlatCrash fault = {.what = "synthetic exception", .module = "driver", .access = -1, .offset = 42};
        crash_handler(&fault);
    }
    rt_fatal("synthetic crash");
}
