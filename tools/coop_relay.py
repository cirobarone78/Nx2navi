#!/usr/bin/env python3
"""NX2navi co-op relay: forwards datagrams between the players of a room (up to 8), nothing else. Standard library only.

Usage: python tools/coop_relay.py [--host 0.0.0.0] [--port 24680] [--allow-chosen-codes] [--log-addrs]

The game's co-op layer (runtime/coop.c) talks to it over UDP; any player can run one on a machine the others can reach
(forward the UDP port on a router) and put its address into the game's coop_server setting. docs/coop.md describes the
setup. The relay keeps only who is in which room: it never reads what the players send each other, stores nothing on
disk, and a restart loses the rooms (the players create one again).

The wire format is runtime/coop_proto.h's; tests/coop_vectors.txt holds byte vectors both sides are checked against.
A datagram is a 24-byte header, little-endian, then a payload:

    0  u16 magic 0x434E        2  u8 version (1; the relay accepts that one)   3  u8 type
    4  u32 room (0 until joined)   8  u8 src member 1..8 (0 = relay)   9  u8 dst member (0xFF: every other member)
    10 u16 payload length      12 u32 seq (the sender's datagram counter)
    16 u8[8] tag = HMAC-SHA256(key, header[0:16] + payload)[:8], key = SHA256("nx2navi-coop/1" + code)[:16]

Joining is a cookie exchange, so a spoofed source address cannot make the relay send anything larger than it
received: the first CONNECT (padded to 64 bytes) is answered with a CHALLENGE holding a cookie of the sender's address
and the minute; the CONNECT that carries the cookie joins. The room code is the shared secret: the relay and the members
know it, a datagram with a wrong tag is dropped, and nothing is encrypted.
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import secrets
import select
import socket
import struct
import sys
import threading
import time

# ---- the wire format

MAGIC = 0x434E
PROTO = 1
HDR_LEN = 24
MAX_DGRAM = 1200
DST_ALL = 0xFF
ALPHABET = '23456789BCDFGHJKMNPRSTVWXYZ'   # no vowels, no 0/1/O/I/L, no Q or E: 27 symbols
CODE_LEN = 6
MAX_MEMBERS = 8
NAME_MAX = 24
COOKIE_LEN = 16
CONNECT_MIN = 64   # a datagram's smallest size for a CONNECT: the CHALLENGE answering it is smaller

T_CONNECT, T_CHALLENGE, T_JOINED, T_ERROR, T_ROSTER, T_PING, T_PONG, T_LEAVE, T_DATA = 1, 2, 3, 5, 6, 7, 8, 9, 0x10
INTENT_CREATE, INTENT_JOIN = 1, 2
E_NO_ROOM, E_ROOM_FULL, E_PROTO, E_RATE, E_BAD_TAG, E_SERVER_FULL = 1, 2, 3, 4, 5, 6
FLAG_AWAY = 1

# ---- timing (seconds)
SILENT_AWAY = 8       # a member that sent nothing for this long shows as away in the roster
SILENT_DROP = 15      # and is dropped after this; its member id stays reserved for a rejoin
REJOIN_KEEP = 60      # a dropped member's id is kept this long after its last datagram
ROOM_EMPTY_TTL = 600  # a room with nobody in it is deleted after this
ROSTER_EVERY = 5      # the roster is sent again this often: a lost update repairs itself
ROSTER_AGAIN = (0.25, 0.75)   # and after a change, again this long after it: three chances against loss, as in coop.c's CONNECT bursts


def normalize_code(text):
    """The canonical 6-character room code of text (any case, '-' and spaces ignored), or None when it is not one."""
    out = []
    for ch in text:
        if ch in '- ':
            continue
        o = ord(ch)
        if 97 <= o <= 122:
            ch = chr(o - 32)
        out.append(ch)
    code = ''.join(out)
    return code if len(code) == CODE_LEN and all(c in ALPHABET for c in code) else None


def format_code(code):
    return code[:3] + '-' + code[3:]


def random_code():
    return ''.join(secrets.choice(ALPHABET) for _ in range(CODE_LEN))


def room_key(code):
    return hashlib.sha256(b'nx2navi-coop/1' + code.encode('ascii')).digest()[:16]


def make_tag(key, hdr16, payload):
    return hmac.new(key, hdr16 + payload, hashlib.sha256).digest()[:8]


class Dgram:
    """A parsed datagram: the header fields, its first 16 bytes (what the tag covers) and the payload."""
    __slots__ = ('ver', 'type', 'room', 'src', 'dst', 'seq', 'tag', 'payload', 'hdr16')

    def __init__(self, ver, type_, room, src, dst, seq, tag, payload, hdr16):
        self.ver, self.type, self.room, self.src, self.dst = ver, type_, room, src, dst
        self.seq, self.tag, self.payload, self.hdr16 = seq, tag, payload, hdr16

    def verify(self, key):
        return hmac.compare_digest(make_tag(key, self.hdr16, self.payload), self.tag)


def pack(type_, room, src, dst, seq, payload=b'', key=None, ver=PROTO):
    """One datagram. key None: the tag is zero (a CONNECT that has no code yet, answers to one)."""
    hdr = struct.pack('<HBBIBBHI', MAGIC, ver, type_, room, src, dst, len(payload), seq & 0xFFFFFFFF)
    return hdr + (make_tag(key, hdr, payload) if key else bytes(8)) + payload


def parse(data):
    """The Dgram in data, or None when it is not one: wrong size, magic, or a length that does not match."""
    if len(data) < HDR_LEN or len(data) > MAX_DGRAM:
        return None
    magic, ver, type_, room, src, dst, length, seq = struct.unpack_from('<HBBIBBHI', data)
    if magic != MAGIC or length != len(data) - HDR_LEN:
        return None
    return Dgram(ver, type_, room, src, dst, seq, data[16:24], data[HDR_LEN:], data[:16])


def clean_name(raw):
    """A display name from the bytes a client sent: at most NAME_MAX bytes of valid UTF-8 without control characters
    (each of those, and each invalid byte, becomes '?')."""
    text = raw.decode('utf-8', errors='replace')
    text = ''.join('?' if c == '\ufffd' or ord(c) < 32 or ord(c) == 127 else c for c in text)
    out = b''
    for c in text:
        enc = c.encode('utf-8')
        if len(out) + len(enc) > NAME_MAX:
            break
        out += enc
    return out


def pack_roster(members):
    """members: (id, flags, name bytes) in id order."""
    out = bytes([len(members)])
    for mid, flags, name in members:
        out += bytes([mid, flags, len(name)]) + name
    return out


def parse_roster(payload, offset=0):
    """((id, flags, name bytes), ...) from payload[offset:], or None when it is cut short or malformed."""
    if len(payload) <= offset:
        return None
    n, at, out = payload[offset], offset + 1, []
    if n > MAX_MEMBERS:
        return None
    for _ in range(n):
        if at + 3 > len(payload):
            return None
        mid, flags, nlen = payload[at], payload[at + 1], payload[at + 2]
        at += 3
        if nlen > NAME_MAX or at + nlen > len(payload):
            return None
        out.append((mid, flags, payload[at:at + nlen]))
        at += nlen
    return tuple(out)


def pack_connect(intent, flags, code, client_id, name, cookie):
    """The CONNECT payload. code: the 6 characters, or '' (create: the relay picks one). Zero-padded so that the
    datagram is CONNECT_MIN bytes at least."""
    body = struct.pack('<BB6sQB', intent, flags, code.encode('ascii') if code else bytes(6), client_id, len(name))
    body += name + cookie
    return body + bytes(max(0, CONNECT_MIN - HDR_LEN - len(body)))


def parse_connect(payload):
    """(intent, flags, code, client_id, name bytes, cookie) or None. code: '' when the field is zero, else the canonical
    room code (the wire carries canonical codes only: a client normalises what the player typed)."""
    if len(payload) < 17:
        return None
    intent, flags, raw, cid, nlen = struct.unpack_from('<BB6sQB', payload)
    if nlen > NAME_MAX or 17 + nlen + COOKIE_LEN > len(payload):
        return None
    code = '' if raw == bytes(6) else raw.decode('latin-1')
    if code and normalize_code(code) != code:
        return None
    return intent, flags, code, cid, payload[17:17 + nlen], payload[17 + nlen:17 + nlen + COOKIE_LEN]


def pack_error(reason, text):
    t = text.encode('utf-8')[:60]
    return bytes([reason, len(t)]) + t


def parse_error(payload):
    """(reason, text) or None."""
    if len(payload) < 2 or 2 + payload[1] > len(payload):
        return None
    return payload[0], payload[2:2 + payload[1]].decode('utf-8', errors='replace')


# ---- the relay

class Bucket:
    """A token bucket: rate tokens per second up to burst."""
    __slots__ = ('rate', 'burst', 'tokens', 'stamp')

    def __init__(self, rate, burst, now):
        self.rate, self.burst, self.tokens, self.stamp = rate, burst, burst, now

    def take(self, n, now):
        self.tokens = min(self.burst, self.tokens + (now - self.stamp) * self.rate)
        self.stamp = now
        if self.tokens < n:
            return False
        self.tokens -= n
        return True


class Member:
    __slots__ = ('id', 'cid', 'name', 'addr', 'last_seen', 'away', 'live', 'connect_seq', 'pkts', 'bytes')

    def __init__(self, mid, cid, name, addr, now, connect_seq, pkts, bytes_):
        self.id, self.cid, self.name, self.addr = mid, cid, name, addr
        self.last_seen, self.away, self.live, self.connect_seq = now, False, True, connect_seq
        self.pkts, self.bytes = pkts, bytes_


class Room:
    __slots__ = ('id', 'code', 'key', 'members', 'empty_since', 'next_roster', 'again')

    def __init__(self, rid, code, now):
        self.id, self.code, self.key = rid, code, room_key(code)
        self.members = {}   # member id -> Member, a dropped one (live False) keeps its id for a rejoin
        self.empty_since = now
        self.next_roster = now + ROSTER_EVERY
        self.again = []     # when the roster goes out once more after a change

    def live(self):
        return [m for m in self.members.values() if m.live]


class Relay:
    """The relay's state machine. handle() takes one received datagram and tick() the passage of time; both return the
    datagrams to send as (address, bytes). now is a monotonic clock in seconds that the caller supplies, so a test
    can run the timeouts without waiting. An address is the tuple socket.recvfrom gives."""

    def __init__(self, allow_chosen_codes=False, max_rooms=1000, log=None, log_addrs=False, secret=None,
                 pkt_rate=200, pkt_burst=400, byte_rate=65536, byte_burst=131072, join_rate=1 / 3, join_burst=20,
                 challenge_rate=500):
        self.allow_chosen_codes = allow_chosen_codes
        self.max_rooms = max_rooms
        self.log_fn = log
        self.log_addrs = log_addrs
        self.secret = secret or secrets.token_bytes(32)
        self.limits = (pkt_rate, pkt_burst, byte_rate, byte_burst)
        self.join_limit = (join_rate, join_burst)
        self.challenge_rate = challenge_rate
        self.rooms = {}        # room id -> Room
        self.codes = {}        # code -> Room
        self.by_addr = {}      # address -> (Room, Member)
        self.join_buckets = {}  # ip -> Bucket
        self.challenges = None
        self.seq = 0
        self.stats = {k: 0 for k in ('rx', 'malformed', 'bad_tag', 'no_member', 'wrong_addr', 'rate_limited',
                                     'challenges', 'joins', 'rejoins', 'leaves', 'drops', 'forwarded', 'rooms_made')}

    # -- helpers

    def log(self, text):
        if self.log_fn:
            self.log_fn(text)

    def where(self, addr):
        return f' ({addr[0]}:{addr[1]})' if self.log_addrs else ''

    def next_seq(self):
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        return self.seq

    def cookie(self, addr, minute):
        return hmac.new(self.secret, f'{addr[0]}|{addr[1]}|{minute}'.encode(), hashlib.sha256).digest()[:COOKIE_LEN]

    def cookie_ok(self, addr, cookie, now):
        minute = int(now // 60)
        return any(hmac.compare_digest(cookie, self.cookie(addr, m)) for m in (minute, minute - 1))

    def to_member(self, room, member, type_, payload=b''):
        return member.addr, pack(type_, room.id, 0, member.id, self.next_seq(), payload, room.key)

    def error(self, addr, reason, text, key=None):
        return [(addr, pack(T_ERROR, 0, 0, 0, self.next_seq(), pack_error(reason, text), key))]

    def roster_payload(self, room):
        return pack_roster([(m.id, FLAG_AWAY if m.away else 0, m.name) for m in sorted(room.live(), key=lambda m: m.id)])

    def roster_to_all(self, room, skip=None, changed_at=None):
        """The roster to every live member but skip. changed_at (the time): the roster changed, so it goes out again soon
        (ROSTER_AGAIN) for a member that lost this one."""
        if changed_at is not None:
            room.again = [changed_at + t for t in ROSTER_AGAIN]
        payload = self.roster_payload(room)
        return [self.to_member(room, m, T_ROSTER, payload) for m in room.live() if m is not skip]

    def joined(self, room, member):
        payload = bytes([member.id]) + room.code.encode('ascii') + self.roster_payload(room)
        return [self.to_member(room, member, T_JOINED, payload)]

    def new_member_limits(self, now):
        pkt_rate, pkt_burst, byte_rate, byte_burst = self.limits
        return Bucket(pkt_rate, pkt_burst, now), Bucket(byte_rate, byte_burst, now)

    def forget(self, room, member):
        """Takes member out of the address index; the caller drops it from the room."""
        if self.by_addr.get(member.addr[:2], (None, None))[1] is member:
            del self.by_addr[member.addr[:2]]

    # -- datagrams

    def handle(self, data, addr, now):
        self.stats['rx'] += 1
        dg = parse(data)
        if dg is None:
            self.stats['malformed'] += 1
            return []
        if dg.ver != PROTO:
            # a client of another protocol major: tell it so, if the answer is no larger than what it sent (a spoofed
            # source gains nothing) and the relay is not already busy with challenges
            out = self.error(addr, E_PROTO, f'relay speaks protocol {PROTO}')
            return out if len(out[0][1]) <= len(data) and self.take_challenge(now) else []
        if dg.type == T_CONNECT:
            return self.connect(dg, data, addr, now)
        room = self.rooms.get(dg.room)
        member = room.members.get(dg.src) if room else None
        if member is None or not member.live and dg.type != T_LEAVE:
            self.stats['no_member'] += 1
            return []
        if member.addr != addr:
            self.stats['wrong_addr'] += 1
            return []
        if not (member.pkts.take(1, now) and member.bytes.take(len(data), now)):
            self.stats['rate_limited'] += 1
            return []
        if not dg.verify(room.key):
            self.stats['bad_tag'] += 1
            return []
        out = []
        if not member.live:   # dropped, its id reserved: a LEAVE gives the id up, anything else is ignored
            del room.members[member.id]
            return out
        member.last_seen = now
        if member.away:
            member.away = False
            out += self.roster_to_all(room, changed_at=now)
        if dg.type == T_PING and len(dg.payload) >= 4:
            out.append(self.to_member(room, member, T_PONG, dg.payload[:4]))
        elif dg.type == T_LEAVE:
            out += self.leave(room, member, now, 'left')
        elif dg.type == T_DATA:
            out += self.forward(room, member, dg, data)
        return out

    def take_challenge(self, now):
        if self.challenges is None:
            self.challenges = Bucket(self.challenge_rate, self.challenge_rate, now)
        return self.challenges.take(1, now)

    def forward(self, room, member, dg, data):
        if dg.dst == DST_ALL:
            targets = [m for m in room.live() if m is not member]
        else:
            target = room.members.get(dg.dst)
            targets = [target] if target and target.live and target is not member else []
        self.stats['forwarded'] += len(targets)
        return [(m.addr, data) for m in targets]

    def connect(self, dg, data, addr, now):
        c = parse_connect(dg.payload)
        if c is None:
            self.stats['malformed'] += 1
            return []
        intent, _flags, code, cid, raw_name, cookie = c
        if intent not in (INTENT_CREATE, INTENT_JOIN):
            self.stats['malformed'] += 1
            return []
        code_norm = code or None
        key = room_key(code_norm) if code_norm else None   # the code is in the CONNECT: the tag proves integrity only
        if not self.cookie_ok(addr, cookie, now):
            # not yet proven to receive at its address: only a challenge, and only if that is no larger than the request
            if len(data) < CONNECT_MIN or not self.take_challenge(now):
                return []
            self.stats['challenges'] += 1
            return [(addr, pack(T_CHALLENGE, 0, 0, 0, self.next_seq(), self.cookie(addr, int(now // 60)), key))]
        if key is not None and not dg.verify(key):
            self.stats['bad_tag'] += 1
            return self.error(addr, E_BAD_TAG, 'bad tag', key)
        known = self.by_addr.get(addr[:2])
        if known and known[1].cid == cid and known[1].connect_seq == dg.seq and known[1].live:
            known[1].last_seen = now   # a CONNECT sent again because its answer was lost: the answer again
            return self.joined(known[0], known[1])
        bucket = self.join_buckets.get(addr[0])
        if bucket is None:
            bucket = self.join_buckets[addr[0]] = Bucket(self.join_limit[0], self.join_limit[1], now)
        if not bucket.take(1, now):
            self.stats['rate_limited'] += 1
            return self.error(addr, E_RATE, 'too many joins, try again later', key)
        name = clean_name(raw_name) or b'Player'
        out = []
        if intent == INTENT_JOIN:
            room = self.codes.get(code_norm) if code_norm else None
            if room is None:
                return self.error(addr, E_NO_ROOM, 'no such room', key)
        else:
            room = self.codes.get(code_norm) if code_norm and self.allow_chosen_codes else None
            if room is None:
                if len(self.rooms) >= self.max_rooms:
                    return self.error(addr, E_SERVER_FULL, 'server full', key)
                room = self.make_room(code_norm if code_norm and self.allow_chosen_codes else None, now)
        if known and known[0] is not room:
            out += self.leave(known[0], known[1], now, 'moved to another room')   # same address, new room: it left the old
        member = next((m for m in room.members.values() if m.cid == cid), None)
        if member is None:
            mid = next((i for i in range(1, MAX_MEMBERS + 1) if i not in room.members), None)
            if mid is None:   # every id is taken: a dropped member's id goes to the newcomer, the longest gone first
                gone = sorted((m for m in room.members.values() if not m.live), key=lambda m: m.last_seen)
                if not gone:
                    return out + self.error(addr, E_ROOM_FULL, 'room full', key)
                mid = gone[0].id
                del room.members[mid]
            pkts, byts = self.new_member_limits(now)
            member = Member(mid, cid, name, addr, now, dg.seq, pkts, byts)
            room.members[mid] = member
            self.stats['joins'] += 1
            self.log(f'room {format_code(room.code)}: {name.decode()} joined as member {mid} '
                     f'({len(room.live())}/{MAX_MEMBERS}){self.where(addr)}')
        else:   # the same client again (a rejoin after the relay was silent, a new address, a new name)
            if member.addr != addr:
                self.forget(room, member)
            if not member.live:
                self.log(f'room {format_code(room.code)}: {name.decode()} rejoined as member {member.id}{self.where(addr)}')
                self.stats['rejoins'] += 1
            member.addr, member.name, member.connect_seq = addr, name, dg.seq
            member.last_seen, member.away, member.live = now, False, True
        old = self.by_addr.get(addr[:2])
        if old and old[1] is not member:   # another member of this room on the same address: that one is gone
            self.forget(old[0], old[1])
            old[0].members.pop(old[1].id, None)
        self.by_addr[addr[:2]] = (room, member)
        room.empty_since = None
        return out + self.joined(room, member) + self.roster_to_all(room, skip=member, changed_at=now)

    def make_room(self, code, now):
        while code is None or code in self.codes:
            code = random_code()
        rid = 0
        while not rid or rid in self.rooms:
            rid = secrets.randbits(32)
        room = Room(rid, code, now)
        self.rooms[rid] = room
        self.codes[code] = room
        self.stats['rooms_made'] += 1
        self.log(f'room {format_code(code)} created ({len(self.rooms)} rooms)')
        return room

    def leave(self, room, member, now, why):
        self.forget(room, member)
        room.members.pop(member.id, None)
        self.stats['leaves'] += 1
        self.log(f'room {format_code(room.code)}: {member.name.decode()} (member {member.id}) {why} '
                 f'({len(room.live())}/{MAX_MEMBERS})')
        if not room.live():
            room.empty_since = now
        return self.roster_to_all(room, changed_at=now)

    # -- time

    def tick(self, now):
        out = []
        for room in list(self.rooms.values()):
            changed = False
            for m in list(room.members.values()):
                silent = now - m.last_seen
                if m.live and silent >= SILENT_DROP:
                    m.live = False
                    self.forget(room, m)
                    self.stats['drops'] += 1
                    self.log(f'room {format_code(room.code)}: {m.name.decode()} (member {m.id}) dropped, silent '
                             f'{SILENT_DROP} s ({len(room.live())}/{MAX_MEMBERS})')
                    changed = True
                elif m.live and not m.away and silent >= SILENT_AWAY:
                    m.away = True
                    changed = True
                elif not m.live and silent >= REJOIN_KEEP:
                    del room.members[m.id]
            if not room.live() and room.empty_since is None:
                room.empty_since = now
            if room.empty_since is not None and now - room.empty_since >= ROOM_EMPTY_TTL:
                del self.rooms[room.id]
                del self.codes[room.code]
                self.log(f'room {format_code(room.code)} closed, empty for {ROOM_EMPTY_TTL // 60} minutes '
                         f'({len(self.rooms)} rooms)')
                continue
            again = any(t <= now for t in room.again)
            if again:
                room.again = [t for t in room.again if t > now]
            if changed or again or now >= room.next_roster:
                out += self.roster_to_all(room, changed_at=now if changed else None)
                room.next_roster = now + ROSTER_EVERY
        for ip, b in list(self.join_buckets.items()):
            if now - b.stamp > 120:
                del self.join_buckets[ip]
        return out


# ---- the socket loop

def open_socket(host, port):
    family, _, _, _, sockaddr = socket.getaddrinfo(host, port, type=socket.SOCK_DGRAM, flags=socket.AI_PASSIVE)[0]
    s = socket.socket(family, socket.SOCK_DGRAM)
    if family == socket.AF_INET6:
        s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    s.bind(sockaddr)
    s.setblocking(False)
    return s


def serve(relay, sock, stop=None, clock=time.monotonic):
    """Runs relay on sock until stop (a threading.Event) is set."""

    def send_all(datagrams):
        for to, out in datagrams:
            try:
                sock.sendto(out, to)
            except OSError:
                pass   # a datagram that cannot go is a lost datagram

    last_tick = clock()
    while not (stop and stop.is_set()):
        if select.select([sock], [], [], 0.1)[0]:
            for _ in range(512):
                try:
                    data, addr = sock.recvfrom(2048)
                except OSError:   # nothing more to read (or an ICMP error reported on Windows)
                    break
                send_all(relay.handle(data, addr, clock()))
        now = clock()
        if now - last_tick >= 0.25:
            last_tick = now
            send_all(relay.tick(now))


def run_in_thread(relay, host='127.0.0.1', port=0):
    """Starts serve() on a new socket in a thread (tests): (thread, stop event, bound port)."""
    sock = open_socket(host, port)
    stop = threading.Event()
    t = threading.Thread(target=lambda: (serve(relay, sock, stop), sock.close()), daemon=True)
    t.start()
    return t, stop, sock.getsockname()[1]


def main():
    ap = argparse.ArgumentParser(description='NX2navi co-op relay (see docs/coop.md)')
    ap.add_argument('--host', default='0.0.0.0', help='address to listen on (default 0.0.0.0; :: for IPv6 and IPv4)')
    ap.add_argument('--port', type=int, default=24680, help='UDP port (default 24680; 0 picks a free one)')
    ap.add_argument('--allow-chosen-codes', action='store_true',
                    help='a create that names a room code makes that room, or joins it when it exists (tests, LAN '
                         'parties); otherwise the relay picks every code')
    ap.add_argument('--log-addrs', action='store_true', help="write the players' addresses to the log (off by default)")
    ap.add_argument('--max-rooms', type=int, default=1000, help='rooms at most (default 1000)')
    a = ap.parse_args()

    def log(text):
        print(time.strftime('%H:%M:%S ') + text, flush=True)

    relay = Relay(allow_chosen_codes=a.allow_chosen_codes, max_rooms=a.max_rooms, log=log, log_addrs=a.log_addrs)
    sock = open_socket(a.host, a.port)
    log(f'coop_relay: listening on {a.host}:{sock.getsockname()[1]}'
        f'{" (chosen room codes allowed)" if a.allow_chosen_codes else ""}')
    try:
        serve(relay, sock)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
