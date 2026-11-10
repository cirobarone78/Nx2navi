// Co-op session layer (coop.h): the state machine of one player's connection to a relay, and the thread that runs it.
//
// States: OFF (the COOP setting is off), LEFT (on, in no room), CONNECTING (reaching the relay to create or join a room),
// JOINED, RECONNECTING (the relay went silent; joining the same room again as the same client).
//
// Reaching the relay is a cookie exchange (tools/coop_relay.py): a CONNECT padded to 64 bytes is answered by a CHALLENGE
// with a cookie, a CONNECT that carries the cookie is answered by JOINED or an ERROR. A burst of four CONNECTs 250 ms apart
// carries the exchange over a lossy path; a burst without an answer is followed by a wait of 1, 2, 4, 8, then 15 s. In a room
// a PING every 2 s keeps the NAT mapping open and measures the round trip, and 8 s without any datagram from the relay is
// RECONNECTING. The relay forwards the players' messages (DATA datagrams) unread; each carries the sender's tag, which the
// receivers check with the room key, and a counter the receivers use to drop duplicates.
//
// One thread owns the socket, the DNS lookup and the state. Other threads reach it through a mailbox (commands, settings), an
// outbox (messages to send) and a copy of the state they may read; all three sit behind one lock that the thread holds only
// to copy. The thread never holds a virtual core (kernel.c: host work runs without one), as discord_rpc.c's.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_app.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

enum {
    PING_MS = 2000, SILENCE_MS = 8000, BURST_GAP_MS = 250, BACKOFF_FIRST_MS = 1000, BACKOFF_MAX_MS = 15000,
    BURST_TRIES = 4,                 // CONNECTs per burst
    STATS_MS = 60000,                // the periodic counters line, while in a room
    LOG_EVERY_MS = 30000,            // a repeated cause logs this often
    RENAME_MS = 1000,                // a name change joins the room again after this much quiet
    OUT_SLOTS = 64,                  // messages waiting to be sent
    DEFAULT_PORT = 24680,
    VIEW_HOOKS = 8,
};

typedef struct { uint8_t dst; uint16_t len; uint8_t data[COOP_MAX_PAYLOAD]; } OutMsg;
typedef struct { CoopMsgFn fn; void *user; } MsgHandler;
typedef struct { CoopViewFn fn; void *user; } ViewHook;

// A cause that logs at most once per LOG_EVERY_MS.
enum { L_MALFORMED, L_SENDER, L_TAG, L_OUTBOX, L_NO_SERVER, L_RESOLVE, L_SOCKET, L_CAUSES };

// A window of the last 64 counters of one sender, to drop a datagram the network delivered twice. A counter far outside the
// window (a sender that restarted) starts a new window; a replayed old datagram is harmless, every message is idempotent.
typedef struct { uint32_t high; uint64_t bits; int valid; } Window;

struct CoopSess {
    // ---- settings, set by coop_sess_new and coop_sess_configure
    char tag[16];
    uint32_t ping_ms, silence_ms, burst_gap_ms, backoff_first_ms, backoff_max_ms;
    // what the stepping thread works with now
    char server[160], name[COOP_NAME_MAX + 1], host[128];
    uint16_t port, local_port;
    uint64_t client_id;
    unsigned drop_pct;
    int retry;

    // ---- shared, behind lock
    PlatMutex lock;
    PlatEvent off_done;
    int cmd;                                    // a command waiting (COOP_CMD_*), 0 none; a later one replaces it
    char cmd_code[COOP_CODE_LEN + 1];
    int cfg_pending;                            // pending holds settings the thread has not applied
    struct { char server[160], name[COOP_NAME_MAX + 1]; uint64_t client_id; uint16_t local_port; unsigned drop_pct; int retry, on; } pending;
    OutMsg *out;
    unsigned out_head, out_tail;
    CoopView view;
    uint64_t heard_at[COOP_MAX_MEMBERS + 1];    // last message of each member, by id; 0 never
    MsgHandler msg[256];
    ViewHook hooks[VIEW_HOOKS];
    int n_hooks;
    int pub_state;                              // state, readable without the lock
    int pub_joined;

    // ---- the stepping thread's
    CoopState state;
    PlatUdp *sock;
    PlatAddr relay;
    int relay_known;
    char relay_text[64];                        // the relay's resolved address, for the log
    int intent, made_room, have_key;            // CONNECT's intent; this player created the room; key is valid
    char want[COOP_CODE_LEN + 1];               // the room being joined or made ("" a create without a chosen code)
    uint8_t key[COOP_KEY_LEN];                  // of want
    uint8_t cookie[COOP_COOKIE_LEN];
    int have_cookie, said_answer, heard_relay;
    uint32_t connect_seq, out_seq;
    uint64_t next_tx, last_connect_tx, last_rx, next_ping, next_stats, rename_at;
    uint32_t backoff;
    int burst_left;
    uint32_t room;
    int self;
    char code[COOP_CODE_LEN + 1];
    uint8_t room_key[COOP_KEY_LEN];
    CoopRoster roster;
    uint32_t roster_seq;
    Window rw[COOP_MAX_MEMBERS + 1];
    char status[96];
    int dirty;
    uint64_t rng;
    uint64_t log_at[L_CAUSES];
    uint32_t n_tx, n_rx, n_lost, n_malformed, n_bad_tag, n_sender, n_dup, n_test_drop, n_outbox_full;
    uint32_t stats_seen;
    uint8_t rx[2048];
};

// ---- logging

