#!/usr/bin/env python3
"""Exercise the stereo frame-selection code without a Vulkan device or game."""
import os
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
parser.add_argument('--vma-include', help='directory containing vk_mem_alloc.h')
args = parser.parse_args()
CLANG = args.cc
cc_path = shutil.which(CLANG) or CLANG
vma = args.vma_include or str(Path(cc_path).resolve().parent.parent / 'include/vma')
source = ROOT / 'runtime/vk_stereo.c'
# The pre-feature fallback represents the absence of any depth selection.
code = source.read_text(encoding='utf-8') if source.exists() else ''
if '/* BEGIN STEREO SELECTION */' in code:
    code = code.split('/* BEGIN STEREO SELECTION */', 1)[1].split('/* END STEREO SELECTION */', 1)[0]
else:
    code = '''static void scene_note(VkImage d, VkImageView v, unsigned w, unsigned h, const float *p) {}
static void scene_gone(VkImage image) {}
static StereoFrame scene_take(void) { StereoFrame f = {0}; return f; }'''
harness = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uintptr_t VkImage;
typedef uintptr_t VkImageView;
typedef struct { VkImage depth; VkImageView view; uint32_t width, height; float projection[4]; } StereoFrame;
''' + code + r'''
int main(void) {
    const float p[4] = {1,2,3,4}; StereoFrame f;
    scene_note(10, 1, 11, 1280, 720, p);
    scene_copy(10, 20); scene_copy(20, 30);
    f = scene_take(99); assert(!f.depth);
    scene_note(10, 1, 11, 1280, 720, p); scene_copy(10, 20); scene_copy(20, 30);
    f = scene_take(30); assert(f.depth == 1 && f.view == 11 && f.projection[2] == 3);
    scene_note(10, 1, 11, 1280, 720, p); scene_copy(10, 20); scene_copy(99, 20);
    assert(!scene_take(20).depth);
    scene_note(10, 1, 11, 1280, 720, p); scene_copy(10, 20); scene_gone(10);
    assert(!scene_take(20).depth);
    scene_note(10, 1, 11, 1280, 720, p); scene_gone(1);
    scene_note(20, 2, 22, 1920, 1080, p); assert(!scene_take(20).depth);
    scene_note(10, 1, 11, 1280, 720, p); scene_note(20, 2, 22, 1280, 720, p);
    scene_gone(1); scene_gone(2); scene_note(30, 3, 33, 1920, 1080, p);
    assert(!scene_take(30).depth);
    scene_note(10, 1, 11, 1280, 720, p);
    for (int i = 0; i < 100; ++i) scene_copy(10, 100 + i);
    assert(!scene_take(10).depth);
    scene_note(10, 1, 11, 1280, 720, p); assert(scene_take(10).depth == 1);
    assert(!scene_take(10).depth);
    scene_note(10, 1, 11, 1280, 720, p); scene_copy(10, 20);
    scene_note(10, 1, 11, 1280, 720, p); // later geometry updates shared depth, not the old colour copy
    assert(!scene_take(20).depth);
    return 0;
}
'''
def main():
    d = ROOT / 'build/test/stereo-runtime'
    d.mkdir(parents=True, exist_ok=True)
    c = d / 'stereo_selection.c'
    exe = d / ('stereo_selection.exe' if os.name == 'nt' else 'stereo_selection')
    c.write_text(harness, encoding='utf-8')
    subprocess.run([str(CLANG), '-std=c11', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    # Compile the complete production implementation and exercise its public hooks.
    c.write_text(r'''
