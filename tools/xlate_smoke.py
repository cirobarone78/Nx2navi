"""Smoke test for build/xlate/nx2navi_xlate(.exe): no game files needed.

Sends one hand-assembled Maxwell fragment shader (writes the constant colour 1.0, 0.5, 0.25, 1.0 to render target 0)
through --serve, checks the reply, validates the SPIR-V with spirv-val, then sends a malformed line and checks the
service survives and still answers. Then it sends a compute shader that adds 1.0 with RED.E.ADD.F32.FTZ.RN to a global
address twice, once without and once with the job flag "float_atomic_add": the first must come back as a
compare-and-swap loop, the second as one OpAtomicFAddEXT with its capability (translator patch 0039); both are validated.

  python tools/xlate_smoke.py
"""

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


def fragment_code() -> bytes:
    header = [0] * 20
    header[0] = 2 | (3 << 5) | (5 << 10)  # SPH type 2, version 3, stage fragment
    header[18] = 0xF  # OMAP: render target 0, components xyzw
    words = [SCHED, mov32i(0, 1.0), mov32i(1, 0.5), mov32i(2, 0.25),
             SCHED, mov32i(3, 1.0), EXIT, 0]
    return struct.pack("<20I", *header) + struct.pack(f"<{len(words)}Q", *words)


def mov_c(dest: int, offset: int) -> int:
    return 0x4C98078000000000 | ((offset // 4) << 20) | PT | dest   # MOV Rd, c[0x0][offset]


def red_f32_add(addr: int, data: int) -> int:
    return 0xEBF8000000000000 | (1 << 48) | (3 << 20) | (addr << 8) | PT | data   # RED.E.ADD.F32.FTZ.RN [Ra.64], Rb


def compute_code() -> bytes:
    """Compute program: [c0[0x140]] (a pointer, taken as a storage buffer) += 1.0, no SPH."""
    words = [SCHED, mov_c(0, 0x140), mov_c(1, 0x144), mov32i(2, 1.0),
             SCHED, red_f32_add(0, 2), EXIT, 0]
    return struct.pack(f"<{len(words)}Q", *words)


def opcodes(path: Path) -> dict[int, int]:
    """Instruction count per SPIR-V opcode, and under -1 - capability the declared capabilities."""
    words = struct.unpack(f"<{path.stat().st_size // 4}I", path.read_bytes())
    count: dict[int, int] = {}
    i = 5
    while i < len(words):
        op, n = words[i] & 0xFFFF, words[i] >> 16
        count[op] = count.get(op, 0) + 1
        if op == 17:
            count[-1 - words[i + 1]] = 1
        i += n
    return count


def check_float_atomic(tmp: Path) -> int:
    OP_ATOMIC_CAS, OP_ATOMIC_FADD, CAP_ATOMIC_FLOAT32_ADD = 230, 6035, 6033
    code = tmp / "cs.bin"
    code.write_bytes(compute_code())
    out = tmp / "cs_out"
    lines = []
    for key, native in (("red_loop", False), ("red_native", True)):
        job = {"key": key, "stage": "cs", "code": str(code), "debug": False, "local": [32, 1, 1], "shared": 0}
        if native:
            job["float_atomic_add"] = True
        lines.append(json.dumps({"job": job, "out": str(out)}))
    r = subprocess.run([str(EXE), "--serve"], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=60)
    if r.stdout.splitlines() != ["ok red_loop", "ok red_native"]:
        print("float atomic add: unexpected replies", r.stdout.splitlines(), r.stderr, file=sys.stderr)
        return 1
    for key, native in (("red_loop", False), ("red_native", True)):
        spv = out / f"{key}.spv"
        ops = opcodes(spv)
        want = (ops.get(OP_ATOMIC_FADD, 0) == 1 and ops.get(OP_ATOMIC_CAS, 0) == 0 and (-1 - CAP_ATOMIC_FLOAT32_ADD) in ops) if native \
            else (ops.get(OP_ATOMIC_FADD, 0) == 0 and ops.get(OP_ATOMIC_CAS, 0) == 1 and (-1 - CAP_ATOMIC_FLOAT32_ADD) not in ops)
        v = subprocess.run([str(SPIRV_VAL), "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True)
        print(f"{key}: {'OpAtomicFAddEXT' if ops.get(OP_ATOMIC_FADD) else 'compare-and-swap loop' if ops.get(OP_ATOMIC_CAS) else 'no atomic'}, "
              f"spirv-val exit {v.returncode} {v.stdout}{v.stderr}")
        if not want or v.returncode:
            print(f"float atomic add: {key} is not what the job asked for", file=sys.stderr)
            return 1
    return 0


def main() -> int:
    if not EXE.is_file():
        print(f"missing {EXE}; run tools/build_xlate.py", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        code = tmp / "ps.bin"
        code.write_bytes(fragment_code())
        out = tmp / "out"
        job = {"key": "const_ps", "stage": "ps", "code": str(code), "debug": False, "fout": {"0": "float"}}
        lines = [
            json.dumps({"job": job, "out": str(out)}),
            "this is not json",
            json.dumps({"job": dict(job, key="again"), "out": str(out)}),
        ]
        r = subprocess.run([str(EXE), "--serve"], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=60)
        replies = r.stdout.splitlines()
        print("replies:", replies)
        if len(replies) != 3 or replies[0] != "ok const_ps" or not replies[1].startswith("fail ") or replies[2] != "ok again":
            print("unexpected replies; stderr:", r.stderr, file=sys.stderr)
            return 1
        spv = out / "const_ps.spv"
        data = spv.read_bytes()
        if len(data) < 20 or struct.unpack("<I", data[:4])[0] != 0x07230203:
            print("reply is not SPIR-V", file=sys.stderr)
            return 1
        v = subprocess.run([str(SPIRV_VAL), "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True)
        print(f"spirv-val exit {v.returncode} ({len(data)} bytes) {v.stdout}{v.stderr}")
        if v.returncode:
            return v.returncode
        return check_float_atomic(tmp)


if __name__ == "__main__":
    sys.exit(main())