#define TRACE(s, ...) do { if (rt_trace & RT_TRACE_COOP) say(s, __VA_ARGS__); } while (0)
static void say(const CoopSess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(const CoopSess *s, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (s->tag[0]) rt_log("coop[%s]: %s", s->tag, msg);
    else rt_log("coop: %s", msg);
}

// 1 when cause may log now: the first time, then once per LOG_EVERY_MS (the periodic counters line has the totals).
static int log_ready(CoopSess *s, int cause, uint64_t now) {
    if (s->log_at[cause] && now - s->log_at[cause] < LOG_EVERY_MS) return 0;
    s->log_at[cause] = now ? now : 1;
    return 1;
}
#define SAY_LIMITED(s, cause, now, ...) do { if (log_ready(s, cause, now)) say(s, __VA_ARGS__); } while (0)

static const char *type_name(unsigned t) {
    switch (t) {
    case COOP_T_CONNECT: return "CONNECT";
    case COOP_T_CHALLENGE: return "CHALLENGE";
    case COOP_T_JOINED: return "JOINED";
    case COOP_T_ERROR: return "ERROR";
    case COOP_T_ROSTER: return "ROSTER";
    case COOP_T_PING: return "PING";
    case COOP_T_PONG: return "PONG";
    case COOP_T_LEAVE: return "LEAVE";
    case COOP_T_DATA: return "DATA";
    default: return "?";
    }
}

// ---- small things

static uint32_t rnd(CoopSess *s) {   // xorshift64*: for the test drop and nothing else
    s->rng ^= s->rng >> 12, s->rng ^= s->rng << 25, s->rng ^= s->rng >> 27;
    return (uint32_t)((s->rng * 0x2545F4914F6CDD1Dull) >> 32);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

static void set_status(CoopSess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_status(CoopSess *s, const char *fmt, ...) {
    char text[sizeof s->status];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (strcmp(text, s->status)) memcpy(s->status, text, sizeof text), s->dirty = 1;
}

static void set_state(CoopSess *s, CoopState st) {
    if (s->state != st) s->dirty = 1;
    s->state = st;
    __atomic_store_n(&s->pub_state, (int)st, __ATOMIC_RELEASE);
    __atomic_store_n(&s->pub_joined, st == COOP_JOINED, __ATOMIC_RELEASE);
}

int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port) {
    while (*text == ' ') text++;
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ') n--;
    if (!n) return 0;
    const char *h = text, *colon = NULL;
    size_t hn = n;
    if (*text == '[') {   // [v6] or [v6]:port
        const char *close = memchr(text, ']', n);
        if (!close) return 0;
        h = text + 1, hn = (size_t)(close - h);
        if (close + 1 < text + n) {
            if (close[1] != ':') return 0;
            colon = close + 1;
        }
    } else {
        size_t colons = 0;
        for (size_t i = 0; i < n; i++)
            if (text[i] == ':') colons++, colon = text + i;
        if (colons > 1) colon = NULL;   // a bare IPv6 address
        else if (colon) hn = (size_t)(colon - text);
    }
    uint32_t p = DEFAULT_PORT;
    if (colon) {
        const char *d = colon + 1;
        size_t dn = (size_t)(text + n - d);
        if (!dn || dn > 5) return 0;
        p = 0;
        for (size_t i = 0; i < dn; i++) {
            if (d[i] < '0' || d[i] > '9') return 0;
            p = p * 10 + (uint32_t)(d[i] - '0');
        }
        if (!p || p > 65535) return 0;
    }
    if (!hn || hn >= cap) return 0;
    memcpy(host, h, hn);
    host[hn] = 0;
    *port = (uint16_t)p;
    return 1;
}

// ---- sending

// One datagram to the relay. key NULL gives a zero tag.
static void send_dgram(CoopSess *s, uint8_t type, uint32_t room, uint8_t src, uint8_t dst, uint32_t seq,
                       const void *payload, size_t n, const uint8_t *key) {
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .src = src, .dst = dst, .room = room, .seq = seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, key);
    if (!k || !s->sock || !s->relay_known) return;
    plat_udp_send(s->sock, &s->relay, d, k);
    s->n_tx++;
    if (rt_trace & RT_TRACE_COOP) {
        if (type == COOP_T_DATA && n) say(s, "tx DATA kind %u to %u, %zu bytes", ((const uint8_t *)payload)[0], dst, k);
        else say(s, "tx %s, %zu bytes", type_name(type), k);
    }
}

// A datagram of the room we are in.
static void send_room(CoopSess *s, uint8_t type, uint8_t dst, const void *payload, size_t n) {
    send_dgram(s, type, s->room, (uint8_t)s->self, dst, s->out_seq++, payload, n, s->room_key);
}

static void send_connect(CoopSess *s, uint64_t now) {
    CoopConnect c = {.intent = (uint8_t)s->intent, .client_id = s->client_id};
    uint8_t payload[96];
    copy_str(c.code, sizeof c.code, s->want);
    copy_str(c.name, sizeof c.name, s->name);
    if (s->have_cookie) memcpy(c.cookie, s->cookie, COOP_COOKIE_LEN);
    size_t n = coop_connect_pack(payload, sizeof payload, &c);
    // the first CONNECT has no cookie and a zero tag; the one that carries the cookie is tagged when there is a code
    send_dgram(s, COOP_T_CONNECT, 0, 0, 0, s->connect_seq, payload, n, s->have_cookie && s->have_key ? s->key : NULL);
    if (!s->have_cookie) s->last_connect_tx = now;
}

// ---- leaving

static void close_socket(CoopSess *s) {
    if (s->sock) plat_udp_close(s->sock);
    s->sock = NULL;
}

// Tells the others and the relay, twice (a datagram may be lost): BYE to every member, then LEAVE.
static void say_goodbye(CoopSess *s) {
    if (!s->sock || !s->relay_known || !s->room) return;
    uint8_t bye[1];
    size_t n = coop_peer_pack(bye, sizeof bye, COOP_K_BYE, NULL, 0);
    for (int i = 0; i < 2; i++) {
        if (s->state == COOP_JOINED) send_room(s, COOP_T_DATA, COOP_DST_ALL, bye, n);
        send_room(s, COOP_T_LEAVE, 0, NULL, 0);
    }
}

// Out of the room (or never in): closes the socket and shows why.
static void go_left(CoopSess *s, const char *why, int log_it) {
    int was_in = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    if (log_it) {
        if (was_in) say(s, "left room (%s)", why);
        else say(s, "no room: %s", why);
    }
    close_socket(s);
    s->room = 0, s->self = 0;
    s->roster.n = 0;
    memset(s->rw, 0, sizeof s->rw);
    set_state(s, COOP_LEFT);
    set_status(s, "%s", why);
    s->dirty = 1;
}

// ---- reaching the relay

// A socket and the relay's address, when there are none yet. 0 and a status when they cannot be had.
static int ensure_socket(CoopSess *s, uint64_t now) {
    if (!s->sock) {
        s->sock = plat_udp_open(s->local_port, 1);
        if (!s->sock) {
            set_status(s, "cannot open a UDP socket");
            SAY_LIMITED(s, L_SOCKET, now, "cannot open a UDP socket");
            return 0;
        }
    }
    if (!s->relay_known) {
        say(s, "connecting to %s", s->server);
        if (!plat_net_resolve(s->host, s->port, &s->relay)) {
            set_status(s, "cannot find the server %s", s->host);
            SAY_LIMITED(s, L_RESOLVE, now, "cannot resolve %s", s->host);
            return 0;
        }
        plat_addr_str(&s->relay, s->relay_text, sizeof s->relay_text);
        s->relay_known = 1;
    }
    return 1;
}

static void schedule_backoff(CoopSess *s, uint64_t now) {
    s->next_tx = now + s->backoff;
    if (!s->heard_relay && s->sock && s->relay_known) set_status(s, "no answer from the relay; trying again in %u s", (s->backoff + 500) / 1000);
    s->backoff = s->backoff * 2 > s->backoff_max_ms ? s->backoff_max_ms : s->backoff * 2;
}

// A new attempt to get into a room: intent, the room's code ("" none), and whether it is a rejoin of the room we were in.
static void begin_episode(CoopSess *s, int intent, const char *code, int rejoin, uint64_t now) {
    s->intent = intent;
    if (code != s->want) copy_str(s->want, sizeof s->want, code);   // a rejoin passes the code it already holds
    s->have_key = s->want[0] != 0;
    if (s->have_key) coop_room_key(s->want, s->key);
    s->connect_seq = s->out_seq++;
    s->have_cookie = s->said_answer = s->heard_relay = 0;
    s->burst_left = 0;
    s->next_tx = now;
    s->backoff = s->backoff_first_ms;
    s->relay_known = 0;   // looked up again: the name may point elsewhere by now
    if (!rejoin) {
        s->room = 0, s->self = 0;
        s->roster.n = 0;
    }
    set_state(s, rejoin ? COOP_RECONNECTING : COOP_CONNECTING);
    if (rejoin) set_status(s, "reconnecting to the relay");
    else set_status(s, "%s", intent == COOP_INTENT_CREATE ? "creating a room" : "joining the room");
    s->dirty = 1;
}

static void connect_timers(CoopSess *s, uint64_t now) {
    if (now < s->next_tx) return;
    if (s->burst_left == 0) {   // a new burst: the socket and the relay's address first
        if (!ensure_socket(s, now)) {
            s->relay_known = 0;
            schedule_backoff(s, now);
            return;
        }
        s->burst_left = BURST_TRIES;
    }
    send_connect(s, now);
    if (--s->burst_left) s->next_tx = now + s->burst_gap_ms;
    else schedule_backoff(s, now);
}

// ---- the roster

static void format_code(const char *code, char out[COOP_CODE_LEN + 2]) {
    if (code[0]) coop_code_format(code, out);
    else out[0] = 0;
}

// 1 when every id of r is 1..8 and none repeats: what the rest of the code indexes by.
static int roster_valid(const CoopRoster *r) {
    unsigned seen = 0;
    for (int i = 0; i < r->n; i++) {
        unsigned id = r->m[i].id;
        if (id < 1 || id > COOP_MAX_MEMBERS || (seen & 1u << id)) return 0;
        seen |= 1u << id;
    }
    return 1;
}

// The roster the relay sent replaces ours; the log tells who came, went, changed name or went away.
static void apply_roster(CoopSess *s, const CoopRoster *r) {
    for (int i = 0; i < r->n; i++) {
        const CoopMemberInfo *m = &r->m[i], *old = NULL;
        for (int j = 0; j < s->roster.n; j++)
            if (s->roster.m[j].id == m->id) old = &s->roster.m[j];
        if (m->id == s->self) continue;
        if (!old) say(s, "%s (member %u) joined the room", m->name, m->id);
        else if (strcmp(old->name, m->name)) say(s, "member %u is now %s", m->id, m->name);
        else if ((old->flags ^ m->flags) & COOP_FLAG_AWAY)
            say(s, "%s (member %u) %s", m->name, m->id, m->flags & COOP_FLAG_AWAY ? "is away" : "is back");
        if (!old) s->rw[m->id] = (Window){0};   // a new member starts a new counter
    }
    for (int j = 0; j < s->roster.n; j++) {
        int id = s->roster.m[j].id, found = 0;
        for (int i = 0; i < r->n; i++) found |= r->m[i].id == id;
        if (found) continue;
        say(s, "%s (member %u) left the room", s->roster.m[j].name, id);
        s->rw[id] = (Window){0};
        plat_mutex_lock(&s->lock);
        s->heard_at[id] = 0;
        plat_mutex_unlock(&s->lock);
    }
    s->roster = *r;
    s->dirty = 1;
}

// ---- receiving

// 1 when seq is new for this sender's window; 0 when it is a duplicate.
static int window_accept(Window *w, uint32_t seq) {
    if (!w->valid) {
        *w = (Window){.high = seq, .bits = 1, .valid = 1};
        return 1;
    }
    int32_t d = (int32_t)(seq - w->high);
    if (d > 0) {
        w->bits = d >= 64 ? 0 : w->bits << d;
        w->bits |= 1;
        w->high = seq;
        return 1;
    }
    if (d <= -64) {   // far behind: the sender restarted
        *w = (Window){.high = seq, .bits = 1, .valid = 1};
        return 1;
    }
    uint64_t bit = 1ull << (unsigned)-d;
    if (w->bits & bit) return 0;
    w->bits |= bit;
    return 1;
}

// The on_* handlers return 1 for a datagram that passed its tag check and made sense: only those show the relay is there.
static int on_challenge(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if ((s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) || h->len != COOP_COOKIE_LEN) return 0;
    if (s->have_key ? !coop_dgram_verify(d, n, s->key) : !coop_dgram_tag_zero(d)) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    memcpy(s->cookie, p, COOP_COOKIE_LEN);
    s->have_cookie = 1;
    s->heard_relay = 1;
    if (!s->said_answer) {
        s->said_answer = 1;
        say(s, "relay %s answered in %u ms", s->relay_text, (unsigned)(now - s->last_connect_tx));
    }
    send_connect(s, now);   // the CONNECT with the cookie, at once
    return 1;
}

static int on_joined(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if (s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) return 0;
    CoopJoined j;
    uint8_t key[COOP_KEY_LEN];
    if (!coop_joined_parse(p, h->len, &j) || !h->room || j.member < 1 || j.member > COOP_MAX_MEMBERS || !roster_valid(&j.roster)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    coop_room_key(j.code, key);
    // a join is for the room asked for; a create takes the code the relay made (or the one it allowed us to choose)
    if (!coop_dgram_verify(d, n, key) || (s->intent == COOP_INTENT_JOIN && memcmp(j.code, s->want, COOP_CODE_LEN))) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    int rejoined = s->state == COOP_RECONNECTING;
    s->room = h->room;
    s->self = j.member;
    memcpy(s->code, j.code, sizeof s->code);
    memcpy(s->room_key, key, COOP_KEY_LEN);
    copy_str(s->want, sizeof s->want, j.code);   // a later rejoin asks for this room
    memcpy(s->key, key, COOP_KEY_LEN);
    s->have_key = 1;
    s->roster = j.roster;
    s->roster_seq = h->seq;
    memset(s->rw, 0, sizeof s->rw);
    s->last_rx = now;
    s->next_ping = now + s->ping_ms;
    s->next_stats = now + STATS_MS;
    plat_mutex_lock(&s->lock);
    memset(s->heard_at, 0, sizeof s->heard_at);
    plat_mutex_unlock(&s->lock);
    char shown[COOP_CODE_LEN + 2];
    coop_code_format(s->code, shown);
    if (rejoined) say(s, "rejoined room %s as member %u", shown, s->self);
    else if (s->intent == COOP_INTENT_CREATE) say(s, "room %s created (member %u)", shown, s->self);
    else say(s, "joined room %s as member %u", shown, s->self);
    if (!rejoined) s->made_room = s->intent == COOP_INTENT_CREATE;
    for (int i = 0; i < s->roster.n; i++)
        if (s->roster.m[i].id != s->self) say(s, "%s (member %u) is in the room", s->roster.m[i].name, s->roster.m[i].id);
    set_state(s, COOP_JOINED);
    set_status(s, "%s", "");
    s->dirty = 1;
    return 1;
}

static int on_error(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if (s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) return 0;
    CoopError e;
    if (!coop_error_parse(p, h->len, &e)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    // tagged with the room key when the CONNECT had a code; the relay cannot tag what it answers to a create without one,
    // nor a protocol mismatch
    int ok = (s->have_key && coop_dgram_verify(d, n, s->key)) || (coop_dgram_tag_zero(d) && (!s->have_key || e.reason == COOP_E_PROTO));
    if (!ok) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    char shown[COOP_CODE_LEN + 2];
    format_code(s->want, shown);
    switch (e.reason) {
    case COOP_E_NO_ROOM:
        if (s->state == COOP_RECONNECTING) {   // the relay lost the room: a restart
            say(s, "room %s is gone from the relay", shown);
            if (!s->retry) go_left(s, "the room is closed", 0);
            else if (s->made_room) {
                say(s, "creating room %s again", shown);
                begin_episode(s, COOP_INTENT_CREATE, s->want, 0, now);
                set_status(s, "the room was lost; creating it again");
            } else {
                begin_episode(s, COOP_INTENT_JOIN, s->want, 0, now);
                set_status(s, "the room was lost; waiting for it");
            }
        } else if (s->retry) {   // not made yet, perhaps: wait and ask again
            s->heard_relay = 1;
            set_status(s, "room %s not found; waiting for it", shown);
            if (s->burst_left || now >= s->next_tx) {   // the first answer of a burst ends it; a CONNECT sent twice is answered twice
                s->burst_left = 0;
                schedule_backoff(s, now);
            }
        } else {
            go_left(s, "no such room", 1);
        }
        break;
    case COOP_E_ROOM_FULL: go_left(s, "the room is full", 1); break;
    case COOP_E_PROTO: go_left(s, "update required: the relay speaks another protocol version", 1); break;
    case COOP_E_RATE: go_left(s, "too many joins from this address; try again in a minute", 1); break;
    case COOP_E_BAD_TAG: go_left(s, "the relay did not accept the room code", 1); break;
    case COOP_E_SERVER_FULL: go_left(s, "the relay is full", 1); break;
    default: go_left(s, "the relay refused", 1); break;
    }
    return 1;
}

static int on_roster(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p) {
    CoopRoster r;
    if (s->state != COOP_JOINED || h->room != s->room) return 0;
    if (!coop_dgram_verify(d, n, s->room_key)) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    if (!coop_roster_parse(p, h->len, &r, NULL) || !roster_valid(&r)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    if ((int32_t)(h->seq - s->roster_seq) > 0) {   // an older roster arriving late is dropped
        s->roster_seq = h->seq;
        apply_roster(s, &r);
    }
    return 1;
}

static int on_pong(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint32_t token;
    if (s->state != COOP_JOINED || h->room != s->room || !coop_u32_parse(p, h->len, &token)) return 0;
    if (!coop_dgram_verify(d, n, s->room_key)) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    uint32_t rtt = (uint32_t)now - token;
    if (rtt <= 60000) {   // a token of ours
        plat_mutex_lock(&s->lock);
        s->view.ping_ms = rtt ? rtt : 1;
        plat_mutex_unlock(&s->lock);
    }
    return 1;
}

static int on_data(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if (s->state != COOP_JOINED || h->room != s->room) return 0;
    if (h->src < 1 || h->src > COOP_MAX_MEMBERS || h->src == s->self || (h->dst != COOP_DST_ALL && h->dst != s->self)) {
        s->n_sender++, s->n_lost++;
        SAY_LIMITED(s, L_SENDER, now, "a DATA datagram from member %u to %u was thrown away", h->src, h->dst);
        return 0;
    }
    if (!coop_dgram_verify(d, n, s->room_key)) {
        s->n_bad_tag++, s->n_lost++;
        SAY_LIMITED(s, L_TAG, now, "a DATA datagram with a wrong tag was thrown away");
        return 0;
    }
    uint8_t kind;
    const uint8_t *bytes;
    size_t nb;
    if (!coop_peer_parse(p, h->len, &kind, &bytes, &nb)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    if (!window_accept(&s->rw[h->src], h->seq)) {
        s->n_dup++, s->n_lost++;
        return 0;
    }
    plat_mutex_lock(&s->lock);
    s->heard_at[h->src] = now;
    plat_mutex_unlock(&s->lock);
    TRACE(s, "rx DATA kind %u from member %u, %zu bytes", kind, h->src, nb);
    CoopMsgFn fn = __atomic_load_n(&s->msg[kind].fn, __ATOMIC_ACQUIRE);
    if (fn) fn(h->src, bytes, nb, s->msg[kind].user);
    return 1;
}

static void handle_datagram(CoopSess *s, size_t n, const PlatAddr *from, uint64_t now) {
    if (s->drop_pct && rnd(s) % 100 < s->drop_pct) {
        s->n_test_drop++;
        return;
    }
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(s->rx, n, &h, &p)) {
        s->n_malformed++, s->n_lost++;
        SAY_LIMITED(s, L_MALFORMED, now, "a datagram that is not a co-op datagram was thrown away");
        return;
    }
    if (!s->relay_known || !plat_addr_eq(from, &s->relay)) {   // only the relay talks to us
        s->n_sender++, s->n_lost++;
        SAY_LIMITED(s, L_SENDER, now, "a datagram from an address that is not the relay was thrown away");
        return;
    }
    if (h.ver != COOP_PROTO && h.type != COOP_T_ERROR) return;
    TRACE(s, "rx %s, %zu bytes", type_name(h.type), n);
    int ok = 0;
    switch (h.type) {
    case COOP_T_CHALLENGE: ok = on_challenge(s, s->rx, n, &h, p, now); break;
    case COOP_T_JOINED: ok = on_joined(s, s->rx, n, &h, p, now); break;
    case COOP_T_ERROR: ok = on_error(s, s->rx, n, &h, p, now); break;
    case COOP_T_ROSTER: ok = on_roster(s, s->rx, n, &h, p); break;
    case COOP_T_PONG: ok = on_pong(s, s->rx, n, &h, p, now); break;
    case COOP_T_DATA: ok = on_data(s, s->rx, n, &h, p, now); break;
    default: break;
    }
    if (ok) {
        s->n_rx++;
        s->last_rx = now;   // a datagram the relay made: it is there
    }
}

// ---- the outbox

static void flush_outbox(CoopSess *s) {
    for (int i = 0; i < OUT_SLOTS; i++) {
        OutMsg m;
        plat_mutex_lock(&s->lock);
        if (s->out_head == s->out_tail) {
            plat_mutex_unlock(&s->lock);
            return;
        }
        OutMsg *slot = &s->out[s->out_tail];
        m.dst = slot->dst, m.len = slot->len;
        memcpy(m.data, slot->data, slot->len);
        s->out_tail = (s->out_tail + 1) % OUT_SLOTS;
        plat_mutex_unlock(&s->lock);
        if (s->state == COOP_JOINED) send_room(s, COOP_T_DATA, m.dst, m.data, m.len);   // anywhere else it is dropped
    }
}

int coop_sess_send(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n) {
    if (!__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE) || n + 1 > COOP_MAX_PAYLOAD || member < 0 ||
        (member > COOP_MAX_MEMBERS && member != COOP_DST_ALL))
        return 0;
    plat_mutex_lock(&s->lock);
    unsigned next = (s->out_head + 1) % OUT_SLOTS;
    if (next == s->out_tail) {
        plat_mutex_unlock(&s->lock);
        __atomic_add_fetch(&s->n_outbox_full, 1, __ATOMIC_RELAXED);
        return 0;
    }
    OutMsg *slot = &s->out[s->out_head];
    slot->dst = member ? (uint8_t)member : COOP_DST_ALL;
    slot->len = (uint16_t)coop_peer_pack(slot->data, sizeof slot->data, kind, bytes, n);
    s->out_head = next;
    plat_mutex_unlock(&s->lock);
    return 1;
}

// ---- commands and settings

// The settings coop_sess_configure queued, on the stepping thread.
static void apply_settings(CoopSess *s, uint64_t now) {
    plat_mutex_lock(&s->lock);
    if (!s->cfg_pending) {
        plat_mutex_unlock(&s->lock);
        return;
    }
    s->cfg_pending = 0;
    char server[sizeof s->server], name[sizeof s->name];
    copy_str(server, sizeof server, s->pending.server);
    copy_str(name, sizeof name, s->pending.name);
    s->client_id = s->pending.client_id;
    s->local_port = s->pending.local_port;
    s->drop_pct = s->pending.drop_pct;
    s->retry = s->pending.retry;
    int on = s->pending.on;
    plat_mutex_unlock(&s->lock);
    if (strcmp(server, s->server)) {
        copy_str(s->server, sizeof s->server, server);
        s->relay_known = 0;
        s->host[0] = 0;
        if (s->server[0] && !coop_server_parse(s->server, s->host, sizeof s->host, &s->port))
            say(s, "the server \"%s\" is not host or host:port", s->server);
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            go_left(s, "the server changed", 1);
        } else if (s->state == COOP_CONNECTING) {
            go_left(s, "the server changed", 0);
        }
    }
    if (strcmp(name, s->name)) {
        copy_str(s->name, sizeof s->name, name);
        if (s->state == COOP_JOINED) s->rename_at = now + RENAME_MS;   // joins again once the typing has stopped
    }
    if (on && s->state == COOP_OFF) {
        set_state(s, COOP_LEFT);
        set_status(s, "%s", "");
    } else if (!on) {
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            say(s, "left room (co-op turned off)");
        }
        close_socket(s);
        s->room = 0, s->self = 0, s->roster.n = 0;
        s->rename_at = 0;
        set_state(s, COOP_OFF);
        set_status(s, "%s", "");
        s->dirty = 1;
        plat_event_set(&s->off_done);
    }
}

static void do_command(CoopSess *s, int cmd, const char *code, uint64_t now) {
    if (s->state == COOP_OFF) return;
    switch (cmd) {
    case COOP_CMD_LEAVE:
        if (s->state == COOP_LEFT) break;
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            say(s, "left room (leave requested)");
        } else {
            say(s, "stopped trying to get into a room");
        }
        go_left(s, "you left the room", 0);
        break;
    case COOP_CMD_CREATE:
    case COOP_CMD_JOIN:
        if (!s->host[0]) {
            SAY_LIMITED(s, L_NO_SERVER, now, "no server configured (coop_server / coop_app.h)");
            go_left(s, "no server configured", 0);
            break;
        }
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) say_goodbye(s);
        begin_episode(s, cmd == COOP_CMD_CREATE ? COOP_INTENT_CREATE : COOP_INTENT_JOIN, code, 0, now);
        break;
    }
}

