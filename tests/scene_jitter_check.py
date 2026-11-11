"""Test production jitter selection, copied depth and viewport transitions without a GPU."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
parser.add_argument('--include', action='append', default=[])
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_dlaa.c').read_text()
draw = (root / 'runtime/vk_exec_draw.c').read_text()
constants = '\n'.join(re.findall(r'^#define (?:OCAM_BLOCK|CAM_BLOCK) .+$', source, re.M))
harness = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "vk_exec.h"
static struct {
    VkImage jdepth, depth_copies[8];
    unsigned depth_copies_n, cam_size, jframe;
    int geometry_jitter, jit_ok, ended, jit_used, hold;
    float jx, jy, ux, uy;
} D;
static int knob = 1;
#define rt_knob(name, fallback) knob
'''
harness += constants + '\n'
a = source.index('static float halton(')
b = source.index('static int scene_color', a)
harness += source[a:b]
a = source.index('static void next_jitter(')
b = source.index('// The DLSS model', a)
harness += source[a:b]
harness += r'''
static struct { float jitter_x, jitter_y; } B;
static unsigned viewport_calls;
static VkViewport recorded;
static void capture_viewport(VkCommandBuffer cb, uint32_t count, const VkViewport *v) {
    (void)cb; assert(count == 1); recorded = *v; ++viewport_calls;
}
#undef vkCmdSetViewportWithCount
#define vkCmdSetViewportWithCount capture_viewport
'''
a = draw.index('static VkViewport draw_viewport(')
b = draw.index('    if (ex->dirty & VK_EXEC_DIRTY_SCISSOR)', a)
harness += draw[a:b] + '\n}\n'
harness += r'''
#define IMAGE(n) ((VkImage)(uintptr_t)(n))
static void expect(VkExec *ex, int mesh, int jittered) {
    float x = 99, y = 99;
    vk_dlaa_draw_jitter(ex, mesh, &x, &y);
    assert(x == (jittered ? D.jx : 0) && y == (jittered ? D.jy : 0));
}
int main(int argc, char **argv) {
    (void)argv;
    knob = argc == 1;
    D.cam_size = OCAM_BLOCK;
    next_jitter(1280, 1920, 0); assert(!D.jit_ok);
    next_jitter(1280, 1920, 1);
    if (!knob) { assert(!D.jit_ok); puts("Jitter override check passed"); return 0; }
    assert(D.jit_ok && D.geometry_jitter);
    D.hold = 1; next_jitter(1280, 1920, 1); assert(!D.jit_ok);
    D.hold = 0; D.jframe = 0; next_jitter(960, 1920, 1);
    float first_x = D.jx, first_y = D.jy;
    assert(fabsf(first_x) < 1e-6f && fabsf(first_y + 1.0f/6) < 1e-6f);
    for (int i = 0; i < 31; ++i) {
        next_jitter(960, 1920, 1);
        assert(D.jx >= -0.5f && D.jx < 0.5f && D.jy >= -0.5f && D.jy < 0.5f);
    }
    next_jitter(960, 1920, 1); assert(D.jx == first_x && D.jy == first_y);
    D.jx = 0.25f; D.jy = -0.125f; D.jdepth = IMAGE(1);
    VkExec ex = {0}; ex.pass_w = 1280; ex.pass_h = 720; ex.depth.image = IMAGE(1); ex.pipe.ds.dt = 1; ex.pipe.ds.df = NVN_DEPTH_FUNC_GREATER;
    expect(&ex, 1, 1); // mesh geometry
    expect(&ex, 0, 0); // fullscreen vertex pass sharing the scene depth
    ex.ubo[0][0].addr = 4096; ex.ubo[0][0].size = OCAM_BLOCK;
    expect(&ex, 0, 1);
    ex.pipe.ds.df = NVN_DEPTH_FUNC_ALWAYS; expect(&ex, 1, 0);
    ex.pipe.ds.df = NVN_DEPTH_FUNC_GREATER; ex.pipe.ds.dt = 0; expect(&ex, 1, 0); ex.pipe.ds.dt = 1;
    vk_dlaa_depth_copy(IMAGE(1), IMAGE(2), 1); ex.depth.image = IMAGE(2); expect(&ex, 1, 1);
    vk_dlaa_depth_copy(IMAGE(2), IMAGE(3), 1); ex.depth.image = IMAGE(3); expect(&ex, 1, 1);
    vk_dlaa_depth_copy(IMAGE(4), IMAGE(3), 0); expect(&ex, 1, 0);
    vk_dlaa_depth_copy(IMAGE(1), IMAGE(4), 0); ex.depth.image = IMAGE(4); expect(&ex, 1, 0);
    ex.depth.image = IMAGE(1); D.ended = 1; expect(&ex, 1, 0); D.ended = 0;
    record_fixed(&ex, 1); assert(viewport_calls == 1 && recorded.x == D.jx);
    record_fixed(&ex, 1); assert(viewport_calls == 1);
    ex.ubo[0][0].size = 0;
    record_fixed(&ex, 0); assert(viewport_calls == 2 && recorded.x == 0 && recorded.y == 720);
    record_fixed(&ex, 1); assert(viewport_calls == 3 && recorded.y == 720 + D.jy);
    D.ended = 1; record_fixed(&ex, 1); assert(viewport_calls == 4 && recorded.x == 0);
    puts("Scene jitter checks passed");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / 'jitter.c'
    exe = Path(temp) / 'jitter.exe'
    c.write_text(harness)
    inc = [part for directory in [str(root / 'runtime'), *args.include] for part in ['-I', directory]]
    subprocess.run([args.cc, '-std=c11', *inc, str(c), '-o', str(exe), '-lm'], check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run([str(exe), 'disabled'], check=True)
