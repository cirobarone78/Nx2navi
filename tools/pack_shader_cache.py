"""Fold the loose files of a shader cache into <cache>/shader-cache.pack and delete them.

The translator writes four files per shader into the cache (runtime/gfx_shader.c): <key>.spv, <key>.res.json,
<name>.use and in/<input id>.in, about 117,000 files after an install, which are slow to copy, back up, scan and sync.
nx2navi.exe reads the pack first and the loose files after it, and keeps writing new translations as loose files, so
this tool can run again at any time the game is closed: it merges the existing pack with the loose files (a loose file
wins over a packed one of the same name) and writes the pack anew. The installer runs it after preparing the shaders.

Layout, little-endian (gfx_shader.c, section "cache pack"): b'TKSPACK1', u32 count, u32 flags, then count entries of 48
bytes sorted by (kind, name): name (the 32 bytes the file name's 64 hex digits spell), u32 kind (1 .spv, 2 .res.json,
3 .use, 4 in/*.in), u32 size, u64 offset; then the data. Flag bit 0 marks exact-content deduplication.
Each lookup key retains its index entry; identical payloads of the same kind share their stored bytes.

    python tools\\pack_shader_cache.py <install folder>\\shader-cache [--keep]

--keep leaves the loose files in place. Close the game first: a running nx2navi.exe has read the old pack's index.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

MAGIC = b'TKSPACK1'
PACK = 'shader-cache.pack'
SHARED_PAYLOADS = 1
KINDS = {'.spv': 1, '.res.json': 2, '.use': 3, '.in': 4}
NAME = re.compile(r'([0-9a-f]{64})(\.spv|\.res\.json|\.use|\.in)')
USE_SIZE = 20   # gfx_shader.c use_store: magic, version, three masks
# Loose files are read on READERS threads, at most WINDOW ahead of the one being written. On Windows the time goes into
# opening each file (about 3 ms the first time, against 0.04 ms once the system has seen it), which threads overlap:
# read one at a time, the 175,000 files of a launcher's cache took minutes.
READERS = 16
WINDOW = 1024


def loose_files(cache):
    """{(kind, name): path} of the cache's loose files; *.in only in in/."""
    found = {}
    for folder, kinds in ((cache, (1, 2, 3)), (cache / 'in', (4,))):
        if not folder.is_dir():
            continue
        with os.scandir(folder) as it:
            for e in it:
                m = NAME.fullmatch(e.name)
                if not m or not e.is_file():
                    continue
                kind = KINDS[m.group(2)]
                if kind in kinds:
                    found[(kind, bytes.fromhex(m.group(1)))] = Path(e.path)
    return found


def read_pack(path):
    """{(kind, name): (offset, size)} of an existing pack; {} when there is none or it fails a check."""
    try:
        f = open(path, 'rb')
    except FileNotFoundError:
        return {}
    with f:
        size = os.fstat(f.fileno()).st_size
        head = f.read(16)
        if len(head) != 16 or head[:8] != MAGIC:
            raise ValueError(f'{path}: invalid shader cache header; original retained')
        count = struct.unpack_from('<I', head, 8)[0]
        if 16 + 48 * count > size:
            raise ValueError(f'{path}: truncated shader cache index')
        raw = f.read(48 * count)
        if len(raw) != 48 * count:
            raise ValueError(f'{path}: truncated shader cache index')
        out = {}
        for i in range(count):
            name, kind, length, offset = struct.unpack_from('<32sIIQ', raw, 48 * i)
            if offset < 16 + 48 * count or offset + length > size or (kind, name) in out:
                raise ValueError(f'{path}: invalid shader cache entry')
            out[(kind, name)] = (offset, length)
        return out


def valid(kind, data):
    return len(data) == USE_SIZE if kind == 3 else len(data) > 0


def read_loose(path):
    try:
        return path.read_bytes()
    except OSError:
        return None


def shared_pack(path):
    if not path.is_file():
        return False
    with path.open('rb') as stream:
        header = stream.read(16)
    return len(header) == 16 and header[:8] == MAGIC and struct.unpack_from('<I', header, 12)[0] & SHARED_PAYLOADS


