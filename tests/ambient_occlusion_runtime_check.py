"""Exercise the real AO scene-selection code without starting a game or GPU.

The shader's numerical output has a separate tiny Vulkan check. This check
compiles the renderer's state functions, using only recording stubs for the
submission snapshot and Vulkan calls.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r'(?m)^(?:static )?[^\n;{}]+\b' + name + r'\([^;]*?\)\s*\{', source)
    if not start:
        raise AssertionError(f"AO function missing: {name}")
    end = start.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start.start():end]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default='clang')
    args = ap.parse_args()
    src = ROOT / 'runtime/vk_ao.c'
    assert src.exists(), 'Ambient occlusion is not wired into the renderer'
    source = src.read_text(encoding='utf-8')
    # Public integration, not just an isolated helper that no renderer calls.
    for path, call in [('runtime/vk_exec_draw.c', 'vk_ao_draw(ex);'),
                       ('runtime/vk_exec_draw.c', 'vk_ao_image_gone(image);'),
                       ('runtime/vk_exec.c', 'vk_ao_frame();'),
                       ('runtime/vk_tonemap.c', 'vk_ao_record('),
                       ('runtime/vk_dlaa.c', 'vk_ao_record('),
                       ('runtime/gfx_present.c', 'vk_ao_release();')]:
        assert call in (ROOT / path).read_text(encoding='utf-8'), f'Missing AO integration: {path}: {call}'
    types = source[source.index('// BEGIN AO state'):source.index('// END AO state')]
    funcs = '\n'.join(function(source, name) for name in
                      ['ao_mode', 'projection_ok', 'scene_reset', 'scene_note', 'scene_pick_depth', 'scene_pick', 'vk_ao_image_gone'])
    code = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t VkImage;
typedef uint64_t VkImageView;
#define VK_NULL_HANDLE 0
''' + types + '\n' + funcs + r'''
int main(void) {
    assert(ao_mode("off") == 0 && ao_mode("ssao") == 1 && ao_mode("hbao") == 2);
    assert(ao_mode("GTAO") == 0 && ao_mode("") == 0);
    float p[16] = {1.2f,0,0,0, 0,1.7f,0,0, 0,0,1.002f,0.2f, 0,0,-1,0};
    assert(projection_ok(p));
    float saved = p[0]; p[0] = NAN; assert(!projection_ok(p)); p[0] = saved;
    p[14] = 0; assert(!projection_ok(p)); p[14] = -1;
    p[1] = 1; assert(!projection_ok(p)); p[1] = 0;
    p[2] = 0.1f; assert(projection_ok(p)); p[2] = 0;
    scene_reset();
    assert(scene_pick() == NULL);
    scene_note(10, 100, 1280, 720, 1000, p);
    assert(scene_pick() && scene_pick()->address == 1000);
    scene_note(20, 200, 640, 360, 2000, p);  // a half-size effect must not steal the scene
    assert(S.depth == 10 && S.width == 1280);
    scene_note(10, 100, 1280, 720, 3000, p);
    scene_note(10, 100, 1280, 720, 3000, p);
    assert(scene_pick()->address == 3000);  // dominant camera, not the last reflection
    scene_note(10, 100, 1280, 720, 4000, p);
    assert(scene_pick()->address == 3000);
    scene_note(30, 300, 1920, 1080, 5000, p);
    assert(S.depth == 30 && S.width == 1920 && scene_pick()->address == 5000);
    for (int i = 0; i < 20; ++i) scene_note(30, 300, 1920, 1080, 6000+i, p);
    assert(S.count <= 8 && scene_pick()->address == 5000);
    scene_reset();
    scene_note(10, 100, 1280, 720, 1000, p);
    scene_note(10, 100, 1280, 720, 1000, p);
    scene_note(20, 200, 1280, 720, 2000, p);
    assert(scene_pick() == NULL); // equal-sized unrelated depths are ambiguous for the spatial path
    assert(scene_pick_depth(10)->address == 1000 && scene_pick_depth(10)->view == 100);
    assert(scene_pick_depth(20)->address == 2000 && scene_pick_depth(30) == NULL);
    vk_ao_image_gone(20);
    assert(!scene_pick_depth(20) && scene_pick_depth(10)->address == 1000);
    vk_ao_image_gone(10);
    scene_note(30, 300, 1280, 720, 3000, p);
    assert(scene_pick() == NULL); // retiring every candidate must not reopen an ambiguous frame
    assert(scene_pick_depth(30)->view == 300);
    scene_note(40, 400, 1920, 1080, 4000, p);
    assert(scene_pick() == NULL); // changing size does not establish spatial output identity either
    assert(scene_pick_depth(40)->view == 400);
    scene_reset(); assert(!S.depth && !S.count && !S.done && scene_pick() == NULL);
    scene_note(40, 400, 800, 600, 7000, p); // smaller window works after reset
    assert(S.width == 800 && scene_pick()->address == 7000);
    vk_ao_image_gone(41); assert(S.depth == 40);
    vk_ao_image_gone(40); assert(!S.depth && !scene_pick());
    scene_note(40, 400, 800, 600, 7000, p);
    S.done = 1; vk_ao_image_gone(40);
    assert(!S.depth && S.done); // retirement must not allow a second AO application this frame
    puts("AO runtime: mode parsing, projection guards, scene sizes, camera votes and reset passed");
}
'''
    out = ROOT / 'build/test/ambient-occlusion'
    out.mkdir(parents=True, exist_ok=True)
    c = out / 'state.c'
    exe = out / 'state.exe'
    c.write_text(code, encoding='utf-8')
    subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.CalledProcessError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        sys.exit(1)
