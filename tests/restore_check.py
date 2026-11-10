"""The restorations (runtime/restore.c, restore_engine.c, byml.c, sarc.c) on files of its own: no game files.

Linux: python tests/restore_check.py (clang or cc, and zstd's development files)
Windows: python tests/restore_check.py --toolchain C:/msys64/ucrt64

Writes fixtures with an encoder of its own (BYML laid out parents first and nothing shared, unlike the engine's
writer): a placement file packed with a zstd dictionary (Pack/ZsDic.pack.zs's role is played by a SARC of two
trained dictionaries), a pack (SARC) of BYML members and raw bytes, and files that are not what they claim.
tests/restore_check.c runs the engine's edits on them, prints each note and checks which applied, feeds the parser,
the engine and the SARC reader and writer every cut length of them and damaged copies, checks that dicts out of order
are refused, and runs restore.c's switches, a start and the probe with the settings, the log and the RomFS stubbed
(every switch off: nothing read, served or logged); this script then decodes what it wrote with its own decoder and
compares the trees with the edits it expects, and checks the pack's other members kept their bytes and alignment.
"""
import argparse
import os
import random
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path

try:
    from compression import zstd as _zstd   # Python 3.14

    def compress(data, dic=None):
        return _zstd.compress(data, zstd_dict=dic) if dic else _zstd.compress(data)

    def decompress(data, dics):
        info = _zstd.get_frame_info(data)
        return _zstd.decompress(data, zstd_dict=dics[info.dictionary_id]) if info.dictionary_id else \
            _zstd.decompress(data)

    def train(samples, size):
        d = _zstd.train_dict(samples, size)
        return d, d.dict_id, d.dict_content
except ImportError:
    import zstandard

    def compress(data, dic=None):
        return zstandard.ZstdCompressor(dict_data=dic, write_content_size=True).compress(data)

    def decompress(data, dics):
        info = zstandard.get_frame_parameters(data)
        dctx = zstandard.ZstdDecompressor(dict_data=dics[info.dict_id]) if info.dict_id else zstandard.ZstdDecompressor()
        return dctx.decompress(data)

    def train(samples, size):
        d = zstandard.train_dictionary(size, samples)
        return d, d.dict_id(), d.as_bytes()

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'restore-check'


# ---- BYML: values are Python objects; F32/U32/I64/U64/F64/Bin/Hash32 tag the types Python has no own type for
class F32(float): pass
class U32(int): pass
class I64(int): pass
class U64(int): pass
class F64(float): pass
class Bin(bytes): pass
class Hash32(dict): pass   # {int hash: value}


def kind(v):
    if v is None: return 0xff
    if isinstance(v, bool): return 0xd0
    if isinstance(v, F32): return 0xd2
    if isinstance(v, F64): return 0xd6
    if isinstance(v, U32): return 0xd3
    if isinstance(v, I64): return 0xd4
    if isinstance(v, U64): return 0xd5
    if isinstance(v, int): return 0xd1
    if isinstance(v, str): return 0xa0
    if isinstance(v, Bin): return 0xa1
    if isinstance(v, Hash32): return 0x20
    if isinstance(v, dict): return 0xc1
    if isinstance(v, list): return 0xc0
    raise TypeError(type(v))


