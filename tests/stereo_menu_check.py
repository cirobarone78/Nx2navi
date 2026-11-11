"""Check the stereo menu and config without starting the game."""
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
struct Row { std::string key; float lo, hi, step; std::vector<std::string> values; std::vector<UiTextId> names; std::vector<std::string> labels; };
static std::vector<Row> rows;
static const char *mode = "off";
extern "C" const char *menu_text(UiTextId id) {
    switch (id) {
    case UI_MENU_STEREO_OFF: return "off label";
    case UI_MENU_STEREO_SBS: return "sbs label";
    case UI_MENU_STEREO_TAB: return "tab label";
    case UI_MENU_STEREO_ANAGLYPH: return "anaglyph label";
    default: return "localized";
    }
}
void opt_begin(const char *, float) { rows.clear(); }
void opt_end() {}
void opt_group(UiTextId) {}
void opt_disable_next(UiTextId) {}
bool opt_choice_list(const char *k, UiTextId, const char *const *v, const char *const *names, int n, UiTextId, int, const char *) {
    Row r{k, 0, 0, 0, {}, {}, {}}; for (int i = 0; i < n; ++i) { r.values.push_back(v[i]); r.labels.push_back(names[i]); } rows.push_back(r); return false;
}
bool opt_slider(const char *k, UiTextId, float lo, float hi, UiTextId, float step, const char *, const char *, const char *) {
    rows.push_back({k, lo, hi, step, {}, {}, {}}); return false;
}
bool opt_choice(const char *k, UiTextId, const UiTextId *v, int n, UiTextId, const char *) {
    Row r{k, 0, 0, 0, {}, {}, {}}; for (int i = 0; i < n; ++i) r.names.push_back(v[i]); rows.push_back(r); return false;
}
bool opt_toggle(const char *k, UiTextId, UiTextId, bool) { rows.push_back({k, 0, 0, 0, {}, {}, {}}); return false; }
bool opt_toggle_value(const char *, UiTextId, UiTextId, const char *, const char *, const char *) { return false; }
static int cfg_bool(const char *) { return 0; }
static const char *cfg_value(const char *k) { return !strcmp(k, "NX2NAVI_STEREO") ? mode : "fsr"; }
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
    for (const char *m : {"off", "sbs", "tab", "anaglyph"}) {
        mode = m; tab_screen(1.0f);
        const Row *ao = find("NX2NAVI_STEREO"); assert(ao);
        assert((ao->values == std::vector<std::string>{"off", "sbs", "tab", "anaglyph"}));
        assert((ao->labels == std::vector<std::string>{"off label", "sbs label", "tab label", "anaglyph label"}));
        const Row *s = find("NX2NAVI_STEREO_SEPARATION"), *r = find("NX2NAVI_STEREO_CONVERGENCE"), *q = find("NX2NAVI_STEREO_SWAP");
        if (!strcmp(m, "off")) { assert(!s && !r && !q); continue; }
        assert(s && s->lo == 0 && s->hi == 100 && s->step == 1);
        assert(r && r->lo == 1 && r->hi == 1000 && r->step == 1);
        assert(q);
    }
    puts("STEREO Screen rows: modes, hidden state, slider bounds and swap passed");
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
    const char *keys[] = {"NX2NAVI_STEREO", "NX2NAVI_STEREO_SEPARATION", "NX2NAVI_STEREO_CONVERGENCE", "NX2NAVI_STEREO_SWAP"};
    const char *defaults[] = {"off", "20", "100", "0"};
    const char *values[] = {"anaglyph", "77", "320", "1"};
    for (int i = 0; i < 4; i++) {
        assert(!getenv(keys[i])); assert(is_known(keys[i] + 8));
        assert(!strcmp(cfg_value(keys[i]), defaults[i])); assert(cfg_meta(keys[i])->live);
        if (argc == 2) { cfg_set(keys[i], values[i]); assert(!strcmp(cfg_value(keys[i]), values[i])); }
    }
    if (argc == 2) assert(cfg_save_to(argv[1]));
    assert(cfg_load(argv[1]));
    for (int i = 0; i < 4; i++) assert(!strcmp(cfg_value(keys[i]), values[i]));
    puts("STEREO config: actual defaults, live changes, save and reload passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CXX') or shutil.which('clang++') or 'clang++')
    args = parser.parse_args()
    cfg = (ROOT / 'runtime/cfg.c').read_text(encoding='utf-8')
    for key, default, values in [('STEREO', 'off', 'off|sbs|tab|anaglyph'),
                                 ('STEREO_SEPARATION', '20', '0..100'),
                                 ('STEREO_CONVERGENCE', '100', '1..1000'),
                                 ('STEREO_SWAP', '0', '0|1')]:
        assert f'{{"{key}", "{default}", "{values}", LIVE}}' in cfg, f'missing STEREO metadata: {key}'
        assert f'{{"{key}", "display"}}' in cfg, f'missing display section: {key}'
    print('STEREO metadata passed')
    translations = (ROOT / 'runtime/ui_text.c').read_text(encoding='utf-8')
    for name in ('STEREO', 'STEREO_DESC', 'STEREO_SEPARATION', 'STEREO_SEPARATION_DESC', 'STEREO_CONVERGENCE', 'STEREO_CONVERGENCE_DESC',
                 'STEREO_SWAP', 'STEREO_SWAP_DESC', 'STEREO_OFF', 'STEREO_SBS', 'STEREO_TAB', 'STEREO_ANAGLYPH'):
        assert translations.count(f'[UI_MENU_{name}]') == 4, f'missing translation: {name}'
    menu = (ROOT / 'runtime/ui/menu.cpp').read_text(encoding='utf-8')
    assert 'row_stereo();' in menu, 'Screen tab must expose STEREO controls'
    work = ROOT / 'build/test/stereo-menu'
    work.mkdir(parents=True, exist_ok=True)
    harness = work / 'check.cpp'
    harness.write_text(PREAMBLE + '\n'.join(function(menu, name) for name in ('row_stereo', 'tab_screen')) + MAIN, encoding='utf-8')
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
    ini.write_text('[display]\n# STEREO settings\n', encoding='utf-8')
    subprocess.run([args.cc, '-x', 'c', '-std=gnu11', '-ffunction-sections', '-fdata-sections', *include,
                    str(config), '-Wl,--gc-sections', '-o', str(config_exe)], check=True, env=env)
    subprocess.run([str(config_exe), str(ini)], check=True, env=env)
    # A fresh process has no previously set values that could hide a broken reload.
    subprocess.run([str(config_exe), str(ini), 'reload'], check=True, env=env)
    text = ini.read_text(encoding='utf-8').lower()
    for key, value in [('stereo', 'anaglyph'), ('stereo_separation', '77'),
                       ('stereo_convergence', '320'), ('stereo_swap', '1')]:
        assert text.index('[display]') < text.index(f'{key} = {value}')


if __name__ == '__main__':
    main()
