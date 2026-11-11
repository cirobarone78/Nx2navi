"""Check the ambient occlusion menu and config without starting the game."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def function(source, name):
    start = source.index(f'static void {name}(')
    body = source.index('{', start)
    depth, end = 1, body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

PREAMBLE = r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "ui/option_rows.h"
struct Row { std::string key; float lo, hi, step; std::vector<std::string> values; std::vector<UiTextId> names; };
static std::vector<Row> rows;
static const char *mode = "off";
extern "C" const char *menu_text(UiTextId) { return "localized"; }
void opt_begin(const char *, float) { rows.clear(); }
void opt_end() {}
void opt_group(UiTextId) {}
void opt_disable_next(UiTextId) {}
bool opt_choice_list(const char *k, UiTextId, const char *const *v, const char *const *, int n, UiTextId, int, const char *) {
    Row r{k, 0, 0, 0, {}, {}}; for (int i = 0; i < n; ++i) r.values.push_back(v[i]); rows.push_back(r); return false;
}
bool opt_slider(const char *k, UiTextId, float lo, float hi, UiTextId, float step, const char *, const char *, const char *) {
    rows.push_back({k, lo, hi, step, {}, {}}); return false;
}
bool opt_choice(const char *k, UiTextId, const UiTextId *v, int n, UiTextId, const char *) {
    Row r{k, 0, 0, 0, {}, {}}; for (int i = 0; i < n; ++i) r.names.push_back(v[i]); rows.push_back(r); return false;
}
bool opt_toggle(const char *, UiTextId, UiTextId, bool) { return false; }
bool opt_toggle_value(const char *, UiTextId, UiTextId, const char *, const char *, const char *) { return false; }
static int cfg_bool(const char *) { return 0; }
static const char *cfg_value(const char *k) { return !strcmp(k, "NX2NAVI_AO") ? mode : "fsr"; }
static int vk_ffx_available() { return 0; }
static int wide_available() { return 0; }
static int res_adjustable() { return 0; }
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
static const Row *find(const char *key) { for (const auto &r : rows) if (r.key == key) return &r; return nullptr; }
int main() {
    for (const char *m : {"off", "ssao", "hbao"}) {
        mode = m; tab_screen(1.0f);
        const Row *ao = find("NX2NAVI_AO"); assert(ao);
        assert((ao->values == std::vector<std::string>{"off", "ssao", "hbao"}));
        const Row *s = find("NX2NAVI_AO_STRENGTH"), *r = find("NX2NAVI_AO_RADIUS"), *q = find("NX2NAVI_AO_QUALITY");
        if (!strcmp(m, "off")) { assert(!s && !r && !q); continue; }
        assert(s && s->lo == 0 && s->hi == 100 && s->step == 1);
        assert(r && r->lo == 1 && r->hi == 100 && r->step == 1);
        assert(q && (q->names == std::vector<UiTextId>{UI_MENU_AO_LOW, UI_MENU_AO_MEDIUM, UI_MENU_AO_HIGH}));
    }
    puts("AO Screen rows: modes, hidden state, slider bounds and quality passed");
}
'''
CONFIG = r'''
#include "cfg.c"
#include <assert.h>
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void rt_log(const char *fmt, ...) { (void)fmt; }
int plat_setenv(const char *name, const char *value) { (void)name; (void)value; return 0; }
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; return 0; }
uint8_t *plat_file_read(const char *p, uint64_t *size) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *data = malloc((size_t)n + 1); assert(data);
    assert(fread(data, 1, n, f) == (size_t)n); fclose(f); data[n] = 0; *size = n; return data;
}
int plat_file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }
int64_t plat_file_size(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
int plat_file_write_atomic(const char *p, const void *data, size_t n) {
    FILE *f = fopen(p, "wb"); if (!f) return 0; int ok = fwrite(data, 1, n, f) == n; return fclose(f) == 0 && ok;
}
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const char *keys[] = {"NX2NAVI_AO", "NX2NAVI_AO_STRENGTH", "NX2NAVI_AO_RADIUS", "NX2NAVI_AO_QUALITY"};
    const char *defaults[] = {"off", "50", "20", "1"};
    const char *values[] = {"hbao", "77", "32", "2"};
    for (int i = 0; i < 4; i++) {
        assert(!getenv(keys[i])); assert(is_known(keys[i] + 8));
        assert(!strcmp(cfg_value(keys[i]), defaults[i])); assert(cfg_meta(keys[i])->live);
        if (argc == 2) { cfg_set(keys[i], values[i]); assert(!strcmp(cfg_value(keys[i]), values[i])); }
    }
    if (argc == 2) assert(cfg_save_to(argv[1]));
    assert(cfg_load(argv[1]));
    for (int i = 0; i < 4; i++) assert(!strcmp(cfg_value(keys[i]), values[i]));
    puts("AO config: actual defaults, live changes, save and reload passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CXX') or shutil.which('clang++') or 'clang++')
    args = parser.parse_args()
    cfg = (ROOT / 'runtime/cfg.c').read_text(encoding='utf-8')
    for key, default, values in [('AO', 'off', 'off|ssao|hbao'),
                                 ('AO_STRENGTH', '50', '0..100'),
                                 ('AO_RADIUS', '20', '1..100'),
                                 ('AO_QUALITY', '1', '0|1|2')]:
        assert f'{{"{key}", "{default}", "{values}", LIVE}}' in cfg, f'missing AO metadata: {key}'
        assert f'{{"{key}", "display"}}' in cfg, f'missing display section: {key}'
    print('AO metadata passed')
    translations = (ROOT / 'runtime/ui_text.c').read_text(encoding='utf-8')
    for name in ('AO', 'AO_DESC', 'AO_STRENGTH', 'AO_STRENGTH_DESC', 'AO_RADIUS', 'AO_RADIUS_DESC',
                 'AO_QUALITY', 'AO_QUALITY_DESC', 'AO_LOW', 'AO_MEDIUM', 'AO_HIGH'):
        assert translations.count(f'[UI_MENU_{name}]') == 4, f'missing translation: {name}'
    menu = (ROOT / 'runtime/ui/menu.cpp').read_text(encoding='utf-8')
    assert 'row_ambient_occlusion();' in menu, 'Screen tab must expose AO controls'
    work = ROOT / 'build/test/ambient-occlusion-menu'
    work.mkdir(parents=True, exist_ok=True)
    harness = work / 'check.cpp'
    harness.write_text(PREAMBLE + '\n'.join(function(menu, name) for name in ('row_ambient_occlusion', 'tab_screen')) + MAIN, encoding='utf-8')
    exe = work / ('check.exe' if os.name == 'nt' else 'check')
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get('PATH', '')
    include = ['-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/third_party/imgui')]
    subprocess.run([args.cc, '-std=c++17', '-O0', '-Wall', '-Wextra', *include, str(harness), '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
    config = work / 'config.c'
    config.write_text(CONFIG, encoding='utf-8')
    config_exe = work / ('config.exe' if os.name == 'nt' else 'config')
    ini = work / 'config.ini'
    ini.write_text('[display]\n# AO settings\n', encoding='utf-8')
    subprocess.run([args.cc, '-x', 'c', '-std=gnu11', '-ffunction-sections', '-fdata-sections', *include,
                    str(config), '-Wl,--gc-sections', '-o', str(config_exe)], check=True, env=env)
    subprocess.run([str(config_exe), str(ini)], check=True, env=env)
    # A fresh process has no previously set values that could hide a broken reload.
    subprocess.run([str(config_exe), str(ini), 'reload'], check=True, env=env)
    text = ini.read_text(encoding='utf-8').lower()
    assert text.index('[display]') < text.index('ao = hbao')


if __name__ == '__main__':
    main()
