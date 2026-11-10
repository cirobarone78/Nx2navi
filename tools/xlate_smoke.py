"""Smoke test for build/xlate/nx2navi_xlate(.exe): no game files needed.

Sends one hand-assembled Maxwell fragment shader (writes the constant colour 1.0, 0.5, 0.25, 1.0 to render target 0)
through --serve, checks the reply, then sends a malformed line and checks the service survives and still answers. A
hand-assembled compute shader that loads a texel from a float image and stores it back follows: the translator
declares such an image without a format, and its read needs StorageImageReadWithoutFormat. Every module is validated
with spirv-val.

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
RZ = 0xFF
IMAGE0 = 0x120 // 4  # first NVN image handle in c[2] (tools/xlate/Program.cs ImgSlot): image slot 0


def fragment_code() -> bytes:
    header = [0] * 20
    header[0] = 2 | (3 << 5) | (5 << 10)  # SPH type 2, version 3, stage fragment
    header[18] = 0xF  # OMAP: render target 0, components xyzw
    words = [SCHED, mov32i(0, 1.0), mov32i(1, 0.5), mov32i(2, 0.25),
             SCHED, mov32i(3, 1.0), EXIT, 0]
    return struct.pack("<20I", *header) + struct.pack(f"<{len(words)}Q", *words)


SULD_P, SUST_P = 0x1D61, 0x1D65  # opcode bits 51-63 of SULD.P and SUST.P


def surface_p(opcode: int, data: int, coord: int, handle: int) -> int:
    """SULD.P / SUST.P 1D RGBA: the data registers from `data` on, coordinate register, image handle (TidB)."""
    no_pred_out = 7 << 30 if opcode == SULD_P else 0  # SULD's second destination predicate: PT, none written
    return (opcode << 51) | (handle << 36) | no_pred_out | (0xF << 20) | PT | (coord << 8) | data


def image_read_code() -> bytes:
    """Compute code: R0-R3 = image 0 at x 0 (SULD.P), stored back there (SUST.P)."""
    words = [SCHED, surface_p(SULD_P, 0, RZ, IMAGE0), surface_p(SUST_P, 0, RZ, IMAGE0), EXIT]
    return struct.pack(f"<{len(words)}Q", *words)


def main() -> int:
    if not EXE.is_file():
        print(f"missing {EXE}; run tools/build_xlate.py", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        code = tmp / "ps.bin"
        code.write_bytes(fragment_code())
        image_code = tmp / "cs.bin"
        image_code.write_bytes(image_read_code())
        out = tmp / "out"
        job = {"key": "const_ps", "stage": "ps", "code": str(code), "debug": False, "fout": {"0": "float"}}
        image_job = {"key": "image_read_cs", "stage": "cs", "code": str(image_code), "debug": False, "local": [1, 1, 1]}
        lines = [
            json.dumps({"job": job, "out": str(out)}),
            "this is not json",
            json.dumps({"job": dict(job, key="again"), "out": str(out)}),
            json.dumps({"job": image_job, "out": str(out)}),
        ]
        r = subprocess.run([str(EXE), "--serve"], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=60)
        replies = r.stdout.splitlines()
        print("replies:", replies)
        if (len(replies) != 4 or replies[0] != "ok const_ps" or not replies[1].startswith("fail ") or replies[2] != "ok again"
                or replies[3] != "ok image_read_cs"):
            print("unexpected replies; stderr:", r.stderr, file=sys.stderr)
            return 1
        failed = 0
        for key in ("const_ps", "image_read_cs"):
            spv = out / f"{key}.spv"
            data = spv.read_bytes()
            if len(data) < 20 or struct.unpack("<I", data[:4])[0] != 0x07230203:
                print(f"{key}: reply is not SPIR-V", file=sys.stderr)
                return 1
            v = subprocess.run([str(SPIRV_VAL), "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True)
            print(f"{key}: spirv-val exit {v.returncode} ({len(data)} bytes) {v.stdout}{v.stderr}")
            failed |= v.returncode != 0
        return int(failed)


if __name__ == "__main__":
    sys.exit(main())
