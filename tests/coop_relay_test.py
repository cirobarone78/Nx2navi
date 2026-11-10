"""tools/coop_relay.py and the co-op wire format: the room-code alphabet, HMAC tags, the byte vectors of
tests/coop_vectors.txt (which runtime/coop_probe.c checks the C codec against), the relay's rooms (create, join, forward,
roster, rejoin, expiry, rate limits, bad tags, a full room, a protocol mismatch) on a fake clock, the same over real UDP
sockets, and, when NX2NAVI_EXE names a built game, `--probe-coop` against a relay started here (with and without receive
loss, and through a relay restart). No game files; stdlib only.

Run: python tests/coop_relay_test.py            (NX2NAVI_EXE=<game>/ocarina also runs the probe)
     python tests/coop_relay_test.py --write-vectors   rewrites tests/coop_vectors.txt from the Python codec
"""
import hashlib
import hmac
import os
import random
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import coop_relay as cr  # noqa: E402

VECTORS = ROOT / 'tests' / 'coop_vectors.txt'
RELAY_PY = ROOT / 'tools' / 'coop_relay.py'


def hx(b):
    return b.hex() if b else '-'


def unhex(s):
    return b'' if s == '-' else bytes.fromhex(s)


# ---- the byte vectors

def build_vectors():
    """The lines of tests/coop_vectors.txt. HMAC values come from the standard library's hmac, everything else from the
    codec in tools/coop_relay.py; runtime/coop_proto.c is checked against the file, not against this function."""
    out = ['# Byte vectors of the co-op wire format. Written by `python tests/coop_relay_test.py --write-vectors`, read by',
           '# tests/coop_relay_test.py and by runtime/coop_probe.c (--probe-coop); each side checks its codec against them.',
           '# Hex is lower case, "-" is empty. Fields are tokens; a name is hex of its UTF-8 bytes.',
           '#',
           '# hmac <name> <key> <message> <HMAC-SHA256>                  a 16-byte key is the room key; the others cover the '
           'block-size cases',
           '# code <canonical|-> <text, the rest of the line>            normalisation: the code a text means, - for none',
           '# key <code> <room key>                                      SHA256("nx2navi-coop/1" + code)[:16]',
           '# dgram <name> <code|-> <ver> <type> <room> <src> <dst> <seq> <payload> <datagram>   parse, verify the tag '
           '(- : a zero tag), pack again',
           '# badtag <name> <code> <datagram>                          parses, and the tag does not verify',
           '# connect|roster|joined|error|peer ...                     one message payload: parse, check the fields, pack '
           'again',
           '# reject <kind> <name> <bytes>                             parse must fail']

    def line(*tokens):
        out.append(' '.join(str(t) for t in tokens))

    for name, key, msg in (
            ('rfc4231-1', bytes([0x0b] * 20), b'Hi There'),
            ('rfc4231-2', b'Jefe', b'what do ya want for nothing?'),
            ('rfc4231-6', bytes([0xaa] * 131), b'Test Using Larger Than Block-Size Key - Hash Key First'),
            ('room-key', cr.room_key('H7K3WM'), bytes(range(26))),
            ('empty', cr.room_key('23BCDF'), b'')):
        line('hmac', name, hx(key), hx(msg), hmac.new(key, msg, hashlib.sha256).hexdigest())
    for code, text in (('BCDFGH', 'bcdfgh'), ('BCDFGH', 'bcd-fg h'), ('BCDFGH', ' BCD-FGH '), ('234567', '234567'),
                       ('XYZ234', 'xyz-234'), ('-', 'BCDFG'), ('-', 'BCDFGHJ'), ('-', 'BCDFG0'), ('-', 'BCDFGE'),
                       ('-', 'BCDFGQ'), ('-', 'AEIOUX'), ('-', 'BCD.FGH'), ('-', '')):
        line('code', code, text)
    for code in ('BCDFGH', '23BCDF', 'H7K3WM', 'ZZZZZZ'):
        line('key', code, hx(cr.room_key(code)))

    key = cr.room_key('H7K3WM')
    cookie = bytes(range(0xA0, 0xB0))
    cid = 0x0123456789ABCDEF
    roster = cr.pack_roster([(1, 0, b'Alice'), (2, cr.FLAG_AWAY, 'Zoë'.encode())])
    dgrams = [
        ('connect-create', None, cr.T_CONNECT, 0, 0, 0, 7,
         cr.pack_connect(cr.INTENT_CREATE, 0, '', cid, b'Alice', bytes(16))),
        ('connect-join', 'H7K3WM', cr.T_CONNECT, 0, 0, 0, 7,
         cr.pack_connect(cr.INTENT_JOIN, 0, 'H7K3WM', cid, b'Bob', cookie)),
        ('challenge', 'H7K3WM', cr.T_CHALLENGE, 0, 0, 0, 1, cookie),
        ('joined', 'H7K3WM', cr.T_JOINED, 0xCAFE0001, 0, 2, 2, bytes([2]) + b'H7K3WM' + roster),
        ('error', 'H7K3WM', cr.T_ERROR, 0, 0, 0, 3, cr.pack_error(cr.E_NO_ROOM, 'no such room')),
        ('roster', 'H7K3WM', cr.T_ROSTER, 0xCAFE0001, 0, 1, 4, roster),
        ('ping', 'H7K3WM', cr.T_PING, 0xCAFE0001, 3, 0, 99, struct.pack('<I', 0xDEADBEEF)),
        ('pong', 'H7K3WM', cr.T_PONG, 0xCAFE0001, 0, 3, 100, struct.pack('<I', 0xDEADBEEF)),
        ('leave', 'H7K3WM', cr.T_LEAVE, 0xCAFE0001, 3, 0, 101, b''),
        ('data-all', 'H7K3WM', cr.T_DATA, 0xCAFE0001, 3, cr.DST_ALL, 102, bytes([0xF0]) + b'hello'),
        ('data-one', 'H7K3WM', cr.T_DATA, 0xCAFE0001, 3, 5, 0xFFFFFFFF, bytes([0x02]) + bytes(range(40))),
        ('data-max', 'H7K3WM', cr.T_DATA, 1, 8, cr.DST_ALL, 0, bytes(i * 7 & 0xFF for i in range(cr.MAX_DGRAM - cr.HDR_LEN))),
    ]
    for name, code, type_, room, src, dst, seq, payload in dgrams:
        d = cr.pack(type_, room, src, dst, seq, payload, cr.room_key(code) if code else None)
        line('dgram', name, code or '-', cr.PROTO, type_, room, src, dst, seq, hx(payload), d.hex())
    d = cr.pack(cr.T_DATA, 0xCAFE0001, 3, cr.DST_ALL, 102, bytes([0xF0]) + b'hello', cr.room_key('H7K3WM'))
    line('badtag', 'flipped-tag', 'H7K3WM', (d[:16] + bytes([d[16] ^ 1]) + d[17:]).hex())
    line('badtag', 'wrong-key', 'BCDFGH', d.hex())
    line('dgram', 'other-version', '-', 2, cr.T_PING, 0, 0, 0, 5, hx(b'\0\0\0\0'),
         cr.pack(cr.T_PING, 0, 0, 0, 5, b'\0\0\0\0', None, ver=2).hex())

    line('connect', 'create', cr.INTENT_CREATE, 0, '-', f'{cid:016x}', hx(b'Alice'), hx(bytes(16)),
         hx(cr.pack_connect(cr.INTENT_CREATE, 0, '', cid, b'Alice', bytes(16))))
    line('connect', 'join', cr.INTENT_JOIN, 0, 'H7K3WM', f'{cid:016x}', hx('Zoë'.encode()), hx(cookie),
         hx(cr.pack_connect(cr.INTENT_JOIN, 0, 'H7K3WM', cid, 'Zoë'.encode(), cookie)))
    long_name = b'x' * cr.NAME_MAX
    line('connect', 'long-name', cr.INTENT_JOIN, 1, 'BCDFGH', f'{1:016x}', hx(long_name), hx(cookie),
         hx(cr.pack_connect(cr.INTENT_JOIN, 1, 'BCDFGH', 1, long_name, cookie)))
    line('roster', 'two', hx(roster), f'1:0:{b"Alice".hex()}', f'2:1:{"Zoë".encode().hex()}')
    line('roster', 'empty', hx(cr.pack_roster([])))
    full = cr.pack_roster([(i, 0, f'P{i}'.encode()) for i in range(1, 9)])
    line('roster', 'full', hx(full), *[f'{i}:0:{f"P{i}".encode().hex()}' for i in range(1, 9)])
    line('joined', 'two', hx(bytes([2]) + b'H7K3WM' + roster), 2, 'H7K3WM', f'1:0:{b"Alice".hex()}', f'2:1:{"Zoë".encode().hex()}')
    line('error', 'no-room', hx(cr.pack_error(cr.E_NO_ROOM, 'no such room')), cr.E_NO_ROOM, hx(b'no such room'))
    line('error', 'bare', hx(cr.pack_error(cr.E_SERVER_FULL, '')), cr.E_SERVER_FULL, '-')
    line('peer', 'hello', hx(bytes([0xF0]) + b'hello'), 0xF0, hx(b'hello'))
    line('peer', 'kind-only', hx(bytes([7])), 7, '-')

    zeros = bytes(16)
    ping = cr.pack(cr.T_PING, 0, 0, 0, 1, b'abcd')
    bad = [
        ('dgram', 'short', bytes(23)),
        ('dgram', 'magic', b'\x00\x00' + ping[2:]),
        ('dgram', 'length-long', ping[:10] + struct.pack('<H', 5) + ping[12:]),
        ('dgram', 'trailing', ping + b'x'),
        ('connect', 'name-too-long', struct.pack('<BB6sQB', 2, 0, bytes(6), 1, 25) + bytes(25) + zeros),
        ('connect', 'no-cookie', struct.pack('<BB6sQB', 2, 0, bytes(6), 1, 3) + b'Bob' + bytes(15)),
        ('connect', 'code-lower', struct.pack('<BB6sQB', 2, 0, b'bcdfgh', 1, 0) + zeros),
        ('roster', 'count-9', bytes([9]) + bytes(40)),
        ('roster', 'cut-name', bytes([1, 1, 0, 9]) + b'abc'),
        ('roster', 'cut-entry', bytes([2, 1, 0, 1, b'A'[0], 2, 0])),
        ('joined', 'code-lower', bytes([1]) + b'h7k3wm' + bytes([0])),
        ('joined', 'code-vowel', bytes([1]) + b'H7K3EM' + bytes([0])),
        ('joined', 'cut', bytes([1]) + b'H7K'),
        ('error', 'cut-text', bytes([1, 9]) + b'abc'),
        ('error', 'cut', bytes([1])),
        ('peer', 'empty', b''),
    ]
    for kind, name, data in bad:
        line('reject', kind, name, hx(data))
    return out