// ---- the step

static void lost_relay(CoopSess *s, uint64_t now) {
    say(s, "relay lost; rejoining");
    begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
}

static void timers(CoopSess *s, uint64_t now) {
    switch (s->state) {
    case COOP_CONNECTING:
    case COOP_RECONNECTING:
        connect_timers(s, now);
        break;
    case COOP_JOINED:
        if (now - s->last_rx >= s->silence_ms) {
            lost_relay(s, now);
            break;
        }
        if (s->rename_at && now >= s->rename_at) {   // the name changed: the relay learns it with a new CONNECT
            s->rename_at = 0;
            say(s, "joining the room again as %s", s->name);
            begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
            break;
        }
        if (now >= s->next_ping) {
            uint8_t token[4];
            coop_u32_pack(token, sizeof token, (uint32_t)now);
            send_room(s, COOP_T_PING, 0, token, sizeof token);
            s->next_ping = now + s->ping_ms;
        }
        if (now >= s->next_stats) {
            s->next_stats = now + STATS_MS;
            uint32_t seen = s->n_rx + s->n_tx + s->n_lost + s->n_test_drop;
            if (seen != s->stats_seen) {
                s->stats_seen = seen;
                say(s, "stats: sent %u, accepted %u, thrown away %u (tag %u, sender %u, not co-op %u, duplicate %u), dropped by the test %u, outbox full %u",
                    s->n_tx, s->n_rx, s->n_lost, s->n_bad_tag, s->n_sender, s->n_malformed, s->n_dup, s->n_test_drop,
                    __atomic_load_n(&s->n_outbox_full, __ATOMIC_RELAXED));
            }
        }
        break;
    default:
        break;
    }
}

