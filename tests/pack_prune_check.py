"""Run the production carry_repack function against shared archive spans, without game files.

Usage: python tests/pack_prune_check.py --cc /path/to/clang
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
    code = (ROOT / 'runtime/gfx_shader.c').read_text()
    start = code.index('typedef struct { uint64_t offset; uint32_t size, entry; } PackSpan;')
    end = code.index('\nstatic void carry_main(', start)
    production = code[start:end]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PATH_CAP 1024
#define PACK_TRAILER 48
#define PACK_SRC_MAGIC "TKSPSRC1"
typedef FILE PlatFile;
typedef struct { uint64_t size; } PlatFileId;
typedef struct { uint8_t name[32]; uint32_t kind, size; uint64_t offset; } PackEnt;
typedef struct { uint32_t kind; uint8_t name[32]; } PackDrop;
typedef struct { PackDrop *v; size_t n; } Drops;
static PlatFile *pack_file;
static PackEnt *pack_ix;
static uint32_t pack_n, pack_flags = 1;
static char cache_dir[PATH_CAP];
static int drop_cmp(const void *a, const void *b) {
    const PackDrop *x=a, *y=b;
    return x->kind != y->kind ? (x->kind < y->kind ? -1 : 1) : memcmp(x->name,y->name,32);
}
static int plat_file_id(PlatFile *f, PlatFileId *id) { fseek(f,0,SEEK_END); id->size=ftell(f); return 1; }
static int pruned_of(const PlatFileId *id, uint64_t *have) { (void)id; *have=0; return 0; }
static void file_id_put(uint8_t *p, const PlatFileId *id) { memset(p,0,40); memcpy(p,&id->size,8); }
static void plat_file_delete(const char *p) { remove(p); }
static PlatFile *plat_fs_create(const char *p,int *err) { *err=0; return fopen(p,"wb"); }
static int64_t plat_file_pwrite(PlatFile *f,const void *p,uint32_t n,uint64_t at) {
    if (fseek(f,(long)at,SEEK_SET)) return -1;
    return (int64_t)fwrite(p,1,n,f);
}
static int pack_read_at(uint64_t at,void *p,uint32_t n) {
    return !fseek(pack_file,(long)at,SEEK_SET) && fread(p,1,n,pack_file)==n;
}
static int plat_file_fsync(PlatFile *f) { return !fflush(f); }
static void plat_file_close(PlatFile *f) { fclose(f); }
static int plat_file_replace(const char *a,const char *b,int sync) { (void)sync; return !rename(a,b); }
static void carry_wait(void) {}
#define rt_log(...) ((void)0)
'''
    checks = r'''
int main(int argc,char **argv) {
    assert(argc==2); snprintf(cache_dir,sizeof cache_dir,"%s",argv[1]);
    PackEnt entries[5]={0}; pack_ix=entries; pack_n=5;
    uint32_t sizes[5]={16,16,16,4*1024*1024+7,5};
    uint64_t offsets[5]={256,256,272,288,288+4*1024*1024+7};
    pack_file=tmpfile(); assert(pack_file);
    for (int i=0;i<5;i++) {
        entries[i].kind=1; entries[i].name[0]=(uint8_t)i;
        entries[i].size=sizes[i]; entries[i].offset=offsets[i];
        unsigned char *data=malloc(sizes[i]); assert(data);
        memset(data,i==1?0:i,sizes[i]);
        assert(plat_file_pwrite(pack_file,data,sizes[i],offsets[i])==sizes[i]); free(data);
    }
    for (int mode=0;mode<3;mode++) {
        PackDrop dv[5]={0}; Drops d={dv,mode==0?0:mode==1?1:5};
        for (size_t i=0;i<d.n;i++) { dv[i].kind=1; dv[i].name[0]=(uint8_t)i; }
        assert(carry_repack(&d));
        char out[PATH_CAP+48]; snprintf(out,sizeof out,"%s/shader-cache.pack.pruned",cache_dir);
        FILE *f=fopen(out,"rb"); assert(f);
        unsigned char head[16]; assert(fread(head,1,16,f)==16);
        uint32_t n,flags; memcpy(&n,head+8,4); memcpy(&flags,head+12,4);
        assert(n==5-d.n && flags==1);
        PackEnt ix[5]; assert(fread(ix,sizeof *ix,n,f)==n);
        if (!mode) assert(ix[0].offset==ix[1].offset);
        for (uint32_t i=0;i<n;i++) {
            unsigned id=ix[i].name[0]; assert(id==i+d.n && ix[i].size==sizes[id]);
            assert(!fseek(f,(long)ix[i].offset,SEEK_SET));
            for (uint32_t j=0;j<ix[i].size;j++) assert(fgetc(f)==(int)(id==1?0:id));
        }
        fseek(f,0,SEEK_END);
        uint64_t payload=mode==2?0:16+16+sizes[3]+5;
        assert((uint64_t)ftell(f)==16+48*n+payload+PACK_TRAILER);
        fclose(f); assert(!remove(out));
    }
    fclose(pack_file); puts("Production carry pruning preserved shared spans, remaining aliases and large payloads");
}
'''
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pack-prune-', dir=build) as folder:
        folder = Path(folder)
        source = folder / 'check.c'
        source.write_text(harness + production + checks)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(folder)], check=True, timeout=60)


if __name__ == '__main__':
    main()
