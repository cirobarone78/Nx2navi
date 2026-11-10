"""Build and run tests/saves_page_check.cpp: the Saves tab's file window (runtime/ui/saves_page.cpp) without the game,
driven with the key events the menu hands Dear ImGui. No confirmation opens with the cursor on the button that acts;
A or Enter pressed again, B then A, A, or a held key with its repeats never deletes, restarts, copies or renames.

Usage: saves_page_check.py   (needs clang++ or g++; no game files)
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
SOURCES = [ROOT / 'tests' / 'saves_page_check.cpp', RT / 'ui' / 'option_rows.cpp', RT / 'ui' / 'menu_skin.cpp',
           RT / 'ui' / 'menu_text_input.cpp',
           RT / 'ui_text.c'] + sorted((RT / 'third_party' / 'imgui').glob('imgui*.cpp'))


def main():
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        sys.exit('needs clang++ or g++ on PATH')
    work = ROOT / 'build' / 'test' / 'saves_page'
    work.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in SOURCES:
        obj = work / (src.stem + '.o')
        lang = ['-x', 'c', '-std=gnu11'] if src.suffix == '.c' else ['-x', 'c++', '-std=gnu++17']
        if subprocess.run([cxx, *lang, '-c', '-O1', '-g', '-w', '-I', str(RT), '-I', str(RT / 'third_party' / 'imgui'),
                           str(src), '-o', str(obj)]).returncode:
            sys.exit(f'{src.name} did not compile')
        objs.append(str(obj))
    exe = work / ('saves_page_check.exe' if os.name == 'nt' else 'saves_page_check')
    if subprocess.run([cxx, *objs, '-o', str(exe), '-lm']).returncode:
        sys.exit('the link failed')
    r = subprocess.run([str(exe)])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
