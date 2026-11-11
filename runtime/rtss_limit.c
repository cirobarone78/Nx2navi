// RivaTuner Statistics Server as the frame limiter (frame_rate.c frame_step_cap_ns): where RTSS is installed and running,
// the frame limit goes into RTSS's profile for this executable and the game's own pacer holds no frames, so there is
// one limiter. Anywhere else, or when any step fails, the game's own limiter works as it always did.
//
// RTSS's hooks library (RTSSHooks64.dll in the folder its installer records under
// HKLM\SOFTWARE\WOW6432Node\Unwinder\RTSS, InstallDir) exports the profile calls, loaded here at run time:
// LoadProfile(name) reads a profile (the global one for a name without its own), Get/SetProfileProperty read and change
// the loaded copy, SaveProfile(name) writes it to Profiles\<name>.cfg, UpdateProfiles makes every hooked process
// read its profile again, EnumProfiles lists the profiles that exist (profile_exists says why it is not called),
// DeleteProfile removes one. "FramerateLimit" is the limit in frames per second (0: none); "FramerateLimitDenominator"
// divides it, so 59940 over 1000 limits to 59.94 (a monitor's measured rate). SaveProfile reports nothing: RTSS's
// Profiles folder sits under Program Files, which a process without administrator rights cannot write, and the save
// then fails silently. So every change is read back
// from the saved profile, and a limit that did not stick leaves the game's own limiter in charge for the session.
//
// RTSS runs when its shared memory "RTSSSharedMemoryV2" carries the signature 'RTSS'; it limits this process once the
// memory's application list holds this process id (RTSS hooks a process some seconds after it starts, its injection
// delay). Until then, and again if RTSS quits, the game's own limiter paces the frames. While RTSS limits, the frame
// rate RTSS itself measures for the game is checked against the limit: well above it for three seconds, and the game
// takes the limiting back for the rest of the run.
//
// The calls run on a thread of their own, every half second, so a present never waits on profile files. On exit
// (rtss_limit_restore, from every path that ends the process) the profile gets back what it held before the game
// changed it: the old limit, or no profile of its own when there was none. A crash before that leaves the game's
// limit in the profile until the next run changes it.
//
// NX2NAVI_RTSS=0 keeps RTSS out of it. Frame generation keeps it out too: RTSS counts every presented frame, the
// generated ones included, and would halve the limit.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>

typedef DWORD (*EnumProfilesFn)(LPSTR list, DWORD size);
typedef void (*ProfileFn)(LPCSTR name);
typedef BOOL (*PropertyFn)(LPCSTR property, LPBYTE data, DWORD size);
typedef void (*UpdateFn)(void);

#define RTSS_SIGNATURE 0x52545353u   // 'RTSS' as RTSS writes it: the memory holds valid data
#define POLL_MS 500
#define CHECK_POLLS 6     // polls in a row RTSS's own frame rate of the game may stay above the limit
#define CHECK_SLACK 1.2   // above limit * this, RTSS is taken not to hold the game to it

enum { FAIL_NONE, FAIL_LIBRARY, FAIL_READONLY, FAIL_NOT_LIMITING };

static PlatMutex lock = PLAT_MUTEX_INIT;   // the hooks calls and the state below
static _Atomic int64_t want = -2;          // millihertz from the pacer: >0 a limit, 0 none, -1 RTSS kept out; -2 unset
static _Atomic int active;                 // RTSS limits this process now
static _Atomic int failed;                 // FAIL_*: RTSS is out of it for the rest of the run
static int started;
static char dll_path[MAX_PATH], profile[MAX_PATH], rtss_dir[MAX_PATH];
static HMODULE dll;
static EnumProfilesFn enum_profiles;
static ProfileFn load_profile, save_profile, delete_profile;
static PropertyFn get_property, set_property;
static UpdateFn update_profiles;
static int has_denominator;   // this RTSS knows FramerateLimitDenominator
static int64_t applied = -1;  // the millihertz the profile holds from this run; -1 untouched
static int64_t refused = -1;  // a limit RTSS refused (out of its range): the game's own limiter keeps that one
static int changed;           // the profile holds a value from this run: put the old one back on exit
static int had_profile;       // the profile existed before this run changed it
static DWORD old_limit, old_denominator = 1;

