#!/usr/bin/env python3
"""Build the optional helper against an externally supplied SPIRV-Tools installation."""
import argparse,os,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--toolchain',type=Path,required=True,help='UCRT64 prefix containing bin/clang++.exe')
p.add_argument('--spirv-tools',type=Path,required=True,help='Prefix containing include/spirv-tools and lib/*.a')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
source=Path(__file__).with_name('bridge.cpp')
libs=[a.spirv_tools/'lib'/f'lib{x}.a' for x in ('SPIRV-Tools-opt','SPIRV-Tools')]
for f in libs:
 if not f.is_file():p.error(f'Missing library: {f}')
compiler=a.toolchain/'bin'/'clang++.exe'
env=os.environ.copy();env['PATH']=str(compiler.parent)+os.pathsep+env.get('PATH','')
subprocess.run([str(compiler),'-O2','-std=c++17','-shared',str(source),'-I'+str(a.spirv_tools/'include'),*[str(f) for f in libs],'-static-libgcc','-static-libstdc++','-o',str(a.out/'nx2navi_spirv_opt.dll')],env=env,check=True)
license=source.parents[2]/'runtime/licenses/spirv-tools-Apache-2.0.txt'
shutil.copy2(license,a.out/'spirv-tools-LICENSE.txt')
