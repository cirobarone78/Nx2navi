"""Exercise the Screen tab's shadow rows without a game or a graphics device.

Compile the actual tab and row function bodies, replacing unrelated window/device UI
with no-ops and recording option rows. Usage: python tests/shadow_menu_check.py --cc <clang++>
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index(f'static void {name}(')
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREAMBLE = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ui/option_rows.h"
struct Row { std::string key; bool disabled; std::vector<std::string> values, names; };
static std::vector<Row> rows;
static bool disabled, adjustable, shadow_patch, cascade_patch;
extern "C" const char *menu_text(UiTextId) { return "localized"; }
void opt_begin(const char *, float) { rows.clear(); disabled = false; }
void opt_end() {}
void opt_group(UiTextId) {}
void opt_text(UiTextId, bool) {}
void opt_disable_next(UiTextId) { disabled = true; }
bool opt_choice_list(const char *key, UiTextId, const char *const *values, const char *const *names,
                     int n, UiTextId, int, const char *) {
    Row row{key, disabled, {}, {}};
    for (int i = 0; i < n; ++i) { row.values.push_back(values[i]); row.names.push_back(names[i]); }
    rows.push_back(row); disabled = false; return false;
}
bool opt_toggle(const char *, UiTextId, UiTextId, bool) { disabled = false; return false; }
bool opt_toggle_value(const char *, UiTextId, UiTextId, const char *, const char *, const char *) { disabled = false; return false; }
bool opt_choice(const char *, UiTextId, const UiTextId *, int, UiTextId, const char *) { disabled = false; return false; }
bool opt_slider(const char *, UiTextId, float, float, UiTextId, float, const char *, const char *, const char *) { disabled = false; return false; }
static int cfg_bool(const char *) { return 0; }
static const char *cfg_value(const char *) { return "fsr"; }
static int vk_ffx_available() { return 0; }
static int wide_available() { return 1; }
static int res_adjustable() { return adjustable; }
static int rt_patch_set(const char *name) {
    if (!std::strcmp(name, "shadow")) return shadow_patch ? 0 : -1;
    if (!std::strcmp(name, "cascade")) return cascade_patch ? 1 : -1;
    assert(false && "Unexpected patch set name");
    return -1;
}
static const char *vsync_note() { return nullptr; }
static void row_window_size() {}
static void row_gpu() {}
static void row_output_res() {}
static void row_game_res() {}
static void row_upscaling() {}
static void row_framegen() {}
static void row_hdr() {}
static void row_texture_detail() {}
'''
MAIN = r'''
static const Row *find(const char *key) {
    for (const auto &row : rows) if (row.key == key) return &row;
    return nullptr;
}
int main() {
    for (unsigned mask = 0; mask < 8; ++mask) {
        adjustable = (mask & 1) != 0;
        shadow_patch = (mask & 2) != 0;
        cascade_patch = (mask & 4) != 0;
        tab_screen(1.0f);
        const auto *scale = find("NX2NAVI_SHADOW_SCALE");
        if (!scale) { std::fputs("FAIL: Screen tab does not expose shadow resolution\n", stderr); return 1; }
        assert(!scale->disabled);
        assert((scale->values == std::vector<std::string>{"1", "2", "3", "4"}));
        assert((scale->names == std::vector<std::string>{"1x", "2x", "3x", "4x"}));
        const auto *range = find("NX2NAVI_SHADOW_RANGE");
        assert(range && range->disabled == !cascade_patch);
        const auto *fixed = find("NX2NAVI_SHADOW_RES");
        assert((fixed != nullptr) == (adjustable && shadow_patch));
        if (fixed) assert(!fixed->disabled);
    }
    std::puts("shadow menu: 1x-4x choices and all 8 independent patch-gate combinations passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CXX') or shutil.which('clang++') or 'clang++')
    args = parser.parse_args()
    source = (ROOT / 'runtime/ui/menu.cpp').read_text(encoding='utf-8')
    functions = '\n'.join(function(source, name) for name in
                          ('row_shadow_res', 'row_shadow_scale', 'row_shadow_range', 'tab_screen'))
    work = ROOT / 'build/test/shadow-menu'
    work.mkdir(parents=True, exist_ok=True)
    harness = work / 'check.cpp'
    harness.write_text(PREAMBLE + functions + MAIN, encoding='utf-8')
    exe = work / ('check.exe' if os.name == 'nt' else 'check')
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get('PATH', '')
    subprocess.run([args.cc, '-std=c++17', '-O0', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/third_party/imgui'),
                    str(harness), '-o', str(exe)], check=True, env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    raise SystemExit(main())