static void view_rebuild(CoopSess *s) {
    plat_mutex_lock(&s->lock);
    CoopView *v = &s->view;
    int in_room = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    v->state = s->state;
    if (in_room) format_code(s->code, v->code);
    else if (s->state == COOP_CONNECTING) format_code(s->want, v->code);
    else v->code[0] = 0;
    v->self = in_room ? s->self : 0;
    v->n = in_room ? s->roster.n : 0;
    for (int i = 0; i < v->n; i++) {
        v->member[i].id = s->roster.m[i].id;
        v->member[i].away = s->roster.m[i].flags & COOP_FLAG_AWAY ? 1 : 0;
        copy_str(v->member[i].name, sizeof v->member[i].name, s->roster.m[i].name);
    }
    copy_str(v->status, sizeof v->status, s->status);
    if (s->state != COOP_JOINED) v->ping_ms = 0;
    v->version++;
    plat_mutex_unlock(&s->lock);
}

static void publish(CoopSess *s) {
    if (!s->dirty) return;
    s->dirty = 0;
    view_rebuild(s);
    CoopView copy;
    ViewHook hooks[VIEW_HOOKS];
    plat_mutex_lock(&s->lock);
    copy = s->view;
    int n = s->n_hooks;
    memcpy(hooks, s->hooks, sizeof hooks);
    plat_mutex_unlock(&s->lock);
    for (int i = 0; i < n; i++) hooks[i].fn(&copy, hooks[i].user);
}

