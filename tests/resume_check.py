"""Build and run tests/resume_check.c: runtime/resume.c, the menu's restart in place, without the game. Each check runs in
its own process:

- wait70: Restart while the game's cooldown runs 70 s more; the new process starts 5 s after the hand-over and looks at
  it 13 s later. The hand-over is taken.
- stale: the same hand-over when the new process starts 70 s after it is ignored.
- future1: the wall clock stepped back between the processes, so the hand-over lies 1 s after the new process's start
  and its first look. It is taken.
- race_a, race_b: the two orders in which a Cancel can meet the guest thread's autosave request, forced by stopping
  each side where the other runs. Nothing restarts.
- no_guest: no guest thread polls the pad while the menu is open; after about 2 s the confirmation offers the plain
  restart instead of a disabled button.

No game files: the module layout resume.c includes is a stand-in written to the test's build folder. --rev REV builds
the checks against runtime/resume.c and resume.h of that commit instead of the working tree.

Usage: resume_check.py [--rev REV] [--log]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
LAYOUT = '''// A stand-in for the generated layout.h (tests/resume_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rev', help='take runtime/resume.c and resume.h from this commit')
    ap.add_argument('--log', action='store_true', help="print resume.c's log lines")
    a = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'resume'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    first = []   # resume.c and resume.h of --rev, found before the working tree's
    if a.rev:
        src = work / 'rev'
        src.mkdir(exist_ok=True)
        for name in ('resume.c', 'resume.h'):
            (src / name).write_bytes(subprocess.check_output(['git', 'show', f'{a.rev}:runtime/{name}'], cwd=ROOT))
        first = ['-I', str(src)]
    exe = work / ('resume_check.exe' if os.name == 'nt' else 'resume_check')
    env = dict(os.environ, NX2NAVI_RESUME_CHECK_LOG='1' if a.log else '0')
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing', '-pthread',
                           *first, '-I', str(ROOT / 'runtime'), '-I', str(work),
                           str(ROOT / 'tests' / 'resume_check.c'), '-o', str(exe), '-lm'], env=env)
    names = subprocess.check_output([str(exe)], env=env, text=True).split()
    failed = []
    for name in names:
        rc = subprocess.run([str(exe), name], env=env).returncode
        print(f'{name}: {"ok" if rc == 0 else "FAILED"}', flush=True)
        if rc:
            failed.append(name)
    print(f'{len(names) - len(failed)} of {len(names)} checks passed' + (f'; failed: {" ".join(failed)}' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