def store_payload(stream, locations, kind, data, end):
    # Hashes select candidates; compare the actual bytes before sharing an offset, including on a hash collision.
    small = len(data) <= 64
    key = (kind, len(data), data if small else hashlib.sha256(data).digest())
    candidates = locations.setdefault(key, [])
    for offset in candidates:
        if small:
            return offset, end
        stream.seek(offset)
        if stream.read(len(data)) == data:
            return offset, end
    stream.seek(end)
    stream.write(data)
    candidates.append(end)
    return end, end + len(data)


def pack(cache, keep=False):
    cache = Path(cache)
    with (cache / '.pack.lock').open('a+b') as lock:
        if os.name == 'nt':
            import msvcrt
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            return _pack(cache, keep)
        finally:
            (cache / (PACK + '.tmp')).unlink(missing_ok=True)


def _pack(cache, keep):
    out = cache / PACK
    packed = read_pack(out)
    loose = loose_files(cache)
    if not loose and (not packed or shared_pack(out)):
        # Nothing to fold in: every file of the cache is already in the pack, so writing it again copies the same
        # bytes over the same entries (1.37 GiB here, 2.6 s, and a file system that then has the old pack's blocks to
        # release as well). The installer runs this after every prepare.
        return len(packed), 0, 0, out.stat().st_size if out.is_file() else 0
    keys = sorted(set(packed) | set(loose))
    tmp = out.with_name(PACK + '.tmp')
    index, written, skipped, offset = [], [], 0, 16 + 48 * len(keys)
    locations = {}
    old = open(out, 'rb') if packed else None
    try:
        with open(tmp, 'w+b') as f, ThreadPoolExecutor(READERS) as pool:
            f.write(bytes(offset))   # header and index, written once the offsets are known
            ahead, todo = deque(), iter(keys)

            def submit():
                key = next(todo, None)
                if key is not None:
                    ahead.append((key, pool.submit(read_loose, loose[key]) if key in loose else None))

            for _ in range(WINDOW):
                submit()
            while ahead:
                key, reading = ahead.popleft()
                submit()
                data = None
                if reading:   # a valid loose file, else the packed copy, else nothing
                    data = reading.result()
                    if data is not None and valid(key[0], data):
                        written.append(loose[key])
                    else:
                        data = None
                if data is None and key in packed:
                    old.seek(packed[key][0])
                    data = old.read(packed[key][1])
                    if len(data) != packed[key][1]:
                        raise OSError(f'{out}: incomplete read; original retained')
                if data is None or not valid(key[0], data):
                    skipped += 1
                    continue
                start, offset = store_payload(f, locations, key[0], data, offset)
                index.append(struct.pack('<32sIIQ', key[1], key[0], len(data), start))
            f.seek(0)
            f.write(MAGIC + struct.pack('<II', len(index), SHARED_PAYLOADS) + b''.join(index))
            f.flush()
            os.fsync(f.fileno())
    finally:
        if old:
            old.close()
    os.replace(tmp, out)
    if not keep:
        for p in written:
            p.unlink(missing_ok=True)
        try:
            (cache / 'in').rmdir()   # only when empty
        except OSError:
            pass
    return len(index), len(written), skipped, offset


def main():
    ap = argparse.ArgumentParser(description='Fold a shader cache into shader-cache.pack.')
    ap.add_argument('cache', type=Path, help='the shader-cache folder next to nx2navi.exe')
    ap.add_argument('--keep', action='store_true', help='keep the loose files')
    a = ap.parse_args()
    if not a.cache.is_dir():
        sys.exit(f'{a.cache} is not a folder')
    n, new, skipped, size = pack(a.cache, a.keep)
    print(f'{a.cache / PACK}: {n} files, {size / 2**20:.0f} MiB ({new} loose files folded in'
          f'{", " + str(skipped) + " unreadable or empty skipped" if skipped else ""})', flush=True)


if __name__ == '__main__':
    main()
