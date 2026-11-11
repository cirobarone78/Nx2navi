"""Build and run tests/import_hold_check.c: the bookkeeping of the import hold (runtime/import_hold.c) with a model of
guest memory, one thread and then a submitting, a game and a completion thread, with the platform layer linked. No game
files, no Vulkan.

Usage: import_hold_check.py [thread | address,undefined]
With an argument the test is built with that -fsanitize= (clang on Linux) and runs a fifth of the operations.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    sanitize = sys.argv[1] if len(sys.argv) > 1 else ''
    work = ROOT / 'build' / 'test' / ('import_hold' + ('-' + sanitize.replace(',', '-') if sanitize else ''))
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('import_hold_check.exe' if win else 'import_hold_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    # platform/linux.c's clipboard calls SDL3, which the runtime links
    libs = ['-lbcrypt', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread', '-lSDL3']
    san = ['-fsanitize=' + sanitize, '-fno-omit-frame-pointer', '-fno-sanitize-recover=all'] if sanitize else []
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', *san, '-I', str(rt),
                           str(ROOT / 'tests' / 'import_hold_check.c'), *map(str, plat), '-o', str(exe), '-lm', *libs],
                          env=env)
    if sanitize:
        env.setdefault('TSAN_OPTIONS', 'halt_on_error=1')
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=1')
    return subprocess.run([str(exe), '0.2' if sanitize else '1'], env=env, timeout=600).returncode


if __name__ == '__main__':
    sys.exit(main())