void coop_sess_step(CoopSess *s) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&s->lock);
    int cmd = s->cmd;
    char code[sizeof s->cmd_code];
    memcpy(code, s->cmd_code, sizeof code);
    s->cmd = 0;
    plat_mutex_unlock(&s->lock);
    apply_settings(s, now);
    if (cmd) do_command(s, cmd, code, now);
    flush_outbox(s);
    publish(s);   // what a command or a setting changed shows before this thread waits for a datagram
    uint32_t wait = s->state == COOP_JOINED ? 8 : s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING ? 50 : 250;
    if (s->sock) {
        PlatAddr from;
        int n = plat_udp_recv(s->sock, s->rx, sizeof s->rx, &from, wait);
        for (int i = 0; n > 0 && i < 64; i++) {
            handle_datagram(s, (size_t)n, &from, plat_now_ms());
            if (!s->sock) break;   // a handler ended it
            n = plat_udp_recv(s->sock, s->rx, sizeof s->rx, &from, 0);
        }
        if (n < 0 && s->sock) {
            SAY_LIMITED(s, L_SOCKET, now, "the socket failed");
            close_socket(s);
            if (s->state == COOP_JOINED) lost_relay(s, plat_now_ms());
        }
    } else {
        plat_sleep_ms(wait);
    }
    timers(s, plat_now_ms());
    publish(s);
}

