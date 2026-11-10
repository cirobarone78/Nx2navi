# Co-op

Each player runs their own game and save; the co-op layer connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: the layer is a host thread of the runtime
(`runtime/coop.c`) with its own UDP socket.

What exists so far: rooms, the relay, a channel for messages between the players, and progress sync (below). Showing
the other players in the world and the menu tab are not there yet; until the tab is, the settings below are in
`nx2navi.ini` only.

## Running a relay

```
python tools/coop_relay.py [--host 0.0.0.0] [--port 24680] [--allow-chosen-codes] [--log-addrs] [--max-rooms 1000]
```

Python 3, standard library only. A player who can reach it by UDP (forward the port on the router, open it in the
firewall) is all it takes; the others put its address into `coop_server`. The relay forwards the datagrams of a room and
reads none of what the players send each other. It keeps only who is in which room, writes nothing to disk, and loses its
rooms when it stops: the players make a room again. Its log has one line per room made, join and leave, with names and
member ids; addresses only with `--log-addrs`.

- A room holds up to 8 players, is closed 10 minutes after the last one left, and a player the relay has not heard from for
  15 s is dropped (the member id is kept for a minute so that the same game joins again as the same member).
- The roster (who is in the room) goes out at a change, again 0.25 s and 0.75 s later, and every 5 s: a datagram is lost
  now and then, and nothing is acknowledged.
- A source address answers a cookie challenge before the relay sends it anything larger than it sent, and each member is
  limited to 200 datagrams and 64 KB per second.
- `--allow-chosen-codes`: a create that names a room code makes that room, or joins it when it exists. For tests and LAN
  parties; otherwise the relay picks every code.
- `--host ::` listens for IPv6 as well as IPv4. The game's own socket is IPv4 and IPv6 in one; Windows may show its firewall
  prompt the first time the game opens it.

## Rooms and codes

A room code is 6 characters of `23456789BCDFGHJKMNPRSTVWXYZ` (no vowels, no 0/1/O/I/L), typed in any case, with or without
`-` (shown `XXX-XXX`). The code is the room's shared secret: every datagram carries an 8-byte HMAC-SHA256 tag keyed from
it, and one with a wrong tag is thrown away by the relay and by the other players. Nothing is encrypted, and the code
travels in the clear when a player joins. Treat it as a secret among friends, not as a password.

## Settings

`nx2navi.ini`, section `[coop]` (`nx2navi.ini.example` has the text); the environment variable is the key with `NX2NAVI_`.

| Key | Default | |
|---|---|---|
| `coop` | `0` | on or off, at once; off: no socket and nothing running |
| `coop_name` | empty | at most 24 bytes; empty: `Player-` and four hex digits of the player's id |
| `coop_server` | none | `host`, `host:port` or `[v6]:port`, default port 24680; with none set the layer stays idle and logs why |
| `coop_port` | `0` | local UDP port, 0 = any; read at start |
| `coop_auto` | `off` | `create` or `join` starts a session at launch (a join needs `coop_room`); no menu row |
| `coop_room` | empty | the room code of `coop_auto`; no menu row |
| `coop_drop` | `0` | test: percent of received datagrams thrown away; no menu row |
| `coop_test_set` | empty | test: `hash=value@frame,...` sets those flags (hash in hex) at those frames of play; no menu row |
| `coop_progress` ... `coop_ammo` | see below | the rules of a room this player makes (Progress sync) |

With `coop_auto` a room the relay lost (it restarted) is made again, under the same code if the relay allows chosen codes, or
waited for, for as long as the session runs. A player who leaves ends the session.

## Progress sync

When one player gains progress, the others' games gain it too, while they play; a player who joins later catches up
with everything the room has, and the room with what the joiner brings. Nothing a player has is taken away by joining.
Each player's own save keeps what it got: the game saves it as its own progress.