def write_vectors():
    VECTORS.write_text('\n'.join(build_vectors()) + '\n', encoding='utf-8')


def read_vectors():
    """[(kind, tokens)] of the data lines; a code line keeps its text as one token after the canonical code."""
    rows = []
    for raw in VECTORS.read_text(encoding='utf-8').splitlines():
        if not raw.strip() or raw.startswith('#'):
            continue
        if raw.startswith('code '):
            parts = raw.split(' ', 2)
            rows.append(('code', [parts[1], parts[2] if len(parts) > 2 else '']))
        else:
            parts = raw.split()
            rows.append((parts[0], parts[1:]))
    return rows


class CodecTest(unittest.TestCase):
    def test_the_alphabet_has_27_symbols_and_no_lookalikes(self):
        self.assertEqual(len(cr.ALPHABET), 27)
        self.assertEqual(len(set(cr.ALPHABET)), 27)
        for c in '01OILEQAU':
            self.assertNotIn(c, cr.ALPHABET)

    def test_normalize(self):
        self.assertEqual(cr.normalize_code('h7k-3wm'), 'H7K3WM')
        self.assertEqual(cr.normalize_code(' h7k 3wm '), 'H7K3WM')
        for bad in ('', 'H7K3W', 'H7K3WMX', 'H7K3W0', 'H7K3WE', 'H7K3W\u017f', 'H7K3W\u00e9', 'H7K3W.'):
            self.assertIsNone(cr.normalize_code(bad), bad)
        self.assertEqual(cr.format_code('H7K3WM'), 'H7K-3WM')

    def test_random_codes_are_valid(self):
        for _ in range(200):
            self.assertEqual(cr.normalize_code(cr.random_code()) is not None, True)

    def test_room_key_is_the_documented_hash(self):
        self.assertEqual(cr.room_key('H7K3WM'), hashlib.sha256(b'nx2navi-coop/1H7K3WM').digest()[:16])

    def test_tag_covers_header_and_payload(self):
        key = cr.room_key('H7K3WM')
        d = cr.pack(cr.T_DATA, 5, 1, 2, 3, b'payload', key)
        dg = cr.parse(d)
        self.assertTrue(dg.verify(key))
        for i in range(len(d)):
            if 16 <= i < 24 or i in (10, 11):
                continue   # the tag itself; the length (covered by parse)
            flipped = d[:i] + bytes([d[i] ^ 0x40]) + d[i + 1:]
            p = cr.parse(flipped)
            if p is not None:
                self.assertFalse(p.verify(key), f'byte {i}')
        self.assertFalse(dg.verify(cr.room_key('BCDFGH')))

    def test_parse_rejects_what_is_not_one_datagram(self):
        d = cr.pack(cr.T_PING, 0, 0, 0, 1, b'abcd')
        self.assertIsNotNone(cr.parse(d))
        self.assertIsNone(cr.parse(d[:-1]))
        self.assertIsNone(cr.parse(d + b'x'))
        self.assertIsNone(cr.parse(b'\x00\x00' + d[2:]))
        self.assertIsNone(cr.parse(bytes(cr.MAX_DGRAM + 1)))

    def test_clean_name(self):
        self.assertEqual(cr.clean_name(b'Alice'), b'Alice')
        self.assertEqual(cr.clean_name(b'A\x01B\x7fC'), b'A?B?C')
        self.assertEqual(cr.clean_name(b'ab\xffcd'), b'ab?cd')
        self.assertEqual(len(cr.clean_name(('\u00e9' * 30).encode())), 24)   # whole characters only
        self.assertEqual(cr.clean_name(b'x' * 30), b'x' * 24)

    def test_vectors(self):
        """The file matches what this codec writes now, and every line checks out."""
        self.assertEqual(VECTORS.read_text(encoding='utf-8'), '\n'.join(build_vectors()) + '\n',
                         'tests/coop_vectors.txt is stale: python tests/coop_relay_test.py --write-vectors')
        count = 0
        for kind, t in read_vectors():
            count += 1
            if kind == 'hmac':
                self.assertEqual(hmac.new(unhex(t[1]), unhex(t[2]), hashlib.sha256).hexdigest(), t[3], t[0])
            elif kind == 'code':
                want = None if t[0] == '-' else t[0]
                self.assertEqual(cr.normalize_code(t[1]), want, t[1])
            elif kind == 'key':
                self.assertEqual(cr.room_key(t[0]).hex(), t[1])
            elif kind == 'dgram':
                name, code, ver, type_, room, src, dst, seq, payload, data = t
                d = unhex(data)
                p = cr.parse(d)
                self.assertEqual((p.ver, p.type, p.room, p.src, p.dst, p.seq, p.payload),
                                 (int(ver), int(type_), int(room), int(src), int(dst), int(seq), unhex(payload)), name)
                if code == '-':
                    self.assertEqual(p.tag, bytes(8), name)
                else:
                    self.assertTrue(p.verify(cr.room_key(code)), name)
                self.assertEqual(cr.pack(p.type, p.room, p.src, p.dst, p.seq, p.payload,
                                         cr.room_key(code) if code != '-' else None, ver=p.ver), d, name)
            elif kind == 'badtag':
                p = cr.parse(unhex(t[2]))
                self.assertIsNotNone(p, t[0])
                self.assertFalse(p.verify(cr.room_key(t[1])), t[0])
            elif kind == 'connect':
                name, intent, flags, code, cid, nm, cookie, payload = t
                c = cr.parse_connect(unhex(payload))
                self.assertEqual(c, (int(intent), int(flags), '' if code == '-' else code, int(cid, 16), unhex(nm),
                                     unhex(cookie)), name)
                self.assertEqual(cr.pack_connect(c[0], c[1], c[2], c[3], c[4], c[5]), unhex(payload), name)
            elif kind in ('roster', 'joined'):
                name, payload, rest = t[0], unhex(t[1]), t[2:]
                if kind == 'joined':
                    self.assertEqual((payload[0], payload[1:7].decode()), (int(rest[0]), rest[1]), name)
                    rest, offset = rest[2:], 7
                else:
                    offset = 0
                want = tuple((int(e.split(':')[0]), int(e.split(':')[1]), bytes.fromhex(e.split(':')[2])) for e in rest)
                self.assertEqual(cr.parse_roster(payload, offset), want, name)
                self.assertEqual(payload[offset:], cr.pack_roster(want), name)
            elif kind == 'error':
                name, payload, reason, text = t
                self.assertEqual(cr.parse_error(unhex(payload)), (int(reason), unhex(text).decode()), name)
            elif kind == 'peer':
                payload = unhex(t[1])
                self.assertEqual((payload[0], payload[1:]), (int(t[2]), unhex(t[3])), t[0])
            elif kind == 'reject':
                what, name, data = t[0], t[1], unhex(t[2])
                parsers = {'dgram': cr.parse, 'connect': cr.parse_connect, 'roster': cr.parse_roster,
                           'joined': lambda b: cr.parse_roster(b, 7) if len(b) > 6 and cr.normalize_code(
                               b[1:7].decode('latin1')) == b[1:7].decode('latin1') else None,
                           'error': cr.parse_error, 'peer': lambda b: b or None}
                self.assertIsNone(parsers[what](data), f'{what} {name}')
            else:
                self.fail(f'unknown vector line {kind}')
        self.assertGreater(count, 60)


