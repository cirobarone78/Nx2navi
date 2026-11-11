"""Check the production grading replay's descriptors and invalidation policy without a GPU."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
parser.add_argument('--include', action='append', default=[])
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_hdr.c').read_text()
start = source.index('void vk_scene_grade_record(')
end = source.index('static int ui_pipeline(', start)
walk = (root / 'runtime/vk_exec.c').read_text()
a = walk.index('        if (vk_scene_grade.graded) {')
b = walk.index('        switch (h->op) {', a + len('        if (vk_scene_grade.graded) {'))
# Find the two closing braces of the invalidation block, not the replay's next switch.
b = walk.index('        }', walk.index('            }', b) + 14) + len('        }')
harness = r'''
#include <assert.h>
#include <stdio.h>
#include "vk_exec.h"
static struct { VkPipeline pipeline; VkPipelineLayout layout; PFN_vkCmdPushDescriptorSetKHR push; } grade;
VkSceneGrade vk_scene_grade;
static int began, drew, ended, pushed;
static VkSceneGrade expected;
#define HANDLE(T, n) ((T)(uintptr_t)(n))
static void begin(VkCommandBuffer cb, const VkRenderingInfo *p) {
    (void)cb; ++began;
    assert(p->renderArea.extent.width == 1920 && p->renderArea.extent.height == 1080);
    assert(p->pColorAttachments->imageView == HANDLE(VkImageView, 8));
}
static void pipeline(VkCommandBuffer cb, VkPipelineBindPoint p, VkPipeline v) { (void)cb; (void)p; (void)v; }
static void viewport(VkCommandBuffer cb, uint32_t n, const VkViewport *v) {
    (void)cb; assert(n == 1 && v->width == 1920 && v->height == 1080);
}
static void scissor(VkCommandBuffer cb, uint32_t n, const VkRect2D *r) {
    (void)cb; assert(n == 1 && r->extent.width == 1920 && r->extent.height == 1080);
}
static void draw(VkCommandBuffer cb, uint32_t n, uint32_t instances, uint32_t v, uint32_t i) {
    (void)cb; assert(n == 3 && instances == 1 && v == 0 && i == 0); ++drew;
}
static void end(VkCommandBuffer cb) { (void)cb; ++ended; }
void rec_push(PFN_vkCmdPushDescriptorSetKHR f, VkCommandBuffer cb, VkPipelineBindPoint p,
                     VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    (void)f; (void)cb; (void)p; (void)layout; assert(set == 0 && n == 4); ++pushed;
    assert(w[0].pBufferInfo->buffer == expected.support.buffer);
    assert(w[3].pBufferInfo->offset == expected.uniforms.offset);
    assert(w[1].pImageInfo->imageView == expected.bloom.imageView);
    assert(w[2].pImageInfo->imageView == HANDLE(VkImageView, 7));
    assert(w[2].pImageInfo->sampler == expected.scene.sampler);
    for (unsigned i = 0; i < n; ++i) assert(w[i].dstBinding == i && w[i].descriptorCount == 1);
}
#undef vkCmdBeginRendering
#define vkCmdBeginRendering begin
#undef vkCmdBindPipeline
#define vkCmdBindPipeline pipeline
#undef vkCmdSetViewportWithCount
#define vkCmdSetViewportWithCount viewport
#undef vkCmdSetScissorWithCount
#define vkCmdSetScissorWithCount scissor
#undef vkCmdDraw
#define vkCmdDraw draw
#undef vkCmdEndRendering
#define vkCmdEndRendering end
'''
harness += source[start:end]
harness += '\nstatic void invalidate(unsigned op) { op_hdr record = {.op = op}; const op_hdr *h = &record;\n'
harness += walk[a:b] + '\n}\n'
harness += r'''
int main(void) {
    expected.support.buffer = HANDLE(VkBuffer, 1);
    expected.uniforms.offset = 4096;
    expected.bloom.imageView = HANDLE(VkImageView, 3);
    expected.scene.imageView = HANDLE(VkImageView, 4);
    expected.scene.sampler = HANDLE(VkSampler, 5);
    vk_scene_grade_record(VK_NULL_HANDLE, &expected, HANDLE(VkImageView, 7), HANDLE(VkImageView, 8), 1920, 1080);
    assert(began == 1 && drew == 1 && ended == 1 && pushed == 1);
    unsigned writes[] = {OP_DRAW, OP_DRAW_MESH_IND, OP_DISPATCH, OP_UPDATE_UB, OP_CLEAR_BUF,
                         OP_COPY_T2T, OP_COPY_B2T, OP_REPORT_COUNTER, OP_SIGNAL_EVENT};
    for (unsigned i = 0; i < sizeof writes / sizeof *writes; ++i) {
        vk_scene_grade.graded = HANDLE(VkImage, 9); invalidate(writes[i]); assert(!vk_scene_grade.graded);
    }
    unsigned state[] = {OP_BIND_PROGRAM, OP_BIND_TEX, OP_BIND_UBO, OP_BARRIER, OP_SET_RT};
    for (unsigned i = 0; i < sizeof state / sizeof *state; ++i) {
        vk_scene_grade.graded = HANDLE(VkImage, 9); invalidate(state[i]); assert(vk_scene_grade.graded);
    }
    puts("Scene grading replay checks passed");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / 'grade.c'
    exe = Path(temp) / 'grade.exe'
    c.write_text(harness)
    inc = [part for directory in [str(root / 'runtime'), *args.include] for part in ['-I', directory]]
    subprocess.run([args.cc, '-std=c11', *inc, str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