// RTSS's shared memory: running (1), whether its application list holds this process, and the frame rate RTSS measured
// for it over its last period (0 unknown). The header: signature, version, application entry size, offset of the
// application array, its length. An entry: the process id, the executable path (MAX_PATH), flags, then the period's
// start and end in ms and the frames in it.
static int rtss_running(int *hooked, double *fps) {
    *hooked = 0, *fps = 0;
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!m) return 0;
    const uint8_t *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    int run = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (p && VirtualQuery(p, &mi, sizeof mi) && mi.RegionSize >= 20) {
        DWORD h[5];
        memcpy(h, p, sizeof h);
        run = h[0] == RTSS_SIGNATURE && h[1] >= 0x20000;
        DWORD pid = GetCurrentProcessId();
        enum { PERIOD_AT = 4 + MAX_PATH + 4, ENTRY_MIN = PERIOD_AT + 12 };
        for (DWORD i = 0; run && h[2] >= ENTRY_MIN && i < h[4] && i < 4096; i++) {
            uint64_t at = (uint64_t)h[3] + (uint64_t)i * h[2];
            if (at + ENTRY_MIN > mi.RegionSize) break;
            DWORD id, t[3];
            memcpy(&id, p + at, 4);
            if (id != pid) continue;
            *hooked = 1;
            memcpy(t, p + at + PERIOD_AT, sizeof t);   // start, end, frames
            if (t[1] > t[0]) *fps = 1000.0 * t[2] / (t[1] - t[0]);
            break;
        }
    }
    if (p) UnmapViewOfFile(p);
    CloseHandle(m);
    return run;
}

static int load_hooks(void) {
    if (dll) return 1;
    dll = LoadLibraryA(dll_path);
    if (!dll) {
        rt_log("frame rate: RivaTuner Statistics Server's %s did not load (%lu)", dll_path, GetLastError());
        return 0;
    }
    enum_profiles = (EnumProfilesFn)(void *)GetProcAddress(dll, "EnumProfiles");
    load_profile = (ProfileFn)(void *)GetProcAddress(dll, "LoadProfile");
    save_profile = (ProfileFn)(void *)GetProcAddress(dll, "SaveProfile");
    delete_profile = (ProfileFn)(void *)GetProcAddress(dll, "DeleteProfile");
    get_property = (PropertyFn)(void *)GetProcAddress(dll, "GetProfileProperty");
    set_property = (PropertyFn)(void *)GetProcAddress(dll, "SetProfileProperty");
    update_profiles = (UpdateFn)(void *)GetProcAddress(dll, "UpdateProfiles");
    if (enum_profiles && load_profile && save_profile && delete_profile && get_property && set_property && update_profiles)
        return 1;
    rt_log("frame rate: RivaTuner Statistics Server's hooks library lacks the profile calls");
    FreeLibrary(dll);
    dll = NULL;
    return 0;
}

// The profile exists on its own: RTSS keeps it as Profiles\<name>.cfg in its folder. EnumProfiles would say the same
// (a comma-separated list of names) but crashes this process: it walks Profiles\*.cfg with _findfirst and _findnext and
// keeps the find handle in 32 bits. The handle points into the process heap, which here lies above 4 GB (the guest
// address space takes the low part, memory.c mem_init), so _findnext gets a cut pointer and faults in ntdll writing to
// it. NX2NAVI_RTSS_ENUM=1 calls EnumProfiles anyway, to test an RTSS that keeps the whole handle.
static int profile_exists(void) {
    if (!rt_knob("RTSS_ENUM", "0")) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\Profiles\\%s.cfg", rtss_dir, profile);
        return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    char list[8192];
    DWORD need = enum_profiles(list, sizeof list);
    list[sizeof list - 1] = 0;
    if (need > sizeof list)
        rt_log("frame rate: RivaTuner Statistics Server lists %lu bytes of profiles; reading the first %zu", need, sizeof list);
    size_t n = strlen(profile);
    int found = 0;
    for (char *s = list; *s;) {
        char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len == n && !_strnicmp(s, profile, n)) { found = 1; break; }
        if (!end) break;
        s = end + 1;
    }
    free(list);
    return found;
}

// Writes limit/denominator into the profile and reads them back from the saved file: 1 when they stuck, 0 when RTSS
// refused a value, -1 when the save did not reach the file. RTSS checks each value against the other one already in
// the profile (the limit over the denominator must be a rate it accepts: 165080 is refused over 1 and taken over 1000),
// so the limit goes to 0 (none) before the denominator changes.
static int write_limit(DWORD limit, DWORD denominator) {
    load_profile(profile);
    DWORD none = 0;
    if (!set_property("FramerateLimit", (LPBYTE)&none, sizeof none)) return 0;
    if (has_denominator && !set_property("FramerateLimitDenominator", (LPBYTE)&denominator, sizeof denominator)) return 0;
    if (limit && !set_property("FramerateLimit", (LPBYTE)&limit, sizeof limit)) return 0;
    save_profile(profile);
    load_profile(profile);
    DWORD l = ~0u, d = 1;
    if (!get_property("FramerateLimit", (LPBYTE)&l, sizeof l)) return -1;
    if (has_denominator && !get_property("FramerateLimitDenominator", (LPBYTE)&d, sizeof d)) return -1;
    return l == limit && (!has_denominator || d == denominator) ? 1 : -1;
}