# ---- the relay on a fake clock

class Client:
    """A fake client: what coop.c does, one datagram at a time, against Relay.handle."""
    _next_port = 40000

    def __init__(self, name='Alice', cid=None, ip='10.0.0.1'):
        Client._next_port += 1
        self.addr = (ip, Client._next_port)
        self.name = name.encode()
        self.cid = cid if cid is not None else random.getrandbits(64)
        self.seq = 0
        self.room = 0
        self.id = 0
        self.code = ''
        self.key = None

    def connect(self, intent, code='', cookie=bytes(16), seq=None):
        payload = cr.pack_connect(intent, 0, code, self.cid, self.name, cookie)
        key = cr.room_key(cr.normalize_code(code) or code) if code and any(cookie) else None
        return cr.pack(cr.T_CONNECT, 0, 0, 0, self.seq if seq is None else seq, payload, key)

    def join(self, relay, now, intent=cr.INTENT_JOIN, code='', seq=None):
        """The whole handshake; (JOINED fields or ERROR (reason, text), the other datagrams the relay sent)."""
        self.seq += 1
        out = relay.handle(self.connect(intent, code, seq=seq), self.addr, now)
        assert len(out) == 1 and out[0][0] == self.addr, out
        ch = cr.parse(out[0][1])
        assert ch.type == cr.T_CHALLENGE and len(out[0][1]) < 64
        out = relay.handle(self.connect(intent, code, ch.payload, seq=seq), self.addr, now)
        mine = [cr.parse(d) for to, d in out if to == self.addr]
        rest = [(to, cr.parse(d)) for to, d in out if to != self.addr]
        first = mine[0]
        if first.type == cr.T_ERROR:
            return cr.parse_error(first.payload), rest
        assert first.type == cr.T_JOINED, first.type
        self.room, self.id = first.room, first.payload[0]
        self.code = first.payload[1:7].decode()
        self.key = cr.room_key(self.code)
        assert first.verify(self.key)
        return (self.id, self.code, cr.parse_roster(first.payload, 7)), rest

    def send(self, relay, type_, payload=b'', dst=0, now=0, key='own', room=None, src=None):
        self.seq += 1
        key = self.key if key == 'own' else key
        d = cr.pack(type_, self.room if room is None else room, self.id if src is None else src, dst, self.seq, payload, key)
        return relay.handle(d, self.addr, now), d