The game keeps its progress as flags (a database keyed by 32-bit hashes of flag names). Between two frames the runtime
reads the flags that changed, and sets the ones the room has and this game lacks through the game's own set routines,
as if the player had done it. Nothing is read or set while the game is on its title screen, loading a save or changing
scene, and not while its clock stands (the pause menu): what arrives meanwhile is set afterwards. A chest or a door
another player opened shows open the next time the area loads; an item shows in the menu the next time it opens.

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
  pickup and the door are progress, so they are gone and open for everyone: the room never has more keys than the doors
  it can open, and a player is never left without the key another one picked up. Two players who each used the last key
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
room; a joiner's own rule settings are not used while it is in the room. They travel with the sync messages, so a
joiner and a game that rejoins after a relay restart get them; if the maker left before anyone heard them, the member
with the lowest id makes them from its settings 5 s after joining. The maker can change a rule during the session:
switched to shared or pool, the category merges from then on; switched to own, it stops, and nothing already merged is
taken back.

### Messages

Sync rides in the room's peer messages. Each game sends a digest (a sum per section of its state, and the rules) every
2 s and at once to a player who joins; a section whose sum differs is sent to that player, so lost messages are
repaired and a late joiner catches up within a few seconds. A change goes out at once and again 80 and 250 ms later.
Sending is paced at 32 KB/s, under the relay's limit per member. Games of a different version do not merge.

## In the log

State changes only: `coop: connecting to <host:port>`, `relay <ip:port> answered in N ms`, `room XXX-XXX created`, `joined
room XXX-XXX as member N`, `<name> (member N) joined the room` and `left the room`, `relay lost; rejoining`, `left room
(<why>)`. A cause that repeats (a datagram with a wrong tag, no server) logs once per 30 s; a counters line follows every
minute in a room. Peer addresses are never logged, the relay's is. `NX2NAVI_TRACE=coop` adds a line per datagram.

Progress sync: `coop: sync: playing a save: N progress flags, ...` when a save is being played (and `... nothing is read
or written until a save is played` when it stops), `the room's rules: progress shared, ...`, `took N values from the
room (progress a, items b)` at most every 5 s. `NX2NAVI_TRACE=coop` adds `sent`, `received` and `applied` lines by
category, hash and value, and when the game starts or stops running. With `coop = 0` nothing of this is logged and the
game's frames do no more than check the setting.

## The interface for the layers above

`runtime/coop.h`: `coop_send(kind, bytes, n)` and `coop_send_to(member, ...)` send a message to the others (it is a
datagram: it may be lost or arrive out of order; a datagram delivered twice is filtered), `coop_on_message(kind, fn, user)`
receives them on the coop thread, `coop_on_view(fn, user)` reports the state and the roster, `coop_view_get(&view)` copies
them, `coop_command(COOP_CMD_CREATE | COOP_CMD_JOIN | COOP_CMD_LEAVE, code)` drives the room. `coop_shutdown()` says LEAVE
and BYE when the process ends. The wire format is `runtime/coop_proto.h`; `tests/coop_vectors.txt` holds byte vectors
that both it and `tools/coop_relay.py` are checked against. Progress sync uses `coop_on_tick(fn, user)` for its timers.

## Checks

- `python tests/coop_relay_test.py`: the relay (rooms, rejoin, timeouts, limits, bad tags) on a fake clock and over UDP,
  and the codec against the vectors. With `NX2NAVI_EXE=<game>/ocarina` it also runs the probe against a relay it starts,
  with and without 30 % loss, and restarts that relay in the middle.
- `ocarina --probe-coop`: the sockets, the codec, a fuzz of every parser, and two sessions in one process that make and
  join a room, exchange messages both ways, take garbage and forged datagrams, lose the relay and find each other again.
  Needs no window, GPU or game files. It runs its own small relay; `NX2NAVI_COOP_RELAY=host:port` uses a real one (started
  with `--allow-chosen-codes`), `NX2NAVI_COOP_DROP=30` drops that share of what the sessions receive,
  `NX2NAVI_COOP_VECTORS=tests/coop_vectors.txt` checks the byte vectors. Its sync part runs the merge engine between
  fake games on a fake network: messages in every order and repeated give one state, a late joiner and the room catch up
  with each other, four players at 30 % loss end with the union, and the rules: the maker's hold, pools with spends and
  gains at once, keys, rules switched mid-session.
