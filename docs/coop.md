# Co-op

Each player runs their own game and save; the co-op layer connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: the layer is a host thread of the runtime
(`runtime/coop.c`) with its own UDP socket.

What exists so far is the connection layer: rooms, the relay, and a channel for messages between the players. Syncing
progress, showing the other players in the world and the menu tab come on top of it and are not there yet; until they
are, the settings below are in `nx2navi.ini` only.

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

With `coop_auto` a room the relay lost (it restarted) is made again, under the same code if the relay allows chosen codes, or
waited for, for as long as the session runs. A player who leaves ends the session.

## In the log

State changes only: `coop: connecting to <host:port>`, `relay <ip:port> answered in N ms`, `room XXX-XXX created`, `joined
room XXX-XXX as member N`, `<name> (member N) joined the room` and `left the room`, `relay lost; rejoining`, `left room
(<why>)`. A cause that repeats (a datagram with a wrong tag, no server) logs once per 30 s; a counters line follows every
minute in a room. Peer addresses are never logged, the relay's is. `NX2NAVI_TRACE=coop` adds a line per datagram.

## The interface for the layers above

`runtime/coop.h`: `coop_send(kind, bytes, n)` and `coop_send_to(member, ...)` send a message to the others (it is a
datagram: it may be lost or arrive out of order; a datagram delivered twice is filtered), `coop_on_message(kind, fn, user)`
receives them on the coop thread, `coop_on_view(fn, user)` reports the state and the roster, `coop_view_get(&view)` copies
them, `coop_command(COOP_CMD_CREATE | COOP_CMD_JOIN | COOP_CMD_LEAVE, code)` drives the room. `coop_shutdown()` says LEAVE
and BYE when the process ends. The wire format is `runtime/coop_proto.h`; `tests/coop_vectors.txt` holds byte vectors
that both it and `tools/coop_relay.py` are checked against.

## Checks

- `python tests/coop_relay_test.py`: the relay (rooms, rejoin, timeouts, limits, bad tags) on a fake clock and over UDP,
  and the codec against the vectors. With `NX2NAVI_EXE=<game>/ocarina` it also runs the probe against a relay it starts,
  with and without 30 % loss, and restarts that relay in the middle.
- `ocarina --probe-coop`: the sockets, the codec, a fuzz of every parser, and two sessions in one process that make and
  join a room, exchange messages both ways, take garbage and forged datagrams, lose the relay and find each other again.
  Needs no window, GPU or game files. It runs its own small relay; `NX2NAVI_COOP_RELAY=host:port` uses a real one (started
  with `--allow-chosen-codes`), `NX2NAVI_COOP_DROP=30` drops that share of what the sessions receive,
  `NX2NAVI_COOP_VECTORS=tests/coop_vectors.txt` checks the byte vectors.