def parsed(out):
    return [(to, cr.parse(d)) for to, d in out]


class RelayTest(unittest.TestCase):
    def setUp(self):
        self.relay = cr.Relay()
        self.log = []
        self.relay.log_fn = self.log.append

    def room_of(self, n, now=0):
        """n clients in one room: (clients, code)."""
        a = Client('A')
        info, _ = a.join(self.relay, now, cr.INTENT_CREATE)
        clients = [a]
        for i in range(1, n):
            c = Client(f'P{i}')
            info, _ = c.join(self.relay, now, cr.INTENT_JOIN, a.code)
            clients.append(c)
        return clients, a.code

    # -- joining

    def test_create_answers_a_challenge_then_a_room(self):
        a = Client('Alice')
        a.seq = 1
        first = a.connect(cr.INTENT_CREATE)
        self.assertGreaterEqual(len(first), cr.CONNECT_MIN)
        out = self.relay.handle(first, a.addr, 0)
        self.assertEqual(len(out), 1)
        ch = cr.parse(out[0][1])
        self.assertEqual((ch.type, ch.room, ch.src), (cr.T_CHALLENGE, 0, 0))
        self.assertLess(len(out[0][1]), len(first))
        self.assertEqual(len(ch.payload), cr.COOKIE_LEN)
        self.assertEqual(self.relay.rooms, {})   # nothing exists before the cookie came back
        info, rest = Client.join(a, self.relay, 0, cr.INTENT_CREATE)
        self.assertEqual(info[0], 1)
        self.assertIsNotNone(cr.normalize_code(info[1]))
        self.assertEqual(info[2], ((1, 0, b'Alice'),))
        self.assertEqual(rest, [])
        self.assertEqual(len(self.relay.rooms), 1)

    def test_a_short_connect_gets_no_answer(self):
        a = Client()
        d = a.connect(cr.INTENT_CREATE)
        self.assertEqual(self.relay.handle(d[:cr.CONNECT_MIN - 1], a.addr, 0), [])   # not a valid datagram either
        short = cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(cr.INTENT_CREATE, 0, '', 1, b'', bytes(16))[:33])
        self.assertLess(len(short), cr.CONNECT_MIN)
        self.assertEqual(self.relay.handle(short, a.addr, 0), [])

    def test_a_cookie_belongs_to_its_address_and_expires(self):
        a, b = Client('A'), Client('B')
        a.seq = b.seq = 1
        ch = cr.parse(self.relay.handle(a.connect(cr.INTENT_CREATE), a.addr, 0)[0][1])
        # from another address the same cookie is not valid: another challenge
        out = self.relay.handle(b.connect(cr.INTENT_CREATE, cookie=ch.payload), b.addr, 1)
        self.assertEqual(cr.parse(out[0][1]).type, cr.T_CHALLENGE)
        # after two minutes it has expired
        out = self.relay.handle(a.connect(cr.INTENT_CREATE, cookie=ch.payload), a.addr, 125)
        self.assertEqual(cr.parse(out[0][1]).type, cr.T_CHALLENGE)
        out = self.relay.handle(a.connect(cr.INTENT_CREATE, cookie=ch.payload), a.addr, 59)
        self.assertEqual(cr.parse(out[0][1]).type, cr.T_JOINED)

    def test_join_and_the_roster_of_the_others(self):
        a = Client('Alice')
        a.join(self.relay, 0, cr.INTENT_CREATE)
        b = Client('Bob')
        info, rest = b.join(self.relay, 1, cr.INTENT_JOIN, a.code)
        self.assertEqual(info[0], 2)
        self.assertEqual(info[2], ((1, 0, b'Alice'), (2, 0, b'Bob')))
        self.assertEqual(len(rest), 1)
        to, dg = rest[0]
        self.assertEqual((to, dg.type, dg.dst), (a.addr, cr.T_ROSTER, 1))
        self.assertTrue(dg.verify(a.key))
        self.assertEqual(cr.parse_roster(dg.payload), info[2])

    def test_wrong_code_no_such_room_and_a_bad_tag(self):
        a = Client()
        info, _ = a.join(self.relay, 0, cr.INTENT_JOIN, 'BCDFGH')
        self.assertEqual(info[0], cr.E_NO_ROOM)
        # a CONNECT whose tag is wrong
        b = Client()
        b.seq = 1
        ch = cr.parse(self.relay.handle(b.connect(cr.INTENT_JOIN, 'BCDFGH'), b.addr, 0)[0][1])
        d = bytearray(b.connect(cr.INTENT_JOIN, 'BCDFGH', ch.payload))
        d[17] ^= 1
        out = parsed(self.relay.handle(bytes(d), b.addr, 0))
        self.assertEqual(cr.parse_error(out[0][1].payload)[0], cr.E_BAD_TAG)
        self.assertTrue(out[0][1].verify(cr.room_key('BCDFGH')))

    def test_a_connect_sent_again_is_answered_again(self):
        a = Client('A')
        a.seq = 1
        ch = cr.parse(self.relay.handle(a.connect(cr.INTENT_CREATE), a.addr, 0)[0][1])
        d = a.connect(cr.INTENT_CREATE, cookie=ch.payload)
        first = cr.parse(self.relay.handle(d, a.addr, 0)[0][1])
        second = cr.parse(self.relay.handle(d, a.addr, 0.1)[0][1])
        self.assertEqual((second.type, second.room, second.payload[:7]), (cr.T_JOINED, first.room, first.payload[:7]))
        self.assertEqual(len(self.relay.rooms), 1)
        self.assertEqual(len(self.relay.rooms[first.room].live()), 1)
        # a create with a new sequence number after that is a new room
        a.seq = 2
        out = self.relay.handle(a.connect(cr.INTENT_CREATE, cookie=ch.payload), a.addr, 1)
        self.assertEqual(cr.parse(out[0][1]).type, cr.T_JOINED)
        self.assertEqual(len(self.relay.rooms), 2)
        self.assertEqual(sum(len(r.live()) for r in self.relay.rooms.values()), 1)   # it left the first

    def test_room_full_at_eight(self):
        clients, code = self.room_of(8)
        self.assertEqual([c.id for c in clients], list(range(1, 9)))
        x = Client('X')
        info, _ = x.join(self.relay, 0, cr.INTENT_JOIN, code)
        self.assertEqual(info[0], cr.E_ROOM_FULL)

    def test_server_full(self):
        relay = cr.Relay(max_rooms=2)
        for i in range(2):
            Client().join(relay, 0, cr.INTENT_CREATE)
        info, _ = Client().join(relay, 0, cr.INTENT_CREATE)
        self.assertEqual(info[0], cr.E_SERVER_FULL)

    def test_protocol_mismatch(self):
        a = Client()
        d = cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(cr.INTENT_CREATE, 0, '', 1, b'A', bytes(16)), ver=2)
        out = parsed(self.relay.handle(d, a.addr, 0))
        self.assertEqual(cr.parse_error(out[0][1].payload)[0], cr.E_PROTO)
        self.assertEqual(out[0][1].ver, cr.PROTO)
        self.assertLessEqual(len(self.relay.handle(d, a.addr, 0)[0][1]), len(d))   # an answer is never larger than the request
        # too small a request for the answer: nothing
        tiny = cr.pack(cr.T_PING, 0, 0, 0, 1, b'', ver=2)
        self.assertEqual(self.relay.handle(tiny, a.addr, 0), [])

    def test_chosen_codes_only_when_allowed(self):
        a = Client()
        info, _ = a.join(self.relay, 0, cr.INTENT_CREATE, 'TSTW22')
        self.assertNotEqual(info[1], 'TSTW22')
        relay = cr.Relay(allow_chosen_codes=True)
        a, b = Client('A'), Client('B')
        info, _ = a.join(relay, 0, cr.INTENT_CREATE, 'H7K3WM')
        self.assertEqual(info[1], 'H7K3WM')
        info, _ = b.join(relay, 0, cr.INTENT_CREATE, 'H7K3WM')   # a create of an existing code joins it
        self.assertEqual((info[0], info[1], len(info[2])), (2, 'H7K3WM', 2))
        self.assertEqual(len(relay.rooms), 1)

    def test_names_are_cleaned(self):
        a = Client()
        a.name = b'\x01evil\xff' + '\u00e9'.encode() * 8
        info, _ = a.join(self.relay, 0, cr.INTENT_CREATE)
        name = info[2][0][2]
        self.assertLessEqual(len(name), cr.NAME_MAX)
        self.assertTrue(name.startswith(b'?evil?'))
        name.decode('utf-8')

    # -- forwarding

    def test_data_goes_to_the_others_unchanged(self):
        (a, b, c), _ = self.room_of(3)
        out, d = a.send(self.relay, cr.T_DATA, b'\xf0hello', dst=cr.DST_ALL, now=1)
        self.assertEqual(sorted(to for to, _ in out), sorted([b.addr, c.addr]))
        for _, data in out:
            self.assertEqual(data, d)
            self.assertTrue(cr.parse(data).verify(a.key))
        out, d = a.send(self.relay, cr.T_DATA, b'\xf0to b', dst=b.id, now=1)
        self.assertEqual([(to, data) for to, data in out], [(b.addr, d)])
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'x', dst=a.id, now=1)[0], [])   # not to itself
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'x', dst=7, now=1)[0], [])      # nobody there

    def test_data_from_the_wrong_place_is_dropped(self):
        (a, b), _ = self.room_of(2)
        d = cr.pack(cr.T_DATA, a.room, a.id, cr.DST_ALL, 5, b'\xf0x', a.key)
        self.assertEqual(self.relay.handle(d, b.addr, 1), [])                         # a's datagram from b's address
        self.assertEqual(self.relay.handle(d, ('10.9.9.9', 5), 1), [])               # from nowhere
        bad = cr.pack(cr.T_DATA, a.room, a.id, cr.DST_ALL, 5, b'\xf0x', cr.room_key('BCDFGH'))
        self.assertEqual(self.relay.handle(bad, a.addr, 1), [])                       # wrong key
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'\xf0x', dst=cr.DST_ALL, now=1, room=a.room + 1)[0], [])
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'\xf0x', dst=cr.DST_ALL, now=1, src=b.id)[0], [])   # as b
        self.assertEqual(a.send(self.relay, cr.T_DATA, b'\xf0x', dst=cr.DST_ALL, now=1, key=None)[0], [])    # zero tag
        self.assertGreaterEqual(self.relay.stats['bad_tag'], 2)
        self.assertEqual(len(a.send(self.relay, cr.T_DATA, b'\xf0x', dst=cr.DST_ALL, now=1)[0]), 1)   # and the real one

    def test_ping_pong(self):
        (a,), _ = self.room_of(1)
        out, _ = a.send(self.relay, cr.T_PING, struct.pack('<I', 0x01020304), now=1)
        (to, pong), = parsed(out)
        self.assertEqual((to, pong.type, pong.src, pong.dst, pong.payload), (a.addr, cr.T_PONG, 0, a.id, struct.pack('<I', 0x01020304)))
        self.assertTrue(pong.verify(a.key))

    def test_leave_frees_the_id_and_updates_the_others(self):
        (a, b, c), code = self.room_of(3)
        out, _ = b.send(self.relay, cr.T_LEAVE, now=1)
        got = parsed(out)
        self.assertEqual(sorted(to for to, _ in got), sorted([a.addr, c.addr]))
        self.assertEqual([m[0] for m in cr.parse_roster(got[0][1].payload)], [1, 3])
        d = Client('D')
        info, _ = d.join(self.relay, 2, cr.INTENT_JOIN, code)
        self.assertEqual(info[0], 2)   # the lowest free id

    # -- time

    def test_silence_marks_away_then_drops_and_the_id_is_kept_for_a_rejoin(self):
        (a, b), code = self.room_of(2)
        self.relay.tick(1)   # the join's repeats of the roster
        self.assertEqual(self.relay.tick(4), [])
        out = parsed(self.relay.tick(cr.SILENT_AWAY + 1))   # both silent: both away
        flags = {m[0]: m[1] for m in cr.parse_roster(out[0][1].payload)}
        self.assertEqual(flags, {1: cr.FLAG_AWAY, 2: cr.FLAG_AWAY})
        # a keeps pinging, b does not
        a.send(self.relay, cr.T_PING, b'\0\0\0\0', now=cr.SILENT_AWAY + 2)
        out = parsed(self.relay.tick(cr.SILENT_DROP + 1))
        roster = cr.parse_roster(out[0][1].payload)
        self.assertEqual([(m[0], m[1]) for m in roster], [(1, 0)])
        self.assertEqual(sorted(to for to, _ in out), [a.addr])
        # b is back with a new address, within the minute: its id again
        b2 = Client('B', cid=b.cid)
        info, rest = b2.join(self.relay, 30, cr.INTENT_JOIN, code)
        self.assertEqual(info[0], 2)
        self.assertEqual([to for to, _ in rest], [a.addr])
        # a third arrives while b2 was gone: not the id b2 keeps
        c = Client('C')
        self.assertEqual(c.join(self.relay, 31, cr.INTENT_JOIN, code)[0][0], 3)

    def test_a_dropped_id_is_free_again_after_a_minute(self):
        (a, b), code = self.room_of(2)
        self.relay.tick(cr.SILENT_DROP + 1)   # both dropped
        self.relay.tick(cr.REJOIN_KEEP + 5)
        c = Client('C')
        self.assertEqual(c.join(self.relay, cr.REJOIN_KEEP + 6, cr.INTENT_JOIN, code)[0][0], 1)

    def test_a_full_room_gives_a_dropped_id_to_a_newcomer(self):
        clients, code = self.room_of(8)
        keep = clients[1:]
        self.relay.tick(cr.SILENT_AWAY + 1)
        for c in keep:
            c.send(self.relay, cr.T_PING, b'\0\0\0\0', now=cr.SILENT_AWAY + 2)
        self.relay.tick(cr.SILENT_DROP + 1)   # only clients[0] is silent and dropped
        x = Client('X')
        info, _ = x.join(self.relay, cr.SILENT_DROP + 2, cr.INTENT_JOIN, code)
        self.assertEqual(info[0], 1)

    def test_an_empty_room_closes_after_ten_minutes(self):
        (a,), code = self.room_of(1)
        a.send(self.relay, cr.T_LEAVE, now=1)
        self.relay.tick(cr.ROOM_EMPTY_TTL - 1)
        self.assertEqual(len(self.relay.rooms), 1)
        self.relay.tick(cr.ROOM_EMPTY_TTL + 2)
        self.assertEqual(self.relay.rooms, {})
        info, _ = Client().join(self.relay, cr.ROOM_EMPTY_TTL + 3, cr.INTENT_JOIN, code)
        self.assertEqual(info[0], cr.E_NO_ROOM)

    def test_the_roster_is_repeated_so_a_lost_update_repairs(self):
        (a, b), _ = self.room_of(2)
        out = self.relay.tick(cr.ROSTER_EVERY + 1)
        self.assertEqual(sorted(to for to, _ in out), sorted([a.addr, b.addr]))

    def test_a_change_of_the_roster_goes_out_three_times(self):
        (a, b), code = self.room_of(2)
        self.relay.tick(1)                                           # the room's own repeats are not what is measured
        c = Client('C')
        c.join(self.relay, 3, cr.INTENT_JOIN, code)                  # the first time: to a and b, with the JOINED to c
        self.assertEqual(self.relay.tick(3.1), [])
        for delay in cr.ROSTER_AGAIN:                                # then to everyone, c included
            out = parsed(self.relay.tick(3 + delay + 0.01))
            self.assertEqual(sorted(to for to, _ in out), sorted([a.addr, b.addr, c.addr]), delay)
            self.assertEqual([m[0] for m in cr.parse_roster(out[0][1].payload)], [1, 2, 3])
        self.assertEqual(self.relay.tick(4.5), [])                   # and then only the periodic one

    def test_a_leave_goes_out_three_times_too(self):
        (a, b), _ = self.room_of(2)
        self.relay.tick(1)
        b.send(self.relay, cr.T_LEAVE, now=2)
        for when in (2 + cr.ROSTER_AGAIN[0], 2 + cr.ROSTER_AGAIN[1]):
            out = parsed(self.relay.tick(when + 0.01))
            self.assertEqual([to for to, _ in out], [a.addr], when)
            self.assertEqual([m[0] for m in cr.parse_roster(out[0][1].payload)], [1])

    # -- limits

    def test_a_member_over_its_packet_rate_is_dropped(self):
        relay = cr.Relay(pkt_rate=200, pkt_burst=20)
        a = Client()
        a.join(relay, 0, cr.INTENT_CREATE)
        answered = sum(1 for _ in range(100) if a.send(relay, cr.T_PING, b'\0\0\0\0', now=1)[0])
        self.assertEqual(answered, 20)
        self.assertEqual(relay.stats['rate_limited'], 80)
        self.assertEqual(len(a.send(relay, cr.T_PING, b'\0\0\0\0', now=1.05)[0]), 1)   # 200/s refills 10 in 50 ms

    def test_a_member_over_its_byte_rate_is_dropped(self):
        relay = cr.Relay(byte_rate=10000, byte_burst=5000)
        a, b = Client(), Client()
        a.join(relay, 0, cr.INTENT_CREATE)
        b.join(relay, 0, cr.INTENT_JOIN, a.code)
        big = bytes(1000)
        sent = sum(1 for _ in range(20) if a.send(relay, cr.T_DATA, big, dst=cr.DST_ALL, now=2)[0])
        self.assertLess(sent, 6)
        self.assertGreaterEqual(sent, 3)

    def test_joins_per_address_are_limited(self):
        relay = cr.Relay(join_burst=3, join_rate=1 / 60)
        results = []
        for i in range(5):
            info, _ = Client(f'P{i}').join(relay, 0, cr.INTENT_CREATE)
            results.append(info[0] if isinstance(info[0], int) and info[0] == cr.E_RATE else 'ok')
        self.assertEqual(results, ['ok', 'ok', 'ok', cr.E_RATE, cr.E_RATE])
        info, _ = Client('other', ip='10.0.0.2').join(relay, 0, cr.INTENT_CREATE)   # another address is not limited
        self.assertEqual(len(info), 3)

    def test_challenges_are_rate_limited_for_every_address_together(self):
        relay = cr.Relay(challenge_rate=10)
        answered = 0
        for i in range(50):
            c = Client(ip=f'10.1.0.{i}')
            c.seq = 1
            answered += len(relay.handle(c.connect(cr.INTENT_CREATE), c.addr, 0))
        self.assertEqual(answered, 10)

    def test_garbage_is_ignored(self):
        rnd = random.Random(7)
        for _ in range(3000):
            data = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 80)))
            self.assertEqual(self.relay.handle(data, ('10.0.0.9', 1), 0), [])
        base = cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(cr.INTENT_CREATE, 0, '', 1, b'A', bytes(16)))
        for _ in range(3000):   # a CONNECT with bytes changed: never an exception
            d = bytearray(base)
            for _ in range(rnd.randrange(1, 4)):
                d[rnd.randrange(len(d))] = rnd.getrandbits(8)
            self.relay.handle(bytes(d), ('10.0.0.9', 1), 0)
            self.relay.handle(bytes(d[:rnd.randrange(len(d))]), ('10.0.0.9', 1), 0)

    def test_log_names_no_addresses_unless_asked(self):
        a = Client('Alice')
        a.join(self.relay, 0, cr.INTENT_CREATE)
        text = '\n'.join(self.log)
        self.assertIn('Alice joined as member 1', text)
        self.assertNotIn('10.0.0.1', text)
        relay = cr.Relay(log=self.log.append, log_addrs=True)
        Client('Bob').join(relay, 0, cr.INTENT_CREATE)
        self.assertIn('10.0.0.1', self.log[-1])


