"""Compile and validate the stereo shader's production descriptor/push ABI."""
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT / 'runtime/shaders/stereo.comp'
    assert source.is_file(), 'Stereo shader is not implemented'
    compiler = os.environ.get('GLSLC') or shutil.which('glslc')
    validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
    assert compiler and validator, 'Set GLSLC and SPIRV_VAL to the shader tools'
    output = ROOT / 'build/test/stereo/stereo.spv'
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, '-O', '--target-env=vulkan1.1', '-fshader-stage=compute',
                    str(source), '-o', str(output)], check=True, timeout=30)
    subprocess.run([validator, '--target-env', 'vulkan1.1', str(output)], check=True, timeout=30)
    data = output.read_bytes()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    assert words[0] == 0x07230203
    decorations, members, variables, pointers, structs = {}, {}, {}, {}, {}
    local = None
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 65535
        assert count > 0 and i + count <= len(words)
        args = words[i + 1:i + count]
        if opcode == 71:  # OpDecorate
            decorations.setdefault(args[0], {})[args[1]] = args[2:]
        elif opcode == 72 and args[2] == 35:  # OpMemberDecorate Offset
            members.setdefault(args[0], {})[args[1]] = args[3]
        elif opcode == 59:  # OpVariable: result type, result id, storage class
            variables[args[1]] = (args[0], args[2])
        elif opcode == 32:  # OpTypePointer
            pointers[args[0]] = (args[1], args[2])
        elif opcode == 30:  # OpTypeStruct
            structs[args[0]] = args[1:]
        elif opcode == 16 and args[1] == 17:  # OpExecutionMode LocalSize
            local = args[2:]
        i += count
    bindings = {deco[33][0] for ident, deco in decorations.items()
                if ident in variables and deco.get(34) == (0,) and 33 in deco}
    assert bindings == {0, 1, 2}, bindings
    push = [pointers[type_id][1] for type_id, storage in variables.values() if storage == 9]
    assert len(push) == 1 and len(structs[push[0]]) == 4
    assert members[push[0]] == {0: 0, 1: 16, 2: 32, 3: 48}, members
    assert local == (8, 8, 1), local
    print('Stereo shader: optimized Vulkan 1.1 validation, bindings 0/1/2, 64-byte ABI and 8x8 workgroup passed')


if __name__ == '__main__':
    main()
