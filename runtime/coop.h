// Co-op session layer: one host thread, "coop", owns a UDP socket to a relay (tools/coop_relay.py), keeps the player in a
// room of up to 8 and carries the messages the players send each other (coop_proto.h: the wire format). It never holds a
// virtual core and never touches game state: progress sync and the player ghosts are layers above this one, which send
// with coop_send and receive with coop_on_message. The game's own nn::socket stays offline.
//
// Threads. Everything below the "process" heading may be called from any thread; the callbacks run on the coop thread
// (keep them short, they hold up the socket) and must not call coop_command. coop_send copies the message into an outbox the
// coop thread flushes within about 8 ms; coop_view_get and coop_command take a lock the coop thread holds only to copy.
//
// The session is off until the COOP setting (nx2navi.ini [coop], the menu) is on; docs/coop.md has the settings.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "coop_proto.h"

typedef enum {
    COOP_OFF,           // the COOP setting is off: no socket, no thread work
    COOP_LEFT,          // on, in no room (the status says why after a failure)
    COOP_CONNECTING,    // reaching the relay to create or join a room
    COOP_JOINED,        // in a room
    COOP_RECONNECTING,  // the relay went silent; joining the same room again with the same client id
} CoopState;

typedef enum { COOP_CMD_CREATE = 1, COOP_CMD_JOIN, COOP_CMD_LEAVE } CoopCmd;

typedef struct {
    uint8_t id;                 // member id 1..8
    uint8_t away;               // the relay has not heard from the member for a while
    char name[COOP_NAME_MAX + 1];
    uint32_t heard_ms;          // milliseconds since the last message from this member through the relay; UINT32_MAX never
} CoopPeer;

typedef struct {
    CoopState state;
    unsigned version;           // changes with the state, the room, the roster and the status text
    char code[COOP_CODE_LEN + 2];   // "XXX-XXX" in a room, or while joining one; else ""
    int self;                   // this player's member id; 0 outside a room
    int n;                      // members of the room, this player included, by id
    CoopPeer member[COOP_MAX_MEMBERS];
    uint32_t ping_ms;           // round trip to the relay; 0 until measured
    char status[96];            // what is going on or what went wrong, in words
    uint32_t tx, rx, lost;      // datagrams sent, accepted, and received but not accepted (bad tag, wrong sender, duplicate)
} CoopView;

// ---- the process's session (the one the settings drive)

// Starts the coop thread. It re-reads the settings when cfg_version changes and does nothing while COOP is off.
void coop_init(void);
// Leaves the room and says BYE to the others, best effort, for the three ways the process ends (boot.c, svc.c,
// gfx_present.c, before fs_save_flush): waits at most about 0.4 s for the coop thread, then returns whatever happened.
void coop_shutdown(void);
// Create, join or leave a room. code (a code as typed: any case, '-' and spaces allowed) is required by JOIN and optional for
// CREATE (a relay that allows chosen codes then makes that room). 1 when queued, 0 when the code is not a room code or
// co-op is off. The outcome shows in coop_view_get.
int coop_command(CoopCmd cmd, const char *code);
void coop_view_get(CoopView *out);

// Peer messages. kind is COOP_K_*; bytes follow it in the datagram. coop_send goes to every other member, coop_send_to to
// one (a member id). 1 when queued, 0 when not in a room, the message is too long (COOP_MAX_PAYLOAD - 1 bytes) or the
// outbox is full: a message is a datagram and may be lost anyway.
int coop_send(uint8_t kind, const void *bytes, size_t n);
int coop_send_to(int member, uint8_t kind, const void *bytes, size_t n);
// fn(from member id, the message bytes after the kind, their count, user) for every message of that kind that passes its
// tag check and the duplicate filter. One handler per kind; register before the message can arrive (at start-up).
typedef void (*CoopMsgFn)(int from, const uint8_t *bytes, size_t n, void *user);
int coop_on_message(uint8_t kind, CoopMsgFn fn, void *user);
// fn(view, user) after the state, the room, the roster or the status changed; up to 8 hooks. A layer that sends a HELLO to
// every new member listens here.
typedef void (*CoopViewFn)(const CoopView *view, void *user);
int coop_on_view(CoopViewFn fn, void *user);

// "host", "host:port", "[v6]:port" or a bare IPv6 address into host (cap bytes) and port (default 24680). 0 when text is
// empty or does not fit.
int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port);

// ---- a session of its own (--probe-coop runs two in one process; the process's session is one of these driven by the settings)
typedef struct CoopSess CoopSess;
typedef struct {
    const char *tag;            // names the session in log lines ("coop[A]: ..."); NULL for none
    const char *server;         // "host[:port]"
    const char *name;           // the player's name
    uint64_t client_id;         // the same one on every reconnect keeps the member id for a minute
    int on;                     // 0: off (COOP_OFF), the COOP setting; 1: on, in no room until a command says
    uint16_t local_port;        // 0: any
    unsigned drop_pct;          // percent of received datagrams thrown away, for tests
    int retry;                  // a room the relay lost (a restart) is made or waited for again instead of ending the session
    // timings in ms, 0 for the defaults (2000, 8000, 250, 1000, 15000); tests shorten them
    uint32_t ping_ms, silence_ms, burst_gap_ms, backoff_first_ms, backoff_max_ms;
} CoopParams;
CoopSess *coop_sess_new(const CoopParams *p);   // p's strings are copied
void coop_sess_free(CoopSess *s);               // the stepping thread must be gone
// The stepping thread calls this in a loop: handles commands, the outbox, one batch of datagrams and the timers, waiting at
// most 8 ms (in a room), 50 ms (reaching the relay) or 250 ms (idle) for the next datagram.
void coop_sess_step(CoopSess *s);
int coop_sess_command(CoopSess *s, CoopCmd cmd, const char *code);   // as coop_command, for this session
int coop_sess_send(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n);   // member 0 or COOP_DST_ALL: all
int coop_sess_on_message(CoopSess *s, uint8_t kind, CoopMsgFn fn, void *user);
int coop_sess_on_view(CoopSess *s, CoopViewFn fn, void *user);
void coop_sess_view(CoopSess *s, CoopView *out);
// Replaces the settings the session reads with those of p (the timings stay as they were made); the stepping thread
// applies them, on or off included. A new server ends the room; a new name, once the room is joined, joins it again under the new name a
// second after the last change, so typing a name does not rejoin at every letter.
void coop_sess_configure(CoopSess *s, const CoopParams *p);
// Ends the session as coop_sess_configure with on = 0 would: LEAVE and BYE, socket closed, state COOP_OFF.
void coop_sess_stop(CoopSess *s);
// Waits at most ms for the stepping thread to have carried out a stop (or a configure with on = 0); 1 when it did.
int coop_sess_wait_off(CoopSess *s, uint32_t ms);