# ---- over real sockets

class UdpTest(unittest.TestCase):
    def setUp(self):
        self.relay = cr.Relay(allow_chosen_codes=True)
        self.thread, self.stop, self.port = cr.run_in_thread(self.relay)
        self.addCleanup(self.thread.join, 2)
        self.addCleanup(self.stop.set)
        self.socks = []

    def sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(2)
        s.bind(('127.0.0.1', 0))
        self.addCleanup(s.close)
        return s

    def exchange(self, s, data):
        s.sendto(data, ('127.0.0.1', self.port))
        return cr.parse(s.recvfrom(2048)[0])

    def handshake(self, s, intent, code, name, seq=1, cid=None):
        cid = random.getrandbits(64) if cid is None else cid
        payload = cr.pack_connect(intent, 0, code, cid, name, bytes(16))
        ch = self.exchange(s, cr.pack(cr.T_CONNECT, 0, 0, 0, seq, payload))
        self.assertEqual(ch.type, cr.T_CHALLENGE)
        payload = cr.pack_connect(intent, 0, code, cid, name, ch.payload)
        return self.exchange(s, cr.pack(cr.T_CONNECT, 0, 0, 0, seq, payload, cr.room_key(code) if code else None))

    def test_two_clients_over_udp(self):
        a, b = self.sock(), self.sock()
        ja = self.handshake(a, cr.INTENT_CREATE, 'H7K3WM', b'A')
        self.assertEqual((ja.type, ja.payload[0], ja.payload[1:7]), (cr.T_JOINED, 1, b'H7K3WM'))
        key = cr.room_key('H7K3WM')
        jb = self.handshake(b, cr.INTENT_JOIN, 'H7K3WM', b'B')
        self.assertEqual((jb.type, jb.payload[0]), (cr.T_JOINED, 2))
        roster = cr.parse(a.recvfrom(2048)[0])   # a hears about b
        self.assertEqual(roster.type, cr.T_ROSTER)
        d = cr.pack(cr.T_DATA, ja.room, 1, cr.DST_ALL, 10, b'\xf0to everyone', key)
        a.sendto(d, ('127.0.0.1', self.port))
        self.assertEqual(b.recvfrom(2048)[0], d)
        d2 = cr.pack(cr.T_DATA, ja.room, 2, 1, 11, b'\xf0to a', key)
        b.sendto(d2, ('127.0.0.1', self.port))
        self.assertEqual(a.recvfrom(2048)[0], d2)
        pong = self.exchange(a, cr.pack(cr.T_PING, ja.room, 1, 0, 12, b'\x01\x02\x03\x04', key))
        self.assertEqual((pong.type, pong.payload), (cr.T_PONG, b'\x01\x02\x03\x04'))

    def test_the_command_line_prints_where_it_listens(self):
        p = subprocess.Popen([sys.executable, str(RELAY_PY), '--host', '127.0.0.1', '--port', '0', '--allow-chosen-codes'],
                             stdout=subprocess.PIPE, text=True)
        self.addCleanup(p.stdout.close)
        self.addCleanup(p.wait, 5)
        self.addCleanup(p.kill)
        line = p.stdout.readline()
        self.assertIn('listening on 127.0.0.1:', line)
        port = int(line.split('listening on 127.0.0.1:')[1].split()[0])
        s = self.sock()
        s.sendto(cr.pack(cr.T_CONNECT, 0, 0, 0, 1, cr.pack_connect(cr.INTENT_CREATE, 0, 'H7K3WM', 1, b'A', bytes(16))),
                 ('127.0.0.1', port))
        self.assertEqual(cr.parse(s.recvfrom(2048)[0]).type, cr.T_CHALLENGE)


