"""Exercise the actual put_picture body and its packed-eye presentation boundary."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang') or 'clang')
    args = parser.parse_args()
    source = (ROOT / 'runtime/gfx_present.c').read_text(encoding='utf-8')
    start = source.index('static int put_picture(')
    body = source.index('{', start)
    end, depth = body + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    setup = r'''
#include "vk.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define H(t,n) ((t)(uintptr_t)(n))
#define FRAMES 2
static int g_sc_hdr, stereo_on=1, stereo_calls, fsr_calls;
static VkImageBlit blit;
static VkFilter filter;
static VkImage last_source;
static VkImage vk_stereo_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt,
                               int w, int h, const int crop[4], int ow, int oh) {
    (void)cb; (void)slot; (void)src; (void)fmt; (void)w; (void)h; (void)crop;
    assert(ow==96 && oh==48);
    stereo_calls++; return stereo_on ? H(VkImage,20) : VK_NULL_HANDLE;
}
VkImage vk_color_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat f, const int r[4]) {
    (void)cb; (void)slot; (void)src; (void)f; (void)r; return VK_NULL_HANDLE;
}
VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat f, const int r[4]) {
    (void)cb; (void)slot; (void)src; (void)f; (void)r; return VK_NULL_HANDLE;
}
int vk_fsr_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat f, int w, int h,
                  const int crop[4], VkImage dst, const int out[4]) {
    (void)cb; (void)slot; (void)src; (void)f; (void)w; (void)h; (void)crop; (void)dst; (void)out;
    fsr_calls++; return 0;
}
static void sync_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout old, VkImageLayout next,
    VkPipelineStageFlags2 s, VkAccessFlags2 a, VkPipelineStageFlags2 d, VkAccessFlags2 b) {
    (void)cb; (void)img; (void)old; (void)next; (void)s; (void)a; (void)d; (void)b;
}
void rc_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                      uint32_t n, const VkImageBlit *regions, VkFilter f) {
    (void)cb; (void)sl; (void)dst; (void)dl; assert(n==1); blit=*regions; filter=f; last_source=src;
}
'''
    checks = r'''
int main(void) {
    const int rect[4]={2,3,64,24}, to[4]={10,5,96,48};
    put_picture(H(VkCommandBuffer,1),0,H(VkImage,2),VK_FORMAT_R8G8B8A8_UNORM,80,40,rect,H(VkImage,3),to,1);
    assert(stereo_calls==1 && !fsr_calls && last_source==H(VkImage,20));
    assert(blit.srcOffsets[0].x==0 && blit.srcOffsets[0].y==0);
    assert(blit.srcOffsets[1].x-blit.srcOffsets[0].x==blit.dstOffsets[1].x-blit.dstOffsets[0].x);
    assert(blit.srcOffsets[1].y-blit.srcOffsets[0].y==blit.dstOffsets[1].y-blit.dstOffsets[0].y);
    assert(filter==VK_FILTER_NEAREST); // no filtered taps can cross packed eyes
    stereo_on=0; stereo_calls=fsr_calls=0;
    put_picture(H(VkCommandBuffer,1),0,H(VkImage,2),VK_FORMAT_R8G8B8A8_UNORM,80,40,rect,H(VkImage,3),to,1);
    assert(stereo_calls==1 && fsr_calls==1 && last_source==H(VkImage,2) && filter==VK_FILTER_LINEAR);
    assert(blit.srcOffsets[0].x==2 && blit.srcOffsets[1].x==66);
    stereo_calls=0;
    put_picture(H(VkCommandBuffer,1),FRAMES,H(VkImage,2),VK_FORMAT_R8G8B8A8_UNORM,80,40,rect,H(VkImage,3),to,1);
    assert(!stereo_calls); // HUD-less frame-generation comparison is never stereo-packed
    puts("Stereo presentation boundary: final-sized unfiltered copy, normal fallback and HUD-less bypass passed");
}
'''
    out = ROOT / 'build/test/stereo-put-picture'
    out.mkdir(parents=True, exist_ok=True)
    path = out / 'check.c'
    path.write_text(setup + source[start:end] + checks, encoding='utf-8')
    cc = shutil.which(args.cc) or args.cc
    exe = out / ('check.exe' if os.name == 'nt' else 'check')
    subprocess.run([cc, '-std=gnu11', '-I' + str(ROOT / 'runtime'),
                    '-I' + str(Path(cc).resolve().parent.parent / 'include/vma'),
                    str(path), '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()
