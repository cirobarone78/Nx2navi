"""Test production SMAA sequence recognition without game data or a GPU."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_exec.c').read_text()
start = source.index('static int scene_record(')
end = source.index('// Ops that only store state', start)
harness = r'''
#include <assert.h>
#include <stdio.h>
#include "nvn_api.h"
typedef struct VkExec { int unused; } VkExec;
#define MOD_MAIN_BASE 0x10000000ull
#define rt_game_version test_game_version
static int rt_game_version = 111;
static int wanted = 1, succeeds = 1, calls;
static int vk_dlaa_scene_wanted(void) { return wanted; }
static int vk_dlaa_scene_resolve(VkExec *ex, uint64_t src, uint64_t dst) {
    (void)ex; assert(src == 7 && dst == 9); ++calls; return succeeds;
}
'''
harness += source[start:end]
walk_start = source.index('static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth) {')
skip_start = source.index('        if (!scene_end && h->op == OP_CLEAR_TEX)', walk_start)
skip_close = '            continue;\n        }\n'
skip_end = source.index(skip_close, skip_start) + len(skip_close)
harness += '\nstatic unsigned replay_clears, replay_dispatches, replay_binds;\n'
harness += 'static void replay(VkExec *ex, const op_hdr *h) { const op_hdr *scene_end = NULL; for (;;) {\n'
harness += source[skip_start:skip_end]
harness += r'''
    if (h->op == OP_END) return;
    replay_clears += h->op == OP_CLEAR_TEX;
    replay_dispatches += h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND;
    replay_binds += h->op == OP_BIND_TEX || h->op == OP_BIND_IMG;
    h = nvn_op_next(h);
} }
static union { uint64_t align; char data[8192]; } storage;
static size_t used;
static op_hdr *add(unsigned op, const void *payload, size_t n) {
    op_hdr *h = (op_hdr *)(storage.data + used);
    h->op = op; h->size = (sizeof(*h) + n + 7) & ~(size_t)7;
    if (n) memcpy(h + 1, payload, n);
    used += h->size;
    assert(used < sizeof storage.data);
    return h;
}
int main(void) {
    tex_t edge = {0}, blend = {0};
    edge.d.format = NVN_FORMAT_RG16UI; blend.d.format = NVN_FORMAT_RGBA8;
    edge.d.width = blend.d.width = 1280; edge.d.height = blend.d.height = 720;
    p_clear_tex clear = {0}; clear.tex = &edge; clear.mask = 15;
    clear.r.width = 1280; clear.r.height = 720; clear.r.depth = 1;
    op_hdr *first = add(OP_CLEAR_TEX, &clear, sizeof clear);
    clear.tex = &blend;
    add(OP_CLEAR_TEX, &clear, sizeof clear);
    p_bind_handles bind = {0}; bind.stage = NVN_SHADER_STAGE_COMPUTE;
    bind.first = 3; bind.n = 1; bind.h[0] = 7;
    op_hdr *source = add(OP_BIND_TEX, &bind, offsetof(p_bind_handles, h) + 8);
    p_dispatch dispatch = {0}; dispatch.caller = MOD_MAIN_BASE + 0x14e3a4c;
    op_hdr *first_dispatch = add(OP_DISPATCH, &dispatch, sizeof dispatch);
    p_dispatch_ind ind = {0}; ind.caller = MOD_MAIN_BASE + 0x14e3a94;
    add(OP_DISPATCH_IND, &ind, sizeof ind);
    dispatch.caller = MOD_MAIN_BASE + 0x14e3b24;
    add(OP_DISPATCH, &dispatch, sizeof dispatch);
    bind.first = 1; bind.h[0] = 9;
    add(OP_BIND_IMG, &bind, offsetof(p_bind_handles, h) + 8);
    dispatch.caller = MOD_MAIN_BASE + 0x251bc0;
    op_hdr *last = add(OP_DISPATCH, &dispatch, sizeof dispatch);
    add(OP_END, NULL, 0);
    VkExec ex = {0};
    assert(scene_scaler(&ex, first) == last && calls == 1);
    succeeds = 0; assert(!scene_scaler(&ex, first) && calls == 2);
    succeeds = 1; wanted = 0; assert(!scene_scaler(&ex, first) && calls == 2);
    wanted = 1; rt_game_version = 143; assert(!scene_scaler(&ex, first) && calls == 2);
    rt_game_version = 111;
    ((p_clear_tex *)(first + 1))->r.width = 640;
    assert(!scene_scaler(&ex, first) && calls == 2);
    ((p_clear_tex *)(first + 1))->r.width = 1280;
    ((p_dispatch *)(first_dispatch + 1))->caller++;
    assert(!scene_scaler(&ex, first) && calls == 2);
    ((p_dispatch *)(first_dispatch + 1))->caller--;
    unsigned saved = source->size; source->size = sizeof(*source);
    assert(!scene_scaler(&ex, first) && calls == 2);
    source->size = saved;
    ((p_bind_handles *)(source + 1))->n = 2; // truncated handle array
    assert(!scene_scaler(&ex, first) && calls == 2);
    ((p_bind_handles *)(source + 1))->n = 1;
    last->op = OP_END;
    assert(!scene_scaler(&ex, first) && calls == 2);
    last->op = OP_DISPATCH;
    assert(scene_scaler(&ex, first) == last && calls == 3);
    replay(&ex, first);
    assert(replay_clears == 0 && replay_dispatches == 0 && replay_binds == 2);
    succeeds = 0;
    replay_clears = replay_dispatches = replay_binds = 0;
    replay(&ex, first);
    assert(replay_clears == 2 && replay_dispatches == 4 && replay_binds == 2);
    puts("SMAA sequence checks passed");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / 'scene.c'
    exe = Path(temp) / 'scene.exe'
    c.write_text(harness)
    subprocess.run([args.cc, '-std=c11', '-I', str(root / 'runtime'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
