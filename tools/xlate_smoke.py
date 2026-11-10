"""Smoke test for build/xlate/nx2navi_xlate(.exe): no game files needed.

Sends hand-assembled Maxwell and SM86 fragment shaders through --serve, checks their numeric contracts and
validates every result with spirv-val. Covers image formats, typed texture fetch/sample/gather and channel
masks, dimension queries, signed integer conversion, and recovery after malformed JSON. No game files.

  python tools/xlate_smoke.py [--translator /path/to/nx2navi_xlate]
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "xlate" / ("nx2navi_xlate.exe" if os.name == "nt" else "nx2navi_xlate")
SPIRV_VAL = (Path(r"C:\msys64\ucrt64\bin\spirv-val.exe") if os.name == "nt"
             else Path(shutil.which("spirv-val") or "/usr/bin/spirv-val"))

PT = 7 << 16  # predicate "always"


def mov32i(dest: int, value: float) -> int:
    imm = struct.unpack("<I", struct.pack("<f", value))[0]
    return 0x0100000000000000 | (imm << 20) | (0xF << 12) | PT | dest


EXIT = 0xE30000000000000F | PT
SCHED = 0x001F8000FC0007E0  # scheduling control word; the decoder skips it


def fragment_code(instructions: list[int], sm86: bool = False, output_mask: int = 15) -> bytes:
    header = [0] * (32 if sm86 else 20)
    header[0] = 2 | (3 << 5) | (5 << 10)  # SPH type 2, version 3, stage fragment
    header[18] = output_mask
    if sm86:
        return struct.pack("<32I", *header) + b"".join(word.to_bytes(16, "little") for word in instructions)
    words = []
    instructions = instructions + [EXIT]
    for start in range(0, len(instructions), 3):
        group = instructions[start:start + 3]
        words.extend([SCHED, *group, *([0] * (3 - len(group)))])
    return struct.pack("<20I", *header) + struct.pack(f"<{len(words)}Q", *words)


def shader_cases() -> list[tuple[str, list[int], dict]]:
    constant = [mov32i(0, 1.0), mov32i(1, 0.5), mov32i(2, 0.25), mov32i(3, 1.0)]
    cases = [("const_ps", constant, {})]
    # TXQ query 1, mask xyz, c[2][0x168] (texture slot 0), LOD zero from RZ.
    # Requesting z on 2D/cube previously emitted CompositeExtract of vec2 at index 2.
    txq = 0xDF48000000000000 | (0x5A << 36) | (7 << 31) | (1 << 22) | (255 << 8) | PT
    for kind in ("2d", "2dms", "cube", "2darray", "2dmsarray", "cubearray", "3d"):
        cases.append(("txq_" + kind, [txq, mov32i(3, 1.0)], {"tex": {"0": kind}}))
    # SULD.2D.RGBA, c[2][0x120] (image slot 0), integer coordinates zero from RZ.
    suld = 0xEB08000000000000 | (0x48 << 36) | (3 << 33) | (7 << 30) | (15 << 20) | (255 << 8) | PT
    # SPIR-V ImageFormat values: test every supported integer storage format, not only Rg16ui.
    image_formats = (("R8G8B8A8Unorm", 0, "float"), ("R32Uint", 33, "uint"),
                     ("R16G16Uint", 36, "uint"), ("R16G16Sint", 26, "sint"),
                     ("R8Uint", 39, "uint"), ("R16Uint", 38, "uint"), ("R8G8Uint", 37, "uint"),
                     ("R32G32Uint", 35, "uint"), ("R10G10B10A2Uint", 34, "uint"),
                     ("R8G8B8A8Uint", 32, "uint"), ("R16G16B16A16Uint", 31, "uint"),
                     ("R32G32B32A32Uint", 30, "uint"), ("R8Sint", 29, "sint"),
                     ("R16Sint", 28, "sint"), ("R8G8Sint", 27, "sint"), ("R32G32Sint", 25, "sint"),
                     ("R32Sint", 24, "sint"), ("R8G8B8A8Sint", 23, "sint"),
                     ("R16G16B16A16Sint", 22, "sint"), ("R32G32B32A32Sint", 21, "sint"),
                     ("R16G16Float", 0, "float"))  # Formatted float loads deliberately declare Unknown.
    for fmt, image_format, component in image_formats:
        cases.append(("image_" + fmt, [suld],
                      {"img": {"0": {"type": "2d", "fmt": fmt}},
                       "tex": {"0": {"type": "2d", "fmt": "R32Uint" if component == "float" else "R32Float"}},
                       "_check": {"storage_format": (image_format, component)}}))
    # Combined-handle byte 0x40 maps to texture slot 4, independently of storage-image slot 4.
    # Poison the image answer to detect accidental sharing of the two format namespaces.
    for kind, fmt, component in (("uint", "R16G16Uint", "uint"), ("sint", "R16G16Sint", "sint"),
                                 ("float", "R32Float", "float")):
        for operation, base, opcode, masks in (("fetch", 0xDC00000000000000, 95, (15, 4, 5)),
                                               ("sample", 0xC080000000000000, 88, (15,)),
                                               ("gather", 0xC800000000000000, 96, (15,))):
            for mask in masks if kind != "float" else (15,):
                inst = base | (0x10 << 36) | (2 << 28) | (mask << 31) | (7 << 51) | (255 << 20) | (255 << 8) | PT
                options = {"tex": {"4": {"type": "2d", "fmt": fmt}},
                           "img": {"4": {"type": "2d", "fmt": "R32Float" if kind != "float" else "R32Uint"}},
                           "_check": {"sample": (component, opcode, mask), "format": fmt}}
                cases.append((f"texture_{operation}_{kind}_{mask}", constant + [inst], options))
    depth = 0xC080000000000000 | (0x10 << 36) | (2 << 28) | (1 << 31) | (7 << 51) | (1 << 50) | (255 << 20) | (255 << 8) | PT
    cases.append(("texture_depth", constant + [depth],
                  {"tex": {"4": {"type": "2d_depth", "fmt": "R32Float"}},
                   "_check": {"sample": ("float", 90, 1), "format": "R32Float"}}))
    # -1 interpreted as S32 becomes -1.0; the same U32 word rounds to 2**32 in F32.
    # Integer output keeps the resulting float bits, bypassing the float-output clamp.
    move = 0x802 | (7 << 12) | (4 << 16) | (0xFFFFFFFF << 32) | (15 << 72)
    for signed, expected in ((True, 0xBF800000), (False, 0x4F800000)):
        convert = 0x306 | (7 << 12) | (4 << 32) | (int(signed) << 74) | (2 << 75) | (2 << 84)
        cases.append(("sm86_i2f_" + ("signed" if signed else "unsigned"), [move, convert],
                      {"sm86": True, "fout": {"0": "uint"}, "_omap": 1,
                       "_check": {"conversion_bits": expected}}))
    return cases


def spirv_instructions(data: bytes) -> list[tuple[int, tuple[int, ...]]]:
    words = struct.unpack(f"<{len(data) // 4}I", data)
    instructions = []
    offset = 5
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if count == 0 or offset + count > len(words):
            raise ValueError("invalid SPIR-V instruction length")
        instructions.append((opcode, words[offset + 1:offset + count]))
        offset += count
    return instructions


def check_numeric_contract(instructions: list[tuple[int, tuple[int, ...]]], check: dict, resources: dict) -> None:
    types = {args[0]: (opcode, args[1:]) for opcode, args in instructions if opcode in (21, 22, 23, 25)}
    value_ops = {1, 43, 44, 46, 61, 65, 79, 80, 81, 83, 87, 88, 89, 90, 95, 96, 97, 98, 111, 112, 124}
    values = {args[1]: (opcode, args) for opcode, args in instructions if opcode in value_ops}

    def component(type_id: int) -> tuple[str, int]:
        opcode, args = types[type_id]
        if opcode == 23:  # OpTypeVector
            return component(args[0])[0], args[1]
        assert opcode in (21, 22) and args[0] == 32, "expected 32-bit numeric component"
        return ("float" if opcode == 22 else "sint" if args[1] else "uint"), 1

    if "storage_format" in check:
        image_format, kind = check["storage_format"]
        images = [args for opcode, args in types.values() if opcode == 25 and args[5] == 2]
        assert len(images) == 1 and images[0][6] == image_format, "storage image format answer changed"
        assert component(images[0][0]) == (kind, 1), "wrong storage image numeric type"
        assert resources["img_queried"] == [0] and not resources["tex_queried"], "image used sampled-texture format answers"
        capabilities = {args[0] for opcode, args in instructions if opcode == 17}
        # StorageImageExtendedFormats=49; Shader-only integer formats are rgba* and r32*.
        extended = image_format in (*range(6, 21), *range(25, 30), *range(34, 40))
        assert (49 in capabilities) == extended, "incorrect extended storage image format capability"
        assert (55 in capabilities) == (image_format == 0), "incorrect unformatted image read capability"
        assert any(opcode == 98 for opcode, _ in instructions), "storage image read was dropped"

    if "sample" in check:
        kind, sample_opcode, mask = check["sample"]
        images = [args for opcode, args in types.values() if opcode == 25 and args[5] == 1]
        assert len(images) == 1 and component(images[0][0]) == (kind, 1), "wrong sampled image numeric type"
        assert images[0][6] == 0, "sampled image format must remain Unknown"
        assert images[0][2] == int(sample_opcode == 90), "depth-compare image declaration changed"
        samples = {args[1] for opcode, args in instructions if opcode == sample_opcode}
        assert samples, "expected texture operation was dropped"
        for sample in samples:
            assert component(values[sample][1][0]) == (kind, 1 if sample_opcode == 90 else 4), "wrong texture result type"
        assert resources["tex_queried"] == [4] and not resources["img_queried"], "combined texture handle used image answers"
        assert any(texture["format"] == check["format"] for texture in resources["textures"]), "texture format answer lost"

        def lanes(value: int) -> tuple[int, ...] | None:
            if value in samples:
                return (0,) if sample_opcode == 90 else (0, 1, 2, 3)
            if value not in values:
                return None
            opcode, args = values[value]
            if opcode in (83, 124):  # CopyObject / Bitcast preserve all bits and lanes.
                return lanes(args[2])
            if opcode == 81:
                source = lanes(args[2])
                return None if source is None else (source[args[3]],)
            if opcode == 79:
                left, right = lanes(args[2]), lanes(args[3])
                return None if left is None or right is None else tuple((left + right)[index] for index in args[4:])
            return None

        if kind != "float":
            selected = tuple(index for index in range(4) if mask & (1 << index))
            casts = [lanes(args[2]) for opcode, args in instructions
                     if opcode == 124 and component(args[0])[0] == "float" and lanes(args[2]) is not None]
            assert selected in casts or tuple(index for cast in casts for index in cast) == selected, "integer sample bits or channel mask lost"
            assert not any(opcode in (111, 112) and lanes(args[2]) is not None
                           for opcode, args in instructions), "integer texture result was numerically converted to float"

    if "conversion_bits" in check:
        # Evaluate only the constant conversion's output slice, not Maxwell instruction flags.
        # This accepts either an emitted conversion or its constant-folded SPIR-V equivalent.
        memory = {args[0]: args[1] for opcode, args in instructions if opcode == 62}
        outputs = {args[1] for opcode, args in instructions if opcode == 59 and args[2] == 3}

        def output_pointer(value: int) -> bool:
            if value in outputs:
                return True
            opcode, args = values.get(value, (0, ()))
            return opcode == 65 and output_pointer(args[2])

        def bits(value: int):
            opcode, args = values[value]
            if opcode == 43:
                return args[2]
            if opcode == 46:  # OpConstantNull
                return 0
            if opcode == 1:  # OpUndef
                return None
            if opcode in (83, 124):
                return bits(args[2])
            if opcode == 61:
                return bits(memory[args[2]])
            if opcode in (44, 80):
                return tuple(bits(source) for source in args[2:])
            if opcode == 81:
                return bits(args[2])[args[3]]
            if opcode in (111, 112):
                number = bits(args[2])
                if opcode == 111 and number & 0x80000000:
                    number -= 1 << 32
                return struct.unpack("<I", struct.pack("<f", float(number)))[0]
            raise AssertionError(f"unexpected operation {opcode} in constant I2F output")

        actual = []
        for opcode, args in instructions:
            if opcode == 62 and output_pointer(args[0]):
                value = bits(args[1])
                actual.extend(value if isinstance(value, tuple) else (value,))
        assert actual and {value for value in actual if value is not None} == {check["conversion_bits"]}, "signed/unsigned I2F output value changed"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--translator", type=Path, default=EXE, help="translator executable; default: build/xlate")
    translator = parser.parse_args().translator.resolve()
    if not translator.is_file():
        print(f"missing {translator}; run tools/build_xlate.py or pass --translator", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        out = tmp / "out"
        jobs = []
        checks = {}
        for key, instructions, options in shader_cases():
            options = dict(options)
            checks[key] = options.pop("_check", {})
            output_mask = options.pop("_omap", 15)
            code = tmp / (key + ".bin")
            code.write_bytes(fragment_code(instructions, options.get("sm86", False), output_mask))
            jobs.append({"key": key, "stage": "ps", "code": str(code), "debug": False,
                         "fout": {"0": "float"}, **options})
        lines = [json.dumps({"job": job, "out": str(out)}) for job in jobs]
        lines.extend(["this is not json", json.dumps({"job": dict(jobs[0], key="again"), "out": str(out)})])
        r = subprocess.run([str(translator), "--serve"], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=60)
        replies = r.stdout.splitlines()
        print("replies:", replies)
        expected = ["ok " + job["key"] for job in jobs]
        if (len(replies) != len(expected) + 2 or replies[:-2] != expected
                or not replies[-2].startswith("fail ") or replies[-1] != "ok again"):
            print("unexpected replies; stderr:", r.stderr, file=sys.stderr)
            return 1
        for job in jobs:
            spv = out / (job["key"] + ".spv")
            data = spv.read_bytes()
            if len(data) < 20 or struct.unpack("<I", data[:4])[0] != 0x07230203:
                print("reply is not SPIR-V", file=sys.stderr)
                return 1
            v = subprocess.run([str(SPIRV_VAL), "--relax-block-layout", "--uniform-buffer-standard-layout",
                                "--scalar-block-layout", "--allow-localsizeid", "--target-env", "vulkan1.3",
                                str(spv)], capture_output=True, text=True)
            print(f"{job['key']}: spirv-val exit {v.returncode} ({len(data)} bytes) {v.stdout}{v.stderr}")
            if v.returncode:
                return v.returncode
            module = spirv_instructions(data)
            if checks[job["key"]]:
                resources = json.loads(spv.with_suffix(".res.json").read_text(encoding="utf-8"))
                try:
                    check_numeric_contract(module, checks[job["key"]], resources)
                except (AssertionError, KeyError, TypeError) as error:
                    print(f"{job['key']}: numeric contract failed: {error}", file=sys.stderr)
                    return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