// 1: the profile holds mhz; 0: RTSS refused it (the profile then holds no limit of this run); -1: the profile cannot be
// written.
static int apply(int64_t mhz) {
    if (!changed) {   // what to put back on exit
        had_profile = profile_exists();
        load_profile(profile);
        old_limit = 0, old_denominator = 1;
        get_property("FramerateLimit", (LPBYTE)&old_limit, sizeof old_limit);
        has_denominator = get_property("FramerateLimitDenominator", (LPBYTE)&old_denominator, sizeof old_denominator);
        if (!old_denominator) old_denominator = 1;
    }
    DWORD limit, denominator = 1;
    if (mhz % 1000 == 0) limit = (DWORD)(mhz / 1000);
    else if (has_denominator) limit = (DWORD)mhz, denominator = 1000;
    else limit = (DWORD)((mhz + 500) / 1000);
    int r = write_limit(limit, denominator);
    if (r < 0) {
        rt_log("frame rate: RivaTuner Statistics Server's profile %s did not keep the limit: its Profiles folder needs the "
               "game run as administrator", profile);
        return -1;
    }
    if (!changed) {
        if (!had_profile)
            rt_log("frame rate: RivaTuner Statistics Server profile %s created; it goes away when the game closes", profile);
        else
            rt_log("frame rate: RivaTuner Statistics Server profile %s held a limit of %lu/%lu; it gets that back when the "
                   "game closes", profile, old_limit, old_denominator);
        changed = 1;
    }
    if (r == 0) {   // RTSS must not keep an earlier limit of this run under the game's own one
        rt_log("frame rate: RivaTuner Statistics Server refused a limit of %.3f fps", mhz / 1000.0);
        if (applied != 0 && write_limit(0, 1) > 0) applied = 0, update_profiles();
        refused = mhz;
        return 0;
    }
    applied = mhz;
    update_profiles();
    if (mhz) rt_log("frame rate: RivaTuner Statistics Server's profile %s limits to %.3f fps", profile, mhz / 1000.0);
    else rt_log("frame rate: RivaTuner Statistics Server's profile %s has no limit (unlimited)", profile);
    return 1;
}

static void put_back(void) {
    if (!changed) return;
    changed = 0;
    applied = -1;
    if (!had_profile) {
        delete_profile(profile);
        rt_log("frame rate: RivaTuner Statistics Server profile %s removed, as before the game started", profile);
    } else if (write_limit(old_limit, old_denominator) > 0) {
        rt_log("frame rate: RivaTuner Statistics Server profile %s has its limit of %lu/%lu back", profile, old_limit,
               old_denominator);
    } else {
        rt_log("frame rate: RivaTuner Statistics Server profile %s did not take its old limit back", profile);
    }
    update_profiles();
}

// Logs which limiter is in use: the first decision, then each change.
static void set_active(int on, const char *why) {
    static int logged = -1;
    atomic_store(&active, on);
    if (logged == on) return;
    logged = on;
    if (on) rt_log("frame rate: RivaTuner Statistics Server does the limiting; the game's own limiter stands aside");
    else rt_log("frame rate: the game's own limiter (%s)", why);
}

static const char *fail_reason(int f) {
    return f == FAIL_READONLY       ? "RivaTuner Statistics Server's profile cannot be changed"
           : f == FAIL_NOT_LIMITING ? "RivaTuner Statistics Server does not hold the game to the limit"
                                    : "RivaTuner Statistics Server's hooks library did not load";
}

