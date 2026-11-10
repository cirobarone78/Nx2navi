# Co-op

Each player runs their own game and save; the co-op layer connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: the layer is a host thread of the runtime
(`runtime/coop.c`) with its own UDP socket.

What exists so far: rooms, the relay, a sealed channel for messages between the players, and progress sync (below).
Showing the other players in the world and the menu tab are not there yet; until the tab is, the settings below are in
`nx2navi.ini` only.

## What joining a room means for your save

Read this before you join a room.

- **Nothing from a room reaches your game until you accept it.** When you join, the log names the other players and
  the room's rules and gives the room's offer a number. Until you put that number into `coop_accept` (or accept in the
  menu, once it exists), your game takes nothing from the room. If a new player joins later, or the room's maker
  changes the rules, that is a new offer: nothing more is taken until you accept it too. `coop_accept = always`
  accepts every room without asking.
- **Your save is copied first.** The moment you accept a room for the first time in a session, the game copies your
  whole save (every slot) to `coop-backup/<date-time>/` in the profile folder, and takes nothing from the room until the
  copy is made. The newest eight copies are kept. "Going back" below says how to restore one.
- **What the room can change in your save** is what its rules share (progress, items, songs, quest items; small keys,
  rupees and ammunition in a pool): every value is checked against your game's own table of flags before it is
  stored, passed on or applied. A flag can only be set to "done", an item count only up to 999, small keys only up to
  15, a pooled counter only within its capacity. Anything else is dropped, counted in the log, and a player who sends a
  few dozen of those is ignored for as long as they stay in the room.
- **What a player you let in can still do.** A player in the room plays with you: with progress shared, what they do
  in their world (open a chest, beat a boss, set a story flag) happens in yours, and a player who wants to can send
  such flags without playing for them. With small keys in a pool, a player can spend the keys everyone shares, and
  with rupees or ammunition in a pool (off by default) the rupees or ammunition. None of this can be undone inside the
  game: go back to the copy of your save. Invite only people you trust with your save, as you would hand them your
  controller.
- **Who can get in.** Only someone who knows the room's secret: 18 characters (85 bits) that the game makes when a room
  is made and that the maker passes on to the others (by voice, a chat message). The relay never learns it. Someone who
  sees your traffic on the network, or who runs the relay, cannot read the messages, change them, make new ones or
  guess the secret from them.
- **Removing a player.** The room's maker can remove a player (`coop_remove = <member id>`): the relay drops them and
  refuses them again under the same identity. Someone who knows the secret can still come back as a new player (and
  everyone then gets a new offer to accept or not); to be sure, make a new room, which has a new secret.

### Who can see what

| Who | Sees | Cannot |
|---|---|---|
| The relay's operator, and anyone on the network path | that a room exists (a random id and the locator, a value derived from the secret that does not give the secret back), the players' names, their IP addresses, when and how much they send | read, change or make any message between the players; learn the secret; change anyone's save |
| The other players in the room | your name, your progress (what the rules share), your messages | your IP address (the relay forwards everything), your account or profile (the id the relay knows you by is random for each start of the game and never shown to them) |
| Your log and "Copy system info" | the room's first three characters and the offer's number; system info shows that `coop_room`, `coop_server`, `coop_name` and `coop_accept` are set, not their values | the secret, the relay's address (it appears only with `NX2NAVI_TRACE=coop`) |

### Going back to your save from before a session

1. Close the game.
2. In the profile folder, open `coop-backup` and pick the copy to go back to (the folder name is the date and time, in
   UTC, at which the room was accepted).
3. Delete the folder `save-tx` (it may hold a save that was being written; it would otherwise be finished over the copy
   at the next start).
4. Move the folder `save/<numbers>` aside (keep it until you are sure), and copy the contents of the backup folder into
   a new `save/<numbers>` of the same name.
5. Start the game: every slot is as it was when you accepted the room. Progress made after that, in any slot, is in the
   folder you moved aside, not in the copy.

## Running a relay

```
python tools/coop_relay.py [--host 0.0.0.0] [--port 24680] [--log-addrs] [--max-rooms 1000] [--rooms-per-network 8]
```