// ---- the session's interface

CoopSess *coop_sess_new(const CoopParams *p) {
    CoopSess *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->out = calloc(OUT_SLOTS, sizeof *s->out);
    if (!s->out) {
        free(s);
        return NULL;
    }
    copy_str(s->tag, sizeof s->tag, p->tag);
    s->ping_ms = p->ping_ms ? p->ping_ms : PING_MS;
    s->silence_ms = p->silence_ms ? p->silence_ms : SILENCE_MS;
    s->burst_gap_ms = p->burst_gap_ms ? p->burst_gap_ms : BURST_GAP_MS;
    s->backoff_first_ms = p->backoff_first_ms ? p->backoff_first_ms : BACKOFF_FIRST_MS;
    s->backoff_max_ms = p->backoff_max_ms ? p->backoff_max_ms : BACKOFF_MAX_MS;
    plat_mutex_init(&s->lock);
    plat_event_init(&s->off_done, 1, 0);
    uint64_t seed = 0;
    if (!plat_random(&seed, sizeof seed) || !seed) seed = plat_now_ns() | 1;
    s->rng = seed;
    s->state = COOP_OFF;
    s->view.state = COOP_OFF;
    coop_sess_configure(s, p);
    apply_settings(s, plat_now_ms());   // nothing else runs yet: the first step need not wait for it
    publish(s);
    return s;
}

