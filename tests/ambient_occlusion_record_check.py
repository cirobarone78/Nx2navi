"""Compile and exercise real AO draw/record bodies with offline recording stubs."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'(?m)^(?:static )?[^\n;{}]+\b' + name + r'\([^;]*?\)\s*\{', source)
    if not match:
        raise RuntimeError(f'missing AO function {name}')
    end, depth = match.end(), 1
    while depth:
        if end >= len(source):
            raise RuntimeError(f'unterminated AO function {name}')
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    source = (ROOT / 'runtime/vk_ao.c').read_text(encoding='utf-8')
    state = source[source.index('// BEGIN AO state'):source.index('static int ao_mode')]
    names = ['ao_mode', 'projection_ok', 'scene_reset', 'scene_note', 'scene_pick_depth', 'scene_pick',
             'vk_ao_draw', 'vk_ao_frame', 'vk_ao_image_gone', 'barrier', 'vk_ao_record']
    fixture = (ROOT / 'tests/ambient_occlusion_record_fixture.c').read_text(encoding='utf-8')
    fixture = fixture.replace('/* AO_STATE */', state).replace('/* AO_FUNCTIONS */',
              '\n'.join(function(source, name) for name in names))
    out = ROOT / 'build/test/ambient-occlusion-record'
    out.mkdir(parents=True, exist_ok=True)
    src, exe = out / 'record.c', out / 'record.exe'
    src.write_text(fixture, encoding='utf-8')
    cc = shutil.which(args.cc) or args.cc
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'runtime'), '-I' + str(ROOT / 'include'),
                    '-I' + str(Path(cc).resolve().parent.parent / 'include/vma'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