Python 3, standard library only. A player who can reach it by UDP (forward the port on the router, open it in the
firewall) is all it takes; the others put its address into `coop_server`. The relay forwards the sealed datagrams of a
room and can read none of what the players send each other. It keeps only who is in which room, writes nothing to
disk, and loses its rooms when it stops: the room's maker makes it again under the same secret. Its log has one line
per room made, join, leave and removal, with names, member ids and a random room id (never the secret or the locator);
addresses only with `--log-addrs`.

- A room holds up to 8 players, is closed 10 minutes after the last one left (a minute, if nobody but its maker was ever
  in it), and a player the relay has not heard from for 15 s is dropped (the member id is kept for a minute so that the
  same game, with its rejoin token, joins again as the same member).
- The roster (who is in the room, each one's session, which one made it) goes out at a change, again 0.25 s and 0.75 s
  later, and every 5 s: a datagram is lost now and then, and nothing is acknowledged.
- Limits: a source address answers a cookie challenge before the relay sends it anything larger than it sent; each
  member is limited to 200 datagrams and 64 KB per second, a datagram forwarded to seven counted seven times; each
  network (an IPv4 /24, an IPv6 /64) to 20 challenges a second, a join every 3 s after a burst of 20 (a join of a room
  that is not there costs three), and 8 open rooms; the relay as a whole to 5000 challenges a second and 8 MB/s of
  forwarding. A member that rejoins with its token needs no challenge, so a flood of challenges does not keep the
  players of a room out of it.
- `--host ::` listens for IPv6 as well as IPv4. The game's own socket is IPv4 and IPv6 in one; Windows may show its firewall
  prompt the first time the game opens it.

## Rooms and secrets

A room is its secret: 18 characters of `23456789BCDFGHJKMNPRSTVWXYZ` (no vowels, no 0/1/O/I/L), typed in any case,
with or without `-` and spaces, shown `XXXXXX-XXXXXX-XXXXXX`. The game makes a new random one for each room it makes;
`coop_room` names one for `coop_auto` (tests, LAN parties). A create of a secret whose room is open on the relay is
refused (it is not a join), so nobody can be put into a room they did not mean to join.

## Settings

`nx2navi.ini`, section `[coop]` (`nx2navi.ini.example` has the text); the environment variable is the key with `NX2NAVI_`.

| Key | Default | |
|---|---|---|
| `coop` | `0` | on or off, at once; off: no socket and nothing running |
| `coop_name` | empty | at most 24 bytes; empty: `Player-` and four random hex digits |
| `coop_server` | none | `host`, `host:port` or `[v6]:port`, default port 24680; with none set the layer stays idle and logs why |
| `coop_port` | `0` | local UDP port, 0 = any; read at start |
| `coop_accept` | `ask` | `ask`: nothing from a room is applied until its offer's number (in the log) is put here or accepted in the menu; `always`: every room is accepted (the save is still copied first); a number: that offer |
| `coop_auto` | `off` | `create` or `join` starts a session at launch (a join needs `coop_room`); no menu row |
| `coop_room` | empty | the room secret of `coop_auto`; no menu row |
| `coop_remove` | empty | the room's maker removes the member of this id; no menu row yet |
| `coop_drop` | `0` | test: percent of received datagrams thrown away; no menu row |
| `coop_test_set` | empty | test, environment only (`NX2NAVI_COOP_TEST_SET`): `hash=value@frame,...` sets those flags (hash in hex) at those frames of play; an `nx2navi.ini` line is ignored |
| `coop_progress` ... `coop_ammo` | see below | the rules of a room this player makes (Progress sync) |

With `coop_auto` a room the relay lost (it restarted) is made again under the same secret by its maker, or waited for,
for as long as the session runs. A player who leaves ends the session. Do not use an `nx2navi.ini` someone else gave
you: it can name a room and `coop_accept = always`.

## Progress sync

When one player gains progress, the others' games gain it too, while they play; a player who joins later catches up
with everything the room has, and the room with what the joiner brings. Each player's own save keeps what it got: the
game saves it as its own progress.

The game keeps its progress as flags (a database keyed by 32-bit hashes of flag names). Between two frames the runtime
reads the flags that changed, and sets the ones the room has and this game lacks through the game's own set routines,
as if the player had done it. Nothing is read or set while the game is on its title screen, loading a save or changing
scene, and not while its clock stands (the pause menu): what arrives meanwhile is set afterwards. A chest or a door
another player opened shows open the next time the area loads; an item shows in the menu the next time it opens.

What one room merged belongs to that room and that save: leaving the room, joining another, or loading another save
forgets it, and a save loaded while in a room gets the room's progress (what the others hold), never the last save's.

### What is shared

| Category | Rule key | Modes (default first) | What |
|---|---|---|---|
| progress | `coop_progress` | `shared`, `own` | story, world and dungeon flags: events, talks, chests, doors, switches, bosses, map and compass |
| items | `coop_items` | `shared`, `own` | equipment, key items, bottles owned, wallet, bag and quiver sizes, magic meter size |
| songs | `coop_songs` | `shared`, `own` | |
| quest | `coop_quest` | `shared`, `own` | medallions, spiritual stones, the stone of agony, the gerudo card |
| keys | `coop_keys` | `pool`, `own` | small keys, per dungeon |
| rupees | `coop_rupees` | `own`, `pool` | |
| ammo | `coop_ammo` | `own`, `pool` | bombs, bombchus, arrows, deku nuts, seeds and sticks, magic beans |

- `shared`: a flag set by anyone stays set for everyone; of a count (an item, an upgrade) the highest wins.
- `own`: not synced; switching to it stops the merge and keeps what was merged.
- `pool`: one count that every player adds to and spends from. Each player's gains and spends are counted apart and add
  up, so two players spending at once both pay; a pool that two spends at once took below zero shows 0 and the next
  gain pays the debt first. A joiner whose count is higher raises the pool to it; a game that restarts and rejoins
  does not add its count again. A pool above a counter's capacity shows the capacity.
- Small keys in the pool: a key anyone picks up goes into the dungeon's pool, a door anyone opens takes one out, and the
  pickup and the door are progress, so they are gone and open for everyone. Two players who each used the last key
  on a different door at the same moment open one door more than the keys paid for.
- Items, songs and quest items can be `own` only with `coop_progress = own`: in a shared world a chest one player
  opened is empty for the others. With progress `own`, keys are `own` too (each world has its own doors). A room's maker
  whose settings break this gets the nearest valid set, and the log says which.

Never shared, by design or because no reliable way exists yet:

- Where Link is, his health (the game republishes it from the player several times a second, so it cannot be set
  through the flags), the magic in the meter (likewise outside the flags), the time of day (the clock runs in the
  game's world manager, not in a flag), bottle contents and the equipment worn.
- Heart containers and pieces and gold skulltula tokens: most of their pickups are flags of single objects in the world
  that this layer does not write, so a shared count would pay one piece or token twice; the reward flags the game names
  for them stay with each player too, so everyone can collect them.
- Items the game takes away again (the trade sequence, rented masks, the knife that breaks, amiibo items): a shared
  highest count would hand them back.
- Flags that the game itself resets at a scene change, a new day or a load, and counters that are not progress (puzzle
  positions, minigame records, enemies defeated).

### The room's rules

The rules belong to the room: every game in it merges the same way. They are the settings of the player who made the
room, whom the relay marks as its maker; no other player's settings are used, and a game takes rule changes only from
the maker's own messages. They travel with the sync messages, so a joiner and a game that rejoins after a relay
restart get them. If the maker has left, the players keep the rules they have, and a newcomer takes them from the
others (and sees them in its offer before anything is applied); nobody makes new ones. The maker can change a rule
during the session: switched to shared or pool, the category merges from then on (after each player accepts the new
offer); switched to own, it stops, and nothing already merged is taken back.

### Messages

Sync rides in the room's sealed messages. Each game sends a digest (a sum per section of its state, and the rules)
every 2 s and at once to a player who joins; a section whose sum differs is sent to that player, so lost messages are
repaired and a late joiner catches up within a few seconds. A change goes out at once and again 80 and 250 ms later.
Sending is paced at 32 KB/s, under the relay's limit per member, and each game takes at most 48 KB/s from any one
player. Games merge only with the same schema: this layer's version, the game's version, and the whole table of what
is shared and up to which value, so two builds that classify a flag differently do not merge.

## The room's secret and the wire

For a reviewer: what is derived from what, what each datagram carries and what a receiver checks. The code is
`runtime/coop_proto.c` (C) and `tools/coop_relay.py` (the relay's side, Python); `tests/coop_vectors.txt` holds byte
vectors of every message that both are checked against.

**Library.** Monocypher 4.0.2 (`runtime/third_party/monocypher/`, two files, unmodified, BSD-2-Clause, dual-licensed
with CC0) for BLAKE2b and XChaCha20-Poly1305. It is small, has no dependencies, builds the same on Windows and Linux,
and its primitives are standard (RFC 7693, and the XChaCha20-Poly1305 construction of the IETF draft). It is used only
in `coop_proto.c`, behind four functions (`coop_room_keys`, `coop_seal`, `coop_open`, and BLAKE2b inside the first), so
another library can replace it there. The relay needs no library: it uses Python's own BLAKE2b and HMAC.

**The secret.** 18 symbols, each drawn uniformly from 27 (bytes from the operating system's random source, a byte
below 243 taken modulo 27): log2(27^18) = 85.6 bits. It is made by the client that creates the room and never sent to
the relay or written to a log; players pass it on themselves.

**Derivation.** With `S` the 18 canonical ASCII characters as the key of BLAKE2b:

- locator `L` = BLAKE2b-128(key `S`, "nx2navi-coop/2 room"): the relay's name for the room, and the key of the tags of
  datagrams between a client and the relay. The relay and anyone who sees a CONNECT learn `L`; getting `S` from it is a
  search over 2^85.6 secrets.
- message key `K` = BLAKE2b-256(key `S`, "nx2navi-coop/2 messages"): seals every message between the players. Only
  holders of `S` have it.

**The client and relay datagrams.** A 24-byte header (magic, version 2, type, the relay's room id, src, dst, length,
sequence) and an 8-byte tag, HMAC-SHA256(`L`, header[0:16] ‖ payload)[0:8]. CONNECT carries `L`, a client id (random
for each start of the game), a session (random for each join), a nonce (random for each attempt), the name, the
relay's cookie and the rejoin token. CHALLENGE, JOINED and ERROR echo the nonce and carry `L`'s tag; the client takes
only answers with its own nonce and its own `L`, so a datagram someone else sends from the relay's address neither
ends an attempt nor puts the player into another room (an untagged "protocol version" error is taken only as the
first answer of an attempt). JOINED carries a rejoin token, random per member: the relay gives a member's id (and the
maker's mark) back only to a CONNECT with it. The ROSTER lists each member's id, flags (away, maker), session and
name. KICK (maker to relay) names a member and its session. These datagrams are authenticated against outsiders who
do not know `L` only; the relay and an observer of a CONNECT can make them, which lets them disturb the session
(drop, reorder, invent members that cannot speak), not change a save.

**The player message envelope** (the payload of DATA, version byte 1):

```
u8 envelope version (1) | u64 sender's session | u32 counter | XChaCha20-Poly1305 ciphertext of (u8 kind, message) | u8[16] tag
nonce (24 bytes) = session (8, LE) | counter (4, LE) | sender's member id (1) | 11 zero bytes
associated data = destination member id, envelope version
```

A session is drawn for every join and the counter starts at 1, so a nonce never repeats under `K` (two sessions
collide with probability about n²/2^65). The kind byte and the message are inside the ciphertext: a new kind of message
(a pose, say) needs only a number, and a later envelope version can change the outer fields without touching the
messages. A message is at most `COOP_MAX_MESSAGE` = 1146 bytes after its kind.

**What a receiver checks, in order:** the sender is in the roster and is not itself, the destination is it or
everyone, the envelope's session is the one the roster lists for that sender, the counter is new within a window of
the last 64 of that session (older ones are refused, not taken as a restart), the tag opens with `K` for that sender
and destination; only then is the counter recorded and the message delivered.

**Replay.** A message recorded before a player joined again carries the old session, which the roster no longer
lists: refused. A message sent again within a session is refused by the window. A relay that lists an old session in
a forged roster could replay that session's messages to a player; they are messages the sender really sent, and sync
messages only raise values, so this brings nothing new into a save.

**Rules and the maker.** The relay marks the member that created the room as its maker; only that member's digests
change the room's rules, only to a later epoch, and the epoch saturates at 2^32−1 instead of wrapping. A newcomer
that holds no rules while no maker is present takes the members' rules and sees them in its offer before anything is
applied. What remains: a member that knows the secret and controls the relay could pass for the maker.

**Per-member limits in each game.** Every value is checked against the local table before it is stored, passed on or
applied; at most 48 KB/s is taken from one member; 32 refused values and the member is ignored while it stays; the
register index is keyed (SipHash-2-4 with a random key of the process); a pool takes at most 256 contributors, a
contributor's gains or spends at most 2^20, and no member can raise this game's own contributor.

## In the log

State changes only: `coop: connecting to the relay`, `the relay answered in N ms`, `room XXX… created`, `joined room
XXX… as member N`, `<name> (member N) joined the room` and `left the room`, `relay lost; rejoining`, `left room (<why>)`.
A cause that repeats (a message that does not open, a replay, no server) logs once per 30 s; a counters line follows
every minute in a room. Peer addresses are never logged; the relay's only with `NX2NAVI_TRACE=coop`, which also adds a
line per datagram.

Progress sync: `coop: sync: playing a save: N progress flags, ...` when a save is being played (and `... nothing is read
or written until a save is played` when it stops), `a room to accept; in the room: ...; rules: ...` with the offer's
number, `room accepted`, `the save as it was before this room is in <folder>`, `the room's rules: progress shared, ...`,
`took N values from the room (progress a, items b)` at most every 5 s, `N values from the room were refused` and
`member N sent values this game does not take; nothing more is taken from it`. `NX2NAVI_TRACE=coop` adds `sent`,
`received` and `applied` lines by category, hash and value. With `coop = 0` nothing of this is logged and the game's
frames do no more than check the setting.

## The interface for the layers above

`runtime/coop.h`: `coop_send(kind, bytes, n)` and `coop_send_to(member, ...)` send a sealed message to the others (it is
a datagram: it may be lost or arrive out of order; a replay is refused), `coop_on_message(kind, fn, user)` receives
them on the coop thread, `coop_on_view(fn, user)` reports the state and the roster (with each member's maker mark and
whether its messages opened), `coop_view_get(&view)` copies them, `coop_command(COOP_CMD_CREATE | COOP_CMD_JOIN |
COOP_CMD_LEAVE, secret)` drives the room, `coop_remove_member(id)` is the maker's removal. `runtime/coop_game.h`:
`coop_game_offer(&offer)` and `coop_game_accept(number)` are the menu's way to show a room and accept it. The wire
format is `runtime/coop_proto.h`. Progress sync uses `coop_on_tick(fn, user)` for its timers.

## Checks

- `python tests/coop_relay_test.py`: the relay (rooms, rejoin with the token, timeouts, limits per member, per network
  and in all, removal by the maker, bad tags, a stolen client id, challenge and room floods, fan-out, garbage from a
  member, the log without secrets) on a fake clock and over UDP, and the codec against the vectors. With
  `NX2NAVI_EXE=<game>/ocarina` it also runs the probe against a relay it starts, with and without 30 % loss, and
  restarts that relay in the middle.
- `ocarina --probe-coop`: the sockets, the codec and its cryptography (known answers, a sealed message that does not
  open when changed or sent by another), a fuzz of every parser, and two sessions in one process that make and join a
  room, exchange messages, take garbage and forgeries from a hostile relay (a changed message, one it sealed itself,
  a replay after a rejoin, lies while a player connects), lose the relay and find each other again, and in which the
  maker removes a player. Needs no window, GPU or game files. It runs its own small relay;
  `NX2NAVI_COOP_RELAY=host:port` uses a real one, `NX2NAVI_COOP_DROP=30` drops that share of what the sessions
  receive, `NX2NAVI_COOP_VECTORS=tests/coop_vectors.txt` checks the byte vectors. Its sync part runs the merge engine
  between fake games on a fake network: merging, late joiners, loss, the rules and pools, nothing carried from one room
  or save to the next, a hostile member, and a rule takeover.