#include "vk_stereo.h"
#include <assert.h>
#include <string.h>
static const char *mode = "sbs";
static int texture_reads, snapshots;
static VkTexImage sampled;
const char *cfg_value(const char *name) { (void)name; return mode; }
const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) {
    (void)view; assert(id == 1); texture_reads++; if (has_view) *has_view = 0; return &sampled;
}
void vk_texture_lock(void) {}
void vk_texture_unlock(void) {}
int vk_usnap_read(uint64_t addr, uint64_t n, void *dst) {
    (void)addr; snapshots++; memset(dst, 0, n); float *p = (float *)dst + 28;
    p[0] = p[5] = p[11] = 1; p[14] = -1; return 1;
}
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat f, VkImageViewType t, uint32_t a,
                                uint32_t l, uint32_t b, uint32_t n, int blocks) {
    (void)img; (void)f; (void)t; (void)a; (void)l; (void)b; (void)n; (void)blocks;
    return (VkImageView)(uintptr_t)11;
}
int main(void) {
    VkExec ex = {0}; VkPipeInfo info = {0};
    VkTexImage z = {.usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    ex.pass_ncolor = 1; ex.ubo[0][0].addr = 1; ex.ubo[0][0].size = 0x3500;
    ex.color[0] = (VkExecTarget){.image=(VkImage)(uintptr_t)10, .width=1280, .height=720,
        .layers=1, .format=VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    ex.depth = (VkExecTarget){.image=(VkImage)(uintptr_t)1, .width=1280, .height=720,
        .layers=1, .format=VK_FORMAT_D32_SFLOAT, .img=&z};
    ex.depth_range[1] = 1; ex.pipe.ds.dt = 1; ex.pipe.ds.dw = 1;
    vk_stereo_draw(&ex, &info);
    vk_stereo_copy((VkImage)(uintptr_t)10, (VkImage)(uintptr_t)20);
    assert(vk_stereo_frame((VkImage)(uintptr_t)20).depth == (VkImage)(uintptr_t)1);
    vk_stereo_draw(&ex, &info);
    assert(!vk_stereo_frame((VkImage)(uintptr_t)99).depth);
    vk_stereo_draw(&ex, &info);
    vk_stereo_temporal((VkImage)(uintptr_t)10, (VkImage)(uintptr_t)2, (VkImage)(uintptr_t)20);
    assert(!vk_stereo_frame((VkImage)(uintptr_t)20).depth);
    vk_stereo_draw(&ex, &info); vk_stereo_image_gone((VkImage)(uintptr_t)1);
    assert(!vk_stereo_frame((VkImage)(uintptr_t)10).depth);
    VkExec scene = ex;
    vk_stereo_draw(&scene, &info);
    sampled = (VkTexImage){.image=(VkImage)(uintptr_t)10, .width=1280, .height=720,
                          .samples=1, .aspect=VK_IMAGE_ASPECT_COLOR_BIT};
    GfxShaderRes resource = {.slot=0, .type=3};
    VkPipeBinding binding = {.kind=GFX_RES_TEX, .stage=GFX_STAGE_FS, .count=1, .res=&resource};
    info.n_bindings=1; info.bindings=&binding;
    ex=(VkExec){0}; ex.pass_ncolor=1; ex.color[0]=scene.color[0];
    ex.color[0].image=(VkImage)(uintptr_t)20; ex.tex[1][0]=1;
    vk_stereo_draw(&ex, &info);
    assert(vk_stereo_frame((VkImage)(uintptr_t)20).depth==(VkImage)(uintptr_t)1);
    vk_stereo_draw(&scene, &info);
    ex.pipe.color.blend_en=1; ex.color[0]=scene.color[0]; info.n_bindings=0;
    vk_stereo_draw(&ex, &info); // blended HUD retains the scene contribution
    assert(vk_stereo_frame(ex.color[0].image).depth==(VkImage)(uintptr_t)1);
    vk_stereo_draw(&scene, &info); vk_stereo_forget(scene.depth.image);
    assert(!vk_stereo_frame(scene.color[0].image).depth); // depth clears invalidate every descendant
    vk_stereo_draw(&scene, &info); vk_stereo_copy(scene.color[0].image, (VkImage)(uintptr_t)20);
    vk_stereo_forget((VkImage)(uintptr_t)20);
    assert(!vk_stereo_frame((VkImage)(uintptr_t)20).depth); // unknown/clear writes do not keep old association
    info.n_bindings=1; ex=(VkExec){0}; ex.pass_ncolor=1; ex.color[0]=scene.color[0];
    ex.color[0].image=(VkImage)(uintptr_t)20; ex.tex[1][0]=1;
    ex.has_scissor=1; ex.scissor.w=640; ex.scissor.h=720;
    vk_stereo_draw(&scene, &info); vk_stereo_draw(&ex, &info);
    assert(!vk_stereo_frame(ex.color[0].image).depth); // partial source draws cannot seed unrelated outputs
    vk_stereo_draw(&scene, &info); vk_stereo_copy(scene.color[0].image,(VkImage)(uintptr_t)20);
    resource.flags=4; binding.kind=GFX_RES_IMG; sampled.image=scene.depth.image;
    ex=(VkExec){0}; ex.img[1][0]=1;
    vk_stereo_draw(&ex,&info); // graphics imageStore can update depth without an attachment
    assert(!vk_stereo_frame((VkImage)(uintptr_t)20).depth);
    const int reflected_stages[]={GFX_STAGE_VS,GFX_STAGE_FS,GFX_STAGE_TS,GFX_STAGE_MS};
    const int nvn_stages[]={0,1,6,7};
    for (int i=0; i<4; i++) {
        vk_stereo_draw(&scene,&info); vk_stereo_copy(scene.color[0].image,(VkImage)(uintptr_t)20);
        binding.stage=reflected_stages[i]; ex=(VkExec){0}; ex.img[nvn_stages[i]][0]=1;
        vk_stereo_images(&ex,&info);
        assert(!vk_stereo_frame((VkImage)(uintptr_t)20).depth);
    }
    resource.flags=0;
    vk_stereo_draw(&scene,&info); vk_stereo_copy(scene.color[0].image,(VkImage)(uintptr_t)20);
    vk_stereo_images(&ex,&info); // read-only image binding cannot invalidate a valid frame
    assert(vk_stereo_frame((VkImage)(uintptr_t)20).depth==(VkImage)(uintptr_t)1);
    resource.flags=4; resource.pool=1;
    vk_stereo_draw(&scene,&info); vk_stereo_copy(scene.color[0].image,(VkImage)(uintptr_t)20);
    vk_stereo_images(&ex,&info); assert(!vk_stereo_frame((VkImage)(uintptr_t)20).depth);
    mode="off"; texture_reads=snapshots=0;
    vk_stereo_draw(&ex, &info); vk_stereo_draw(&scene, &info);
    assert(!texture_reads && !snapshots);
    return 0;
}
''', encoding='utf-8')
    subprocess.run([str(CLANG), '-std=gnu11', '-I', str(ROOT / 'runtime'),
                    '-I', vma,
                    str(ROOT / 'runtime/vk_stereo.c'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    backend = (ROOT / 'runtime/vk_backend.c').read_text(encoding='utf-8')
    handoff = backend[backend.index('static void present_image('):backend.index('// ---- Asynchronous replay')]
    assert handoff.index('vk_stereo_frame(present)') < handoff.index('int held = frame_hold(')
    assert '.stereo = stereo' in handoff and 'StereoFrame stereo;' in backend
    assert 'vk_stereo_present_frame(j.stereo);' in backend and 'vk_stereo_present_frame((StereoFrame){0});' in backend
    graphics = (ROOT / 'runtime/vk_exec_draw.c').read_text(encoding='utf-8')
    assert 'vk_stereo_images(ex, info);' in graphics, 'task/mesh draws must invalidate storage writes too'
    clears = (ROOT / 'runtime/vk_exec.c').read_text(encoding='utf-8')
    for command in ('vkCmdClearAttachments', 'vkCmdClearColorImage', 'vkCmdClearDepthStencilImage'):
        section = clears[clears.index(command + '(ex->cb'):]
        assert 'vk_stereo_forget(' in section[:450], command + ' must invalidate stereo provenance'
    print('stereo frame selection and public hooks: PASS')

if __name__ == '__main__':
    main()