def byml_encode(root, version=7):
    """Parents before children, every container and 8-byte value written where it is met (nothing shared)."""
    e = '<'
    keys, strs = set(), set()

    def walk(v):
        if isinstance(v, str): strs.add(v)
        elif isinstance(v, Hash32): [walk(x) for x in v.values()]
        elif isinstance(v, dict): keys.update(v), [walk(x) for x in v.values()]
        elif isinstance(v, list): [walk(x) for x in v]
    walk(root)
    keys, strs = sorted(keys, key=lambda s: s.encode()), sorted(strs, key=lambda s: s.encode())
    out = bytearray(16)

    def head(t, n):
        return struct.pack(e + 'I', n << 8 | t)

    def table(items):
        at = len(out)
        blob = b''.join(s.encode() + b'\0' for s in items)
        offs, o = [], 4 + 4 * (len(items) + 1)
        for s in items:
            offs.append(o)
            o += len(s.encode()) + 1
        offs.append(o)
        out.extend(head(0xc2, len(items)) + struct.pack(e + '%dI' % len(offs), *offs) + blob)
        while len(out) % 4: out.append(0)
        return at

    kt = table(keys) if keys else 0
    st = table(strs) if strs else 0

    def align(n):
        while len(out) % n: out.append(0)

    def scalar(v):
        t = kind(v)
        if t == 0xa0: return strs.index(v)
        if t == 0xd0: return int(v)
        if t == 0xd1: return v & 0xffffffff
        if t == 0xd2: return struct.unpack(e + 'I', struct.pack(e + 'f', v))[0]
        if t == 0xd3: return int(v)
        if t == 0xff: return 0
        return None

    def container(v):
        """Writes the container at the end, its children after it; returns its offset."""
        align(4)
        at, t = len(out), kind(v)
        items = list(v.items()) if t == 0x20 else sorted(v.items(), key=lambda kv: kv[0].encode()) if t == 0xc1 \
            else list(enumerate(v))
        if t == 0x20: items.sort(key=lambda kv: kv[0])
        n = len(items)
        if t == 0xc0: size = 4 + (n + 3) // 4 * 4 + 4 * n
        elif t == 0xc1: size = 4 + 8 * n
        else: size = 4 + 8 * n + (n + 3) // 4 * 4
        out.extend(b'\0' * size)
        struct.pack_into(e + 'I', out, at, n << 8 | t)
        for i, (k, x) in enumerate(items):
            xt = kind(x)
            val = scalar(x)
            if val is None:
                if xt in (0xc0, 0xc1, 0x20): val = container(x)
                elif xt == 0xa1:
                    align(4)
                    val = len(out)
                    out.extend(struct.pack(e + 'I', len(x)) + x)
                else:
                    align(8)
                    val = len(out)
                    out.extend(struct.pack(e + {0xd4: 'q', 0xd5: 'Q', 0xd6: 'd'}[xt], x))
            if t == 0xc0:
                out[at + 4 + i] = xt
                struct.pack_into(e + 'I', out, at + 4 + (n + 3) // 4 * 4 + 4 * i, val)
            elif t == 0xc1:
                ki = keys.index(k)
                struct.pack_into(e + 'II', out, at + 4 + 8 * i, ki | xt << 24, val)
            else:
                struct.pack_into(e + 'II', out, at + 4 + 8 * i, k, val)
                out[at + 4 + 8 * n + i] = xt
        return at

    root_at = container(root)
    out[0:2] = b'YB'
    struct.pack_into(e + 'HIII', out, 2, version, kt, st, root_at)
    return bytes(out)


def byml_decode(d):
    assert d[:2] == b'YB', d[:4]
    e = '<'
    _, kt, st, root = struct.unpack_from(e + 'HIII', d, 2)

    def u32(o): return struct.unpack_from(e + 'I', d, o)[0]

    def head(o):
        w = u32(o)
        return w & 0xff, w >> 8

    def table(o):
        if not o: return []
        t, n = head(o)
        assert t == 0xc2
        return [d[o + u32(o + 4 + 4 * i):d.index(b'\0', o + u32(o + 4 + 4 * i))].decode() for i in range(n)]
    keys, strs = table(kt), table(st)
    if keys: assert keys == sorted(keys, key=lambda s: s.encode()), 'key table not sorted'
    if strs: assert strs == sorted(strs, key=lambda s: s.encode()), 'string table not sorted'

    def val(t, v):
        if t == 0xa0: return strs[v]
        if t == 0xa1: return Bin(d[v + 4:v + 4 + u32(v)])
        if t in (0xc0, 0xc1, 0x20):
            assert v % 4 == 0
            return node(v, t)
        if t == 0xd0: return bool(v)
        if t == 0xd1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xd2: return F32(struct.unpack('<f', struct.pack('<I', v))[0])
        if t == 0xd3: return U32(v)
        if t in (0xd4, 0xd5, 0xd6):
            assert v % 8 == 0, '64-bit value not 8-aligned'
            return {0xd4: I64, 0xd5: U64, 0xd6: F64}[t](struct.unpack_from(e + {0xd4: 'q', 0xd5: 'Q', 0xd6: 'd'}[t], d, v)[0])
        if t == 0xff: return None
        raise ValueError(hex(t))

    def node(o, want):
        t, n = head(o)
        assert t == want
        if t == 0xc0:
            vo = o + 4 + (n + 3) // 4 * 4
            return [val(d[o + 4 + i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xc1:
            res, last = {}, -1
            for i in range(n):
                w = u32(o + 4 + 8 * i)
                ki, ty = w & 0xffffff, w >> 24
                assert ki > last, 'dict not sorted by key'
                last = ki
                res[keys[ki]] = val(ty, u32(o + 8 + 8 * i))
            return res
        res = Hash32()
        for i in range(n):
            res[u32(o + 4 + 8 * i)] = val(d[o + 4 + 8 * n + i], u32(o + 8 + 8 * i))
        assert list(res) == sorted(res), 'hash dict not sorted'
        return res
    return node(root, head(root)[0]) if root else None


def same(a, b):
    """Tree equality with types: a float and an int of equal value differ."""
    if kind(a) != kind(b): return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float):
        return struct.pack('<f' if isinstance(a, F32) else '<d', a) == struct.pack('<f' if isinstance(b, F32) else '<d', b)
    return a == b


# ---- SARC
def sarc_encode(members, aligns):
    """members: [(name, bytes)] in node order (sorted by the name hash, as the game's are); aligns: data alignment of
    each member."""
    def h(name):
        x = 0
        for c in name.encode(): x = (x * 0x65 + (c if c < 0x80 else c - 0x100)) & 0xffffffff
        return x
    members = sorted(members, key=lambda m: h(m[0]))
    names = bytearray()
    offs = []
    for n, _ in members:
        offs.append(len(names) // 4)
        names += n.encode() + b'\0'
        while len(names) % 4: names.append(0)
    head = 0x14 + 12 + 16 * len(members) + 8 + len(names)
    data_at = (head + 0xff) & ~0xff
    data = bytearray()
    nodes = b''
    for (n, b), off in zip(members, offs):
        a = aligns[n]
        while (data_at + len(data)) % a: data.append(0)
        begin = len(data)
        data += b
        nodes += struct.pack('<IIII', h(n), 0x01000000 | off, begin, len(data))
    out = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, 0, data_at, 0x100, 0))
    out += b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + nodes
    out += b'SFNT' + struct.pack('<HH', 8, 0) + names
    out += b'\0' * (data_at - len(out)) + data
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def sarc_decode(d):
    assert d[:4] == b'SARC'
    head, bom, size, data_at = struct.unpack_from('<HHII', d, 4)
    assert size == len(d), 'SARC size field'
    count = struct.unpack_from('<H', d, head + 6)[0]
    names = head + 12 + 16 * count + 8
    res = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<IIII', d, head + 12 + 16 * i)
        o = names + (attr & 0xffffff) * 4
        res[d[o:d.index(b'\0', o)].decode()] = (data_at + lo, d[data_at + lo:data_at + hi])
    return res


# ---- fixtures and what the edits should make of them
def placement():
    return {
        'Actors': [
            {'Gyaml': 'Obj_A', 'Hash': U64(1001), 'Translate': [F32(1.0), F32(2.0), F32(3.0)],
             'Phive': {'Placement': {'ID': U64(1001)}}, 'SRTHash': U32(77)},
            {'Gyaml': 'Obj_B', 'Hash': U64(1002), 'Translate': [F32(4.0), F32(5.0), F32(6.0)],
             'Rotate': [F32(0.0), F32(1.5), F32(0.0)], 'Bakeable': True, 'SRTHash': U32(78),
             'Phive': {'Placement': {'ID': U64(1002)}},
             'Links': [{'Dst': U64(1001), 'Gyaml': 'Reference', 'Name': 'Reference', 'Src': U64(1002)}]},
            {'Gyaml': 'Obj_A', 'Hash': U64(1003), 'Translate': [F32(7.0), F32(8.0), F32(9.0)],
             'Scale': [F32(1.0), F32(2.0), F32(3.0)], 'Dynamic': {'Count': 5, 'Speed': F32(0.5)}},
            {'Gyaml': 'Obj_C', 'Hash': U64(18446744073709551557), 'Translate': [F32(-1.5), F32(0.25), F32(1e6)]},
        ],
        'FilePath': 'Work/Banc/Scene/Test.bcett.json',
        'Misc': {
            'Bool': False, 'Int': -7, 'UInt': U32(4000000000), 'Float': F32(-0.125), 'Int64': I64(-(1 << 40)),
            'UInt64': U64((1 << 64) - 2), 'Double': F64(3.141592653589793), 'Null': None, 'Bin': Bin(b'\x01\x02\x03'),
            'Empty': {}, 'EmptyList': [], 'Nested': [[U64(5), U64(5)], [U64(5), U64(5)], {'X': F32(1.0)}],
            'Hashes': Hash32({0x9000: 'b', 0x10: 'a', 0xffffffff: [1, 2]}), 'Unicode': 'Ж мод',
        },
    }


def placement_expected():
    t = placement()
    a = t['Actors']
    a[1]['Rotate'][1] = F32(3.0)
    a[1]['Bakeable'] = False
    a[2]['Scale'][1] = F32(5.0)
    a[2]['Dynamic']['Count'] = 9
    t['Misc']['Double'] = F64(2.5)
    return t


def pack_members():
    one = byml_encode({'MoveSpeed': F32(0.15), 'Other': 3})
    three = byml_encode({'Params': {'Speed': F32(1.5), 'Count': 4, 'Flag': True}})
    raw = bytes(random.Random(5).randrange(256) for _ in range(300))
    return [('A/one.bgyml', one), ('B/two.bin', raw), ('C/three.bgyml', three)], \
        {'A/one.bgyml': 8, 'B/two.bin': 0x100, 'C/three.bgyml': 0x80}


def build(cc, flags, env):
    exe = OUT / ('restore-check.exe' if os.name == 'nt' else 'restore-check')
    srcs = [ROOT / 'tests' / 'restore_check.c'] + [ROOT / 'runtime' / n
                                                   for n in ('restore.c', 'restore_engine.c', 'byml.c', 'sarc.c')]
    subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined' if os.name != 'nt' else '-O1',
                    '-I', str(ROOT / 'runtime'), *map(str, srcs), '-o', str(exe), *flags, '-lm'],
                   env=env, check=True)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--toolchain', type=Path, help='an MSYS2 environment (Windows)')
    a = ap.parse_args()
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    env = dict(os.environ)
    if a.toolchain:
        env['PATH'] = str(a.toolchain / 'bin') + os.pathsep + env.get('PATH', '')
        cc, flags = str(a.toolchain / 'bin' / 'clang.exe'), ['-L', str(a.toolchain / 'lib'), '-lzstd']
    else:
        cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
        try:
            flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libzstd'], text=True))
        except (OSError, subprocess.CalledProcessError):
            flags = ['-lzstd']
    exe = build(cc, flags, env)

    # Two trained dictionaries in a SARC, as Pack/ZsDic.pack.zs holds the game's; the placement is packed with the
    # second, so the engine has to pick it by its id.
    rnd = random.Random(1)
    samples = [byml_encode({'Actors': [{'Gyaml': 'Obj_%d' % rnd.randrange(50), 'Hash': U64(rnd.getrandbits(64)),
                                        'Translate': [F32(rnd.random()) for _ in range(3)]} for _ in range(8)]})
               for _ in range(300)]
    d1, id1, c1 = train(samples[:150], 4096)
    d2, id2, c2 = train(samples[150:], 4096)
    assert id1 != id2 and id1 and id2
    zsdic = sarc_encode([('zs.zsdic', c1), ('bcett.byml.zsdic', c2)], {'zs.zsdic': 4, 'bcett.byml.zsdic': 4})
    (OUT / 'zsdic.sarc').write_bytes(zsdic)
    place = byml_encode(placement())
    (OUT / 'place.byml.zs').write_bytes(compress(place, d2))
    members, aligns = pack_members()
    pack = sarc_encode(members, aligns)
    (OUT / 'pack.zs').write_bytes(compress(pack))
    (OUT / 'junk.bin').write_bytes(b'YB\x07\x00' + b'\xff' * 60)
    (OUT / 'otherdic.zs').write_bytes(compress(place, train(samples[:100], 2048)[0]))

    r = subprocess.run([str(exe), str(OUT)], env=env, capture_output=True, text=True, timeout=120)
    print(r.stdout, end='')
    print(r.stderr, end='', file=sys.stderr)
    fails = [r.returncode != 0 and 'restore_check.c failed (exit %d)' % r.returncode]

    def check(cond, what):
        if not cond: fails.append(what)

    dics = {id1: d1, id2: d2}
    out = (OUT / 'place.out.zs').read_bytes()
    check(out[:4] == b'\x28\xb5\x2f\xfd', 'placement: not a zstd frame')
    check(same(byml_decode(decompress(out, dics)), placement_expected()),
          'placement: the edited tree is not the expected one')

    p = decompress((OUT / 'pack.out.zs').read_bytes(), {})
    m = sarc_decode(p)
    check(set(m) == {n for n, _ in members}, 'pack: members changed')
    check(same(byml_decode(m['A/one.bgyml'][1]), {'MoveSpeed': F32(0.15 * 15.71 / 9.68), 'Other': 3}), 'pack: A')
    check(same(byml_decode(m['C/three.bgyml'][1]), {'Params': {'Speed': F32(1.5), 'Count': 7, 'Flag': True}}),
          'pack: C')
    check(m['B/two.bin'][1] == dict(members)['B/two.bin'], 'pack: B lost its bytes')
    for n, (off, _) in m.items():
        check(off % aligns[n] == 0, 'pack: %s at %#x lost its alignment %#x' % (n, off, aligns[n]))

    g = sarc_decode((OUT / 'grown.sarc').read_bytes())
    check(g['A/one.bgyml'][1] == bytes(range(256)) * 3, 'grown pack: A has not the new bytes')
    for n in ('B/two.bin', 'C/three.bgyml'):
        check(g[n][1] == dict(members)[n], 'grown pack: %s lost its bytes' % n)
        check(g[n][0] % aligns[n] == 0, 'grown pack: %s at %#x lost its alignment %#x' % (n, g[n][0], aligns[n]))

    fails = [f for f in fails if f]
    for f in fails:
        print('FAIL', f)
    print('restore-check: %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