# ---- the C client: --probe-coop against a relay started here

EXE = os.environ.get('NX2NAVI_EXE')


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Relay_proc:
    """tools/coop_relay.py as a process on a fixed port, so that it can be stopped and started again."""

    def __init__(self, port):
        self.port, self.p = port, None

    def start(self):
        self.p = subprocess.Popen([sys.executable, str(RELAY_PY), '--host', '127.0.0.1', '--port', str(self.port),
                                   '--allow-chosen-codes'], stdout=subprocess.PIPE, text=True)
        assert 'listening' in self.p.stdout.readline()

    def stop(self):
        if self.p:
            self.p.kill()
            self.p.wait()
            self.p.stdout.close()
            self.p = None


@unittest.skipUnless(EXE and Path(EXE).is_file(), 'set NX2NAVI_EXE to a built game executable to run --probe-coop')
class ProbeTest(unittest.TestCase):
    """The probe starts two sessions in one process and joins them through the relay at NX2NAVI_COOP_RELAY. Between
    its phases it asks for a relay restart on its output and waits for a line on its input."""

    def run_probe(self, drop):
        port = free_udp_port()
        relay = Relay_proc(port)
        relay.start()
        self.addCleanup(relay.stop)
        env = dict(os.environ, NX2NAVI_COOP_RELAY=f'127.0.0.1:{port}', NX2NAVI_COOP_DROP=str(drop),
                   NX2NAVI_COOP_VECTORS=str(VECTORS), NX2NAVI_COOP_RESTART='stdin')
        p = subprocess.Popen([EXE, '--probe-coop'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=env)
        lines = []
        deadline = time.monotonic() + 240
        for line in p.stdout:
            lines.append(line.rstrip())
            if 'coop-probe: restart the relay' in line:
                relay.stop()
                time.sleep(0.3)
                relay.start()
                p.stdin.write('restarted\n')
                p.stdin.flush()
            if time.monotonic() > deadline:
                p.kill()
                break
        p.stdin.close()
        p.stdout.close()
        code = p.wait()
        text = '\n'.join(lines)
        if os.environ.get('COOP_TEST_VERBOSE'):
            print(text)
        self.assertEqual(code, 0, text)
        self.assertIn('coop-probe: passed', text)
        self.assertIn('restart', text)

    def test_probe_without_loss(self):
        self.run_probe(0)

    def test_probe_with_30_percent_loss(self):
        self.run_probe(30)


if __name__ == '__main__':
    if '--write-vectors' in sys.argv:
        write_vectors()
        print(f'wrote {VECTORS}')
    else:
        unittest.main()
