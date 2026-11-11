"""Exercise the production preparation worker with a synthetic translator and resource declarations.

Usage: python tests/prepared_variants_check.py --cc /path/to/clang
No game files, Vulkan device or driver cache are used.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default='clang')
    args = ap.parse_args()
    source = (ROOT / 'runtime/gfx_shader.c').read_text()
    start = source.index('enum { SEED_VARIANTS')
    end = source.index('// Translates every record of ans_path', start)
    worker = source[start:end]
    start = source.index('    size_t u = 0, by[3]', source.index('int gfx_shader_prepare_seed('))
    end = source.index('    free(seen.v);', start)
    record_loop = source[start:end]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#define CONTROL_SIZE 0x880
#define ANS_REC 216
#define SEED_DEFAULT 0x80
#define rt_fatal(...) abort()
struct GfxShader { uint8_t iid[32]; };
static GfxShader shaders[128];
static GfxShaderAnswers requests[128], input;
static int count, kind, failed;
static GfxShaderRes resources[32];
typedef struct { uint64_t value[128]; unsigned count; } Set64;
static uint64_t hash8(const uint8_t *p) { uint64_t x; memcpy(&x,p,8); return x; }
static int set_add(Set64 *s,uint64_t key) {
    for (unsigned i=0;i<s->count;i++) if (s->value[i]==key) return 0;
    assert(s->count<128); s->value[s->count++]=key; return 1;
}
static int64_t plat_inc64(volatile int64_t *v) { return ++*v; }
static void unpack_answers(const uint8_t *p,GfxShaderAnswers *a) { (void)p; *a=input; }
const GfxShader *gfx_shader_get(int st,const uint8_t *ctl,const uint8_t *code,uint64_t len,
                               const uint8_t *c1,const GfxShaderAnswers *a) {
    (void)st; (void)ctl; (void)code; (void)len; (void)c1;
    if (failed && a->mvprev) return NULL;
    for (int i=0;i<count;i++) if (!memcmp(&requests[i],a,sizeof *a)) return &shaders[i];
    assert(count<128); requests[count]=*a;
    shaders[count].iid[0]=(uint8_t)(count+1); return &shaders[count++];
}
int gfx_shader_resources(const GfxShader *s,int k,const GfxShaderRes **out) {
    (void)s; *out=resources; return k==kind ? (kind==GFX_RES_UBO?1:32) : 0;
}
const uint32_t *gfx_shader_spirv(const GfxShader *s,uint64_t *n) { (void)s; *n=0; return NULL; }
uint32_t gfx_shader_inputs(const GfxShader *s,GfxShaderIo out[32]) { (void)s; (void)out; return 0; }
'''
    checks = r'''
int main(void) {
    uint8_t program[44+CONTROL_SIZE]={0}, answer[ANS_REC]={0};
    PrgIdx idx={program}; SeedVariants ids[1];
    for (int test=0;test<5;test++) {
        memset(ids,0,sizeof ids); memset(&input,0,sizeof input); memset(shaders,0,sizeof shaders);
        count=0; failed=test==3; kind=test==0?GFX_RES_TEX:GFX_RES_UBO;
        uint32_t st=test==0?GFX_STAGE_CS:GFX_STAGE_VS; memcpy(program,&st,4);
        answer[0]=(uint8_t)st; answer[ANS_REC-1]=SEED_DEFAULT;
        for (int i=0;i<32;i++) resources[i]=(GfxShaderRes){.slot=i,.type=3};
        if (test) resources[0].slot=test==2?4:7;
        if (test==4) input.mvprev=1;
        Prep p={.progs={&idx,1},.ans=answer,.n_ans=1,.ids=ids};
        prep_worker(&p);
        assert(p.done==1 && p.failed==0 && p.missing==0);
        assert(ids[0].count==(test==0?34:test==1?2:1));
        check_records(ids,answer,1);
        for (int i=0;i<ids[0].count;i++) {
            assert(ids[0].id[i][0]);
            for (int j=i+1;j<ids[0].count;j++) assert(memcmp(ids[0].id[i],ids[0].id[j],32));
        }
        // Returning an already prepared translation must not append another ID.
        int before=ids[0].count; seed_variant_add(ids,&shaders[ids[0].id[0][0]-1]);
        assert(ids[0].count==before);
        // Recorded-play translation does not broaden its requested bind state.
        p=(Prep){.progs={&idx,1},.ans=answer,.n_ans=1};
        int calls=count; prep_worker(&p); assert(count==calls);
    }
    puts("Production preparation worker retained alternatives, excluded unavailable variants and reused IDs");
}
'''
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepared-variants-', dir=build) as folder:
        folder = Path(folder)
        c = folder / 'check.c'
        records = '''
#define PRP_REC 36
static void check_records(SeedVariants *ids,const uint8_t *ab,long long n) {
    uint8_t recs[SEED_VARIANTS * PRP_REC]={0};
''' + record_loop.replace('recs && i < n', 'i < n') + '''
    assert(u==ids[0].count);
    for (size_t i=0;i<u;i++) {
        assert(recs[i*PRP_REC]==ab[0] && recs[i*PRP_REC+1]==0);
        assert(!memcmp(recs+i*PRP_REC+4,ids[0].id[i],32));
    }
}
'''
        c.write_text(harness + worker + records + checks)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                        '-I' + str(ROOT / 'runtime'), str(c), str(ROOT / 'runtime/gfx_shader_guess.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