void coop_sess_free(CoopSess *s) {
    if (!s) return;
    close_socket(s);
    free(s->out);
    free(s);
}

void coop_sess_configure(CoopSess *s, const CoopParams *p) {
    plat_mutex_lock(&s->lock);
    copy_str(s->pending.server, sizeof s->pending.server, p->server);
    char clean[COOP_NAME_MAX + 1];
    coop_name_clean(clean, sizeof clean, (const uint8_t *)(p->name ? p->name : ""), p->name ? strlen(p->name) : 0);
    copy_str(s->pending.name, sizeof s->pending.name, clean);
    s->pending.client_id = p->client_id;
    s->pending.local_port = p->local_port;
    s->pending.drop_pct = p->drop_pct > 100 ? 100 : p->drop_pct;
    s->pending.retry = p->retry;
    s->pending.on = p->on;
    if (!p->on) plat_event_reset(&s->off_done);
    s->cfg_pending = 1;
    plat_mutex_unlock(&s->lock);
}

void coop_sess_stop(CoopSess *s) {
    plat_mutex_lock(&s->lock);
    s->pending.on = 0;
    plat_event_reset(&s->off_done);
    s->cfg_pending = 1;
    plat_mutex_unlock(&s->lock);
}

int coop_sess_command(CoopSess *s, CoopCmd cmd, const char *code) {
    char canon[COOP_CODE_LEN + 1] = "";
    if (cmd != COOP_CMD_CREATE && cmd != COOP_CMD_JOIN && cmd != COOP_CMD_LEAVE) return 0;
    if ((cmd == COOP_CMD_JOIN || (cmd == COOP_CMD_CREATE && code && *code)) && (!code || !coop_code_normalize(code, canon))) return 0;
    plat_mutex_lock(&s->lock);
    s->cmd = (int)cmd;
    copy_str(s->cmd_code, sizeof s->cmd_code, canon);
    plat_mutex_unlock(&s->lock);
    return 1;
}

int coop_sess_wait_off(CoopSess *s, uint32_t ms) { return plat_event_wait_ns(&s->off_done, (uint64_t)ms * 1000000u); }

int coop_sess_on_message(CoopSess *s, uint8_t kind, CoopMsgFn fn, void *user) {
    s->msg[kind].user = user;
    __atomic_store_n(&s->msg[kind].fn, fn, __ATOMIC_RELEASE);
    return 1;
}

int coop_sess_on_view(CoopSess *s, CoopViewFn fn, void *user) {
    plat_mutex_lock(&s->lock);
    int ok = s->n_hooks < VIEW_HOOKS;
    if (ok) s->hooks[s->n_hooks++] = (ViewHook){fn, user};
    plat_mutex_unlock(&s->lock);
    return ok;
}