static void worker(void *arg) {
    (void)arg;
    int was_running = -1, was_hooked = 0, over = 0;
    for (;; plat_sleep_ms(POLL_MS)) {
        int64_t w = atomic_load(&want);
        if (w == -2) continue;
        plat_mutex_lock(&lock);
        int hooked;
        double fps;
        int run = rtss_running(&hooked, &fps), f = atomic_load(&failed);
        if (run != was_running || hooked != was_hooked) {
            if (!run) rt_log("frame rate: RivaTuner Statistics Server is installed but not running");
            else if (!hooked) rt_log("frame rate: RivaTuner Statistics Server runs; it has not hooked the game yet");
            else rt_log("frame rate: RivaTuner Statistics Server has hooked the game");
            was_running = run, was_hooked = hooked;
        }
        if (w < 0) {
            if (dll) put_back();
            set_active(0, "RivaTuner Statistics Server kept out: NX2NAVI_RTSS=0 or frame generation");
        } else if (f) {
            set_active(0, fail_reason(f));
        } else if (!run) {
            set_active(0, "RivaTuner Statistics Server is not running");
        } else if (!load_hooks()) {
            atomic_store(&failed, FAIL_LIBRARY);
            set_active(0, fail_reason(FAIL_LIBRARY));
        } else if (w == refused) {
            set_active(0, "RivaTuner Statistics Server refused this limit");
        } else if (w != applied) {
            int r = apply(w);
            over = 0;
            if (r < 0) atomic_store(&failed, FAIL_READONLY), set_active(0, fail_reason(FAIL_READONLY));
            else if (r == 0) set_active(0, "RivaTuner Statistics Server refused this limit");
            else if (atomic_load(&want) == w) set_active(hooked, "RivaTuner Statistics Server has not hooked the game yet");
        } else if (atomic_load(&want) == w) {   // a newer limit waits for the next poll (rtss_limit_want)
            set_active(hooked, "RivaTuner Statistics Server has not hooked the game yet");
            // RTSS's own count of the game's frames while the game's limiter stands aside: well above the limit for
            // three seconds means RTSS does not hold the game to it, and the game takes the limiting back for the run
            if (!hooked || w == 0 || fps <= w / 1000.0 * CHECK_SLACK) over = 0;
            else if (++over == CHECK_POLLS) {
                rt_log("frame rate: RivaTuner Statistics Server measures %.1f fps with its limit at %.3f fps", fps,
                       w / 1000.0);
                atomic_store(&failed, FAIL_NOT_LIMITING);
                put_back();
                set_active(0, fail_reason(FAIL_NOT_LIMITING));
            }
        }
        plat_mutex_unlock(&lock);
    }
}

static void start(void) {
    started = 1;
    char dir[MAX_PATH];
    DWORD size = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", "InstallDir", RRF_RT_REG_SZ, NULL, dir,
                     &size) != ERROR_SUCCESS) {
        rt_log("frame rate: RivaTuner Statistics Server is not installed; the game's own limiter");
        return;
    }
    snprintf(dll_path, sizeof dll_path, "%s\\RTSSHooks64.dll", dir);
    snprintf(rtss_dir, sizeof rtss_dir, "%s", dir);
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        rt_log("frame rate: RivaTuner Statistics Server's folder %s has no RTSSHooks64.dll; the game's own limiter", dir);
        return;
    }
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    const char *base = exe;
    for (const char *s = exe; *s; s++)
        if (*s == '\\' || *s == '/') base = s + 1;
    snprintf(profile, sizeof profile, "%s", base);   // RTSS names a profile after the executable, as ocarina.exe
    rt_log("frame rate: RivaTuner Statistics Server found in %s; it takes the frame limit while it runs", dir);
    if (!plat_thread_spawn(worker, NULL, "rtss", 0)) rt_log("frame rate: the RivaTuner Statistics Server thread did not start");
}

void rtss_limit_want(int64_t mhz) {
    if (!started && mhz < 0) return;   // kept out from the start, for the whole run (both reasons need a restart)
    if (!started) start();
    // a new limit: the game's own limiter holds it until the worker had RTSS apply it, up to POLL_MS later (a movie's
    // cap that waited for that ran the movie's 60 Hz count fast for half a second)
    if (atomic_exchange(&want, mhz) != mhz) atomic_store(&active, 0);
}

int rtss_limit_active(void) { return atomic_load_explicit(&active, memory_order_relaxed); }

int rtss_limit_state(void) {
    int f = atomic_load_explicit(&failed, memory_order_relaxed);
    if (atomic_load(&want) >= 0 && (f == FAIL_READONLY || f == FAIL_NOT_LIMITING)) return f == FAIL_READONLY ? 2 : 3;
    return rtss_limit_active() && atomic_load(&want) > 0;
}

void rtss_limit_restore(void) {
    if (!started || !*profile) return;
    // the exit may come from a crash on any thread: give up after a second rather than wait on a lock held there
    for (int i = 0; i < 100; i++) {
        if (plat_mutex_trylock(&lock)) {
            if (dll) put_back();
            atomic_store(&want, -2);   // the worker leaves the profile alone from now on
            plat_mutex_unlock(&lock);
            return;
        }
        plat_sleep_ms(10);
    }
}

#else
void rtss_limit_want(int64_t mhz) { (void)mhz; }
int rtss_limit_active(void) { return 0; }
int rtss_limit_state(void) { return 0; }
void rtss_limit_restore(void) {}
#endif
