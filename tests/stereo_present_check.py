"""Compile the actual stereo presentation component with recording Vulkan/VMA stubs."""
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
    compiler = shutil.which(args.cc) or args.cc
    assert (ROOT / 'runtime/vk_stereo_present.c').is_file(), 'Stereo compositor is not implemented'
    build = ROOT / 'build/test/stereo-present'
    build.mkdir(parents=True, exist_ok=True)
    output = build / ('check.exe' if os.name == 'nt' else 'check')
    includes = ['-I' + str(ROOT / 'runtime')]
    vma = Path(compiler).resolve().parent.parent / 'include/vma'
    if vma.is_dir():
        includes += ['-I' + str(vma)]
    subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', *includes,
                    str(ROOT / 'tests/stereo_present_fixture.c'), '-o', str(output)], check=True, timeout=30)
    subprocess.run([str(output)], check=True, timeout=10)


if __name__ == '__main__':
    main()