void coop_sess_view(CoopSess *s, CoopView *out) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&s->lock);
    *out = s->view;
    for (int i = 0; i < out->n; i++) {
        uint64_t at = s->heard_at[out->member[i].id];
        out->member[i].heard_ms = out->member[i].id == out->self ? 0 : at ? (uint32_t)(now - at) : UINT32_MAX;
    }
    out->tx = __atomic_load_n(&s->n_tx, __ATOMIC_RELAXED);   // counted by the stepping thread without the lock
    out->rx = __atomic_load_n(&s->n_rx, __ATOMIC_RELAXED);
    out->lost = __atomic_load_n(&s->n_lost, __ATOMIC_RELAXED);
    plat_mutex_unlock(&s->lock);
}

// ---- the process's session

static CoopSess *g_sess;
static PlatOnce g_once = PLAT_ONCE_INIT;
static int g_started, g_final;

static void make_session(void *arg) {
    (void)arg;
    CoopParams p = {0};
    g_sess = coop_sess_new(&p);
    if (!g_sess) rt_log("coop: out of memory");
}

static CoopSess *process_session(void) {
    plat_once(&g_once, make_session, NULL);
    return g_sess;
}

// The player's identity on the wire: 8 bytes of SHA-256 over the local account and the profile folder. The account alone
// would make two copies of one profile the same player; with the folder they differ, and a game that stays in its folder
// keeps its id.
static uint64_t make_client_id(void) {
    uint64_t uid[2];
    account_uid(uid);
    Sha256 h;
    uint8_t digest[32];
    const char *profile = rt_config()->profile ? rt_config()->profile : "";
    sha256_init(&h);
    sha256_update(&h, "nx2navi-coop", 12);
    sha256_update(&h, uid, sizeof uid);
    sha256_update(&h, profile, strlen(profile));
    sha256_final(&h, digest);
    uint64_t id;
    memcpy(&id, digest, sizeof id);
    return id ? id : 1;
}

static void coop_main(void *arg) {
    (void)arg;
    CoopSess *s = process_session();
    unsigned seen = ~0u;
    int on = 0;
    uint64_t client_id = 0;
    char name[COOP_NAME_MAX + 1] = "", server[160] = "";
    for (;;) {
        unsigned v = cfg_version();
        if (v != seen && !__atomic_load_n(&g_final, __ATOMIC_ACQUIRE)) {   // the settings changed (the menu writes them)
            seen = v;
            int want = cfg_bool("NX2NAVI_COOP");
            const char *auto_mode = cfg_get("NX2NAVI_COOP_AUTO"), *drop = cfg_get("NX2NAVI_COOP_DROP");
            int automatic = auto_mode && (!strcmp(auto_mode, "create") || !strcmp(auto_mode, "join"));
            if (want) {
                if (!client_id) client_id = make_client_id();   // the account is read once the player turns co-op on
                const char *n = cfg_value("NX2NAVI_COOP_NAME");
                coop_name_clean(name, sizeof name, (const uint8_t *)n, strlen(n));
                if (!name[0]) snprintf(name, sizeof name, "Player-%04x", (unsigned)(client_id & 0xFFFF));
                copy_str(server, sizeof server, cfg_value("NX2NAVI_COOP_SERVER"));
            }
            CoopParams p = {.server = server, .name = name, .client_id = client_id, .on = want, .retry = automatic,
                            .local_port = (uint16_t)cfg_int("NX2NAVI_COOP_PORT"), .drop_pct = drop ? (unsigned)atoi(drop) : 0};
            coop_sess_configure(s, &p);
            if (want && !on && auto_mode && *auto_mode && strcmp(auto_mode, "off") && !automatic)
                rt_log("coop: COOP_AUTO must be off, create or join, not '%s'", auto_mode);
            if (want && !on && automatic) {   // a session at launch, for tests and LAN parties
                const char *room = cfg_get("NX2NAVI_COOP_ROOM");
                char code[COOP_CODE_LEN + 1] = "";
                int has = room && *room && coop_code_normalize(room, code);
                int join = !strcmp(auto_mode, "join");
                if (room && *room && !has) rt_log("coop: COOP_ROOM '%s' is not a room code", room);
                if (join && !has) rt_log("coop: COOP_AUTO=join needs COOP_ROOM");
                else coop_sess_command(s, join ? COOP_CMD_JOIN : COOP_CMD_CREATE, code);
            }
            on = want;
        }
        coop_sess_step(s);
    }
}

void coop_init(void) {
    if (!process_session() || g_started) return;
    g_started = 1;
    if (!plat_thread_spawn(coop_main, NULL, "coop", 0)) rt_log("coop: cannot start the co-op thread");
}

void coop_shutdown(void) {
    if (!g_started) return;
    CoopSess *s = process_session();
    __atomic_store_n(&g_final, 1, __ATOMIC_RELEASE);
    int st = __atomic_load_n(&s->pub_state, __ATOMIC_ACQUIRE);
    if (st != COOP_JOINED && st != COOP_RECONNECTING) return;   // no room to say goodbye to
    coop_sess_stop(s);
    coop_sess_wait_off(s, 400);
}

int coop_command(CoopCmd cmd, const char *code) {
    CoopSess *s = process_session();
    if (!s || __atomic_load_n(&s->pub_state, __ATOMIC_ACQUIRE) == COOP_OFF) return 0;
    return coop_sess_command(s, cmd, code);
}

void coop_view_get(CoopView *out) {
    CoopSess *s = process_session();
    if (s) coop_sess_view(s, out);
    else memset(out, 0, sizeof *out);
}

int coop_send(uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s && coop_sess_send(s, 0, kind, bytes, n);
}

int coop_send_to(int member, uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s && member > 0 && coop_sess_send(s, member, kind, bytes, n);
}

int coop_on_message(uint8_t kind, CoopMsgFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_message(s, kind, fn, user);
}

int coop_on_view(CoopViewFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_view(s, fn, user);
}
