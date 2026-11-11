"""Exercise the production present-time bias policy with synthetic modes/extents.

Run with --cc PATH to a C compiler; no game data or GPU is needed.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_dlaa.c').read_text()
start = source.index('static void dlss_mode(void)')
end = source.index('// The preset,', start)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static struct { uint64_t evaluated_extent; } D;
static int active, fsr, calls;
static float applied;
static const char *mode = "dlaa";
static const char *dynres_override(void) { return NULL; }
static const char *cfg_value(const char *key) { (void)key; return mode; }
static void res_dlss_mode(const char *m) { (void)m; }
static int dl_active(void) { return active; }
static int use_fsr(void) { return fsr; }
static float res_dlss_lod_bias(void) { return -0.75f; }
static void vk_sampler_dlss_bias(float b) { applied = b; ++calls; }
#define rt_log(...) ((void)0)
'''
harness += source[start:end]
harness += r'''
static void check(unsigned w, unsigned ow, float expected) {
    D.evaluated_extent = ((uint64_t)w << 32) | ow;
    dlss_mode();
    assert(fabsf(applied - expected) < 0.00001f);
    int previous = calls;
    dlss_mode();
    assert(calls == previous); // unchanged extents do not rebuild sampler state
}
int main(void) {
    active = 1;
    check(0, 0, 0); // no successful evaluation yet
    check(1920, 1920, -1); // DLAA
    mode = "quality";
    check(1280, 1920, log2f(2.0f/3.0f)-1);
    check(960, 1920, -2);
    check(0, 1920, 0);
    check(1920, 0, 0);
    check(2000, 1920, 0);
    fsr = 1;
    check(960, 1920, -0.75f); // keep the FSR policy
    fsr = 0;
    check(1920, 1920, -1);
    active = 0;
    dlss_mode();
    assert(applied == 0 && D.evaluated_extent == 0);
    active = 1;
    dlss_mode();
    assert(applied == 0); // do not reuse a disabled mode's extent
    puts("DLSS material bias checks passed");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / 'bias.c'
    exe = Path(temp) / 'bias.exe'
    c.write_text(harness)
    subprocess.run([args.cc, '-std=c11', str(c), '-o', str(exe), '-lm'], check=True)
    subprocess.run([str(exe)], check=True)
