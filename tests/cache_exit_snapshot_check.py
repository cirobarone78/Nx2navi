"""Windows: exercise exit snapshots during builds, growing cache data, and the conservative other-driver path."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_pipeline.c').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef SRWLOCK PlatMutex;
typedef void *VkPipelineCache;
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
typedef struct { struct { unsigned vendorID; } props; } VkCtx;
static VkCtx context = {{0x8086}};
static VkCtx *vk_ctx(void) { return &context; }
static size_t mock_size = 4;
static int grow_once, grow_always, query_fail;
static PlatMutex cache_rw = SRWLOCK_INIT;
static volatile int32_t cache_saving_at_exit;
static volatile LONG active, snapshots;
static HANDLE old_in, release_old, new_in, file_in, release_file;
static int block_file, fail_file, writes, save_result;
static struct {
    VkPipelineCache cache, play;
    int device, fold, feedback;
    const char *cache_path, *play_path;
    uint64_t saved[2], cache_loaded, play_loaded;
} P;
static void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared(m); }
static void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared(m); }
static int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive(m); }
static int plat_mutex_trylock_shared(PlatMutex *m) { return TryAcquireSRWLockShared(m); }
static void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(m); }
static void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
static uint64_t plat_now_ms(void) { return GetTickCount64(); }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static void fb_log(const char *when) { (void)when; assert(!cache_saving_at_exit); }
static void plat_file_delete(const char *path) { (void)path; }
static int plat_file_write_atomic(const char *path, const void *data, size_t size) {
    (void)path;
    assert(!cache_saving_at_exit); // admission must reopen before IO and diagnostics
    assert(size == mock_size && !memcmp(data, "test", 4));
    writes++;
    if (block_file) {
        SetEvent(file_in);
        assert(WaitForSingleObject(release_file, 2000) == WAIT_OBJECT_0);
    }
    return !fail_file;
}
static int vkGetPipelineCacheData(int device, VkPipelineCache cache, size_t *size, void *data) {
    (void)device; (void)cache;
    if (context.props.vendorID != 0x10de) assert(!active);
    InterlockedIncrement(&snapshots);
    if (!data && query_fail) return -1;
    if (data && (grow_once || grow_always)) {
        grow_once = 0; mock_size = *size + 100; return VK_INCOMPLETE;
    }
    if (data) { assert(*size >= mock_size); memcpy(data, "test", 4); }
    *size = mock_size;
    return VK_SUCCESS;
}
'''
harness += function('static void cache_build_enter(void)') + '\n'
harness += function('static int cache_save_one(int which, uint32_t wait_ms, int exiting)') + '\n'
harness += r'''
static DWORD WINAPI builder(void *old) {
    cache_build_enter();
    InterlockedIncrement(&active);
    SetEvent(old ? old_in : new_in);
    if (old) assert(WaitForSingleObject(release_old, 2000) == WAIT_OBJECT_0);
    InterlockedDecrement(&active);
    plat_mutex_unlock_shared(&cache_rw);
    return 0;
}
static DWORD WINAPI saver(void *unused) {
    (void)unused;
    save_result = cache_save_one(0, 2000, 1);
    return 0;
}
static void join(HANDLE t) { assert(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0); CloseHandle(t); }
int main(void) {
    P.cache = (void *)1; P.cache_path = "unused"; P.play_path = "unused-play"; P.feedback = 1;
    old_in = CreateEventW(NULL, TRUE, FALSE, NULL);
    release_old = CreateEventW(NULL, TRUE, FALSE, NULL);
    new_in = CreateEventW(NULL, TRUE, FALSE, NULL);
    file_in = CreateEventW(NULL, TRUE, FALSE, NULL);
    release_file = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE old = CreateThread(NULL, 0, builder, (void *)1, 0, NULL);
    assert(WaitForSingleObject(old_in, 2000) == WAIT_OBJECT_0);
    block_file = 1;
    HANDLE saving = CreateThread(NULL, 0, saver, NULL, 0, NULL);
    uint64_t until = GetTickCount64() + 2000;
    while (!__atomic_load_n(&cache_saving_at_exit, __ATOMIC_ACQUIRE)) {
        assert(GetTickCount64() < until); Sleep(1);
    }
    HANDLE next = CreateThread(NULL, 0, builder, NULL, 0, NULL);
    assert(WaitForSingleObject(new_in, 20) == WAIT_TIMEOUT);
    SetEvent(release_old);
    assert(WaitForSingleObject(file_in, 2000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(new_in, 2000) == WAIT_OBJECT_0); // IO does not hold admission closed
    SetEvent(release_file);
    join(old); join(next); join(saving);
    assert(save_result == 1 && snapshots == 2 && writes == 1 && !cache_saving_at_exit);

    // Ordinary autosave does not wait behind an active build or close admission.
    cache_build_enter();
    assert(cache_save_one(0, 0, 0) == 0 && !cache_saving_at_exit);
    // A final save times out on a genuinely long driver call and restores admission.
    assert(cache_save_one(0, 1, 1) == 0 && !cache_saving_at_exit);
    plat_mutex_unlock_shared(&cache_rw);
    cache_build_enter(); plat_mutex_unlock_shared(&cache_rw);

    block_file = 0;
    assert(cache_save_one(0, 100, 1) == 1 && writes == 1); // unchanged data is not rewritten
    P.saved[0] = 0; fail_file = 1;
    assert(cache_save_one(0, 100, 1) == -1 && !cache_saving_at_exit && P.saved[0] == 0);
    // NVIDIA exit reads can finish while a build remains active; normal autosaves retain the conservative path.
    fail_file=0;context.props.vendorID=0x10de;P.saved[0]=0;
    ResetEvent(old_in);ResetEvent(release_old);
    old=CreateThread(NULL,0,builder,(void *)1,0,NULL);
    assert(WaitForSingleObject(old_in,2000)==WAIT_OBJECT_0);
    assert(active==1);
    int before=writes;
    assert(cache_save_one(0,100,1)==1 && writes==before+1 && active==1 && !cache_saving_at_exit);
    assert(cache_save_one(0,0,0)==0 && active==1); // no gameplay autosave behavior change
    SetEvent(release_old);join(old);

    // Cache growth retries a fresh size query, and a partial result never replaces an existing file.
    P.saved[0]=0;grow_once=1;
    assert(cache_save_one(0,100,1)==1 && P.saved[0]==mock_size);
    before=writes;
    assert(cache_save_one(0,100,1)==1 && writes==before);
    uint64_t saved=P.saved[0];mock_size++;grow_always=1;
    assert(cache_save_one(0,100,1)==-1 && writes==before && P.saved[0]==saved);
    grow_always=0;query_fail=1;
    assert(cache_save_one(0,100,1)==-1 && writes==before && P.saved[0]==saved);
    query_fail=0;

    // A merge owns exclusive access: even the concurrent snapshot must not race it.
    assert(plat_mutex_trylock(&cache_rw));
    assert(cache_save_one(0,0,1)==0 && writes==before);
    plat_mutex_unlock(&cache_rw);
    puts("Exit snapshot: active builds, merge exclusion, cache growth/retry, partial/error preservation and fallback passed");
    return 0;
}
'''
out = root / 'build/test/cache-exit-snapshot'
out.mkdir(parents=True, exist_ok=True)
(out / 'check.c').write_text(harness)
cc = sys.argv[1] if len(sys.argv) > 1 else 'clang'
subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(out/'check.c'),
                '-o', str(out/'check.exe')], check=True)
subprocess.run([str(out/'check.exe')], check=True, timeout=15)
