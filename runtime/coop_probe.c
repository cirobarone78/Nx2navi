// --probe-coop: the co-op layer without a window, a Vulkan device or game files.
//
// 1. The platform's UDP sockets and addresses on 127.0.0.1.
// 2. The wire format (coop_proto.c): HMAC against known values, the byte vectors of tests/coop_vectors.txt (the file named by
//    NX2NAVI_COOP_VECTORS; the Python of tools/coop_relay.py is checked against the same file), and random bytes into every
//    parser.
// 3. Two sessions (coop.c) in this process, Alice and Bob, through a relay: they create and join a room, exchange peer messages
//    both ways, are hit with garbage and forged datagrams from the relay's address, lose the relay for good (a restart) and
//    find each other again, and leave. The relay is the real tools/coop_relay.py when NX2NAVI_COOP_RELAY=host:port names one
//    (started with --allow-chosen-codes; with NX2NAVI_COOP_RESTART=stdin the probe prints "coop-probe: restart the relay now"
//    and waits for a line on its input, which tests/coop_relay_test.py answers after restarting it), else a small relay of
//    this file on a thread, which can also corrupt a forwarded tag and send the garbage. NX2NAVI_COOP_DROP=30 makes both
//    sessions throw away that percentage of the datagrams they receive.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
// A member learns of a join or a leave from a roster datagram that may be lost; a relay sends it again within a second and every 5 s,
// so this wait holds eleven of them: at 30 % loss they are all lost once in half a million waits.
#define ROSTER_WAIT_MS 40000
#define FAIL_RET(...) do { CHECK(0, __VA_ARGS__); return; } while (0)
#define WAIT_FOR(cond, ms) ({ uint64_t t_ = plat_now_ms() + (ms); int ok_; while (!(ok_ = (cond)) && plat_now_ms() < t_) plat_sleep_ms(10); ok_; })

// ---- hex and a random source

static int hex_digit(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// "-" is empty. The bytes into out (cap), their count, -1 when text is not hex or does not fit.
static int unhex(const char *text, uint8_t *out, size_t cap) {
    if (!strcmp(text, "-")) return 0;
    size_t n = strlen(text);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hex_digit(text[2 * i]), b = hex_digit(text[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2);
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) {
    rng_state ^= rng_state >> 12, rng_state ^= rng_state << 25, rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

// ---- 1. sockets

static void probe_sockets(void) {
    PlatAddr a;
    uint8_t ip[16] = {0}, back[16];
    uint32_t scope = 0;
    char text[96];
    ip[10] = ip[11] = 0xff, ip[12] = 10, ip[13] = 1, ip[14] = 2, ip[15] = 3;
    plat_addr_set(&a, ip, 24680, 0);
    CHECK(plat_addr_port(&a) == 24680, "address port");
    plat_addr_get(&a, back, &scope);
    CHECK(!memcmp(ip, back, 16) && !scope, "address round trip");
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "10.1.2.3:24680"), "IPv4 address text: %s", text);
    PlatAddr b = a;
    CHECK(plat_addr_eq(&a, &b), "equal addresses");
    plat_addr_set(&b, ip, 24681, 0);
    CHECK(!plat_addr_eq(&a, &b), "ports differ");
    uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    plat_addr_set(&a, v6, 5, 0);
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "[2001:db8::1]:5"), "IPv6 address text: %s", text);
    uint8_t lo[16] = {0};
    lo[15] = 1;
    plat_addr_set(&a, lo, 80, 3);
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "[::1%3]:80"), "loopback with a scope: %s", text);
    CHECK(!plat_addr_str(&a, text, 5), "an address that does not fit is refused");
    uint8_t zero[16] = {0};
    plat_addr_set(&a, zero, 0, 0);
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "[::]:0"), "the unspecified address: %s", text);

    PlatAddr loop;
    CHECK(plat_net_resolve("127.0.0.1", 4000, &loop), "resolve 127.0.0.1");
    CHECK(plat_addr_str(&loop, text, sizeof text) && !strcmp(text, "127.0.0.1:4000"), "resolved text: %s", text);
    CHECK(!plat_net_resolve("", 1, &loop) && !plat_net_resolve("no such host.invalid", 1, &loop), "a name that does not resolve fails");

    PlatUdp *u1 = plat_udp_open(0, 1), *u2 = plat_udp_open(0, 0);
    CHECK(u1 && u2, "open two sockets");
    if (!u1 || !u2) return;
    CHECK(plat_udp_port(u1) && plat_udp_port(u2) && plat_udp_port(u1) != plat_udp_port(u2), "bound to free ports");
    uint8_t buf[2048], msg[300];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
    PlatAddr to1, to2, from;
    plat_net_resolve("127.0.0.1", plat_udp_port(u1), &to1);
    plat_net_resolve("127.0.0.1", plat_udp_port(u2), &to2);
    uint64_t t0 = plat_now_ms();
    CHECK(plat_udp_recv(u1, buf, sizeof buf, &from, 50) == 0, "nothing to receive is a timeout");
    CHECK(plat_now_ms() - t0 >= 40, "the timeout waited");
    CHECK(plat_udp_send(u2, &to1, msg, sizeof msg), "send IPv4 to a dual-stack socket");
    int n = plat_udp_recv(u1, buf, sizeof buf, &from, 1000);
    CHECK(n == (int)sizeof msg && !memcmp(buf, msg, sizeof msg), "the datagram arrives whole (%d bytes)", n);
    PlatAddr expect;
    plat_net_resolve("127.0.0.1", plat_udp_port(u2), &expect);
    CHECK(plat_addr_eq(&from, &expect), "the sender is reported as 127.0.0.1:port whatever the socket's family");
    CHECK(plat_udp_send(u1, &from, msg, 10), "send back from the dual-stack socket to an IPv4-only one");
    n = plat_udp_recv(u2, buf, sizeof buf, &from, 1000);
    CHECK(n == 10, "reply arrives (%d)", n);
    plat_udp_send(u2, &to1, msg, sizeof msg);
    n = plat_udp_recv(u1, buf, 100, NULL, 1000);
    CHECK(n == 100, "a datagram longer than the buffer is cut to it (%d)", n);
    uint8_t big[70000] = {0};
    CHECK(!plat_udp_send(u2, &to1, big, sizeof big), "a datagram that is too long can never be sent");
    uint8_t v6addr[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    PlatAddr far6;
    plat_addr_set(&far6, v6addr, 9, 0);
    CHECK(!plat_udp_send(u2, &far6, msg, 10), "an IPv4-only socket cannot reach an IPv6 address");
    plat_udp_close(u1);
    plat_udp_close(u2);
    plat_udp_close(NULL);
}

// ---- 2. the wire format

static void probe_hmac(void) {
    // RFC 4231 test cases 1 and 2
    static const uint8_t key1[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    static const char want1[] = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    static const char want2[] = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    uint8_t mac[32], expect[32];
    coop_hmac_sha256(key1, sizeof key1, "Hi There", 8, NULL, 0, mac);
    CHECK(unhex(want1, expect, 32) == 32 && !memcmp(mac, expect, 32), "HMAC-SHA256, RFC 4231 case 1");
    coop_hmac_sha256("Jefe", 4, "what do ya want ", 16, "for nothing?", 12, mac);   // the message in two pieces
    CHECK(unhex(want2, expect, 32) == 32 && !memcmp(mac, expect, 32), "HMAC-SHA256, RFC 4231 case 2 (two pieces)");
    uint8_t key[16], again[16];
    coop_room_key("H7K3WM", key);
    uint8_t digest[32];
    sha256("nx2navi-coop/1H7K3WM", 20, digest);
    CHECK(!memcmp(key, digest, 16), "the room key is the first 16 bytes of SHA-256 of the label and the code");
    coop_room_key("H7K3WM", again);
    CHECK(!memcmp(key, again, 16), "the room key is deterministic");
}

static void probe_server_parse(void) {
    static const struct { const char *text, *host; unsigned port; int ok; } cases[] = {
        {"relay.example", "relay.example", 24680, 1}, {"relay.example:5000", "relay.example", 5000, 1},
        {"  relay.example  ", "relay.example", 24680, 1}, {"127.0.0.1:65535", "127.0.0.1", 65535, 1},
        {"[::1]", "::1", 24680, 1}, {"[::1]:99", "::1", 99, 1}, {"2001:db8::1", "2001:db8::1", 24680, 1},
        {"", "", 0, 0}, {"   ", "", 0, 0}, {"host:", "", 0, 0}, {"host:0", "", 0, 0}, {"host:65536", "", 0, 0},
        {"host:12x", "", 0, 0}, {"[::1", "", 0, 0}, {"[::1]x", "", 0, 0}, {":80", "", 0, 0}, {"[]:80", "", 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char host[128] = "";
        uint16_t port = 0;
        int ok = coop_server_parse(cases[i].text, host, sizeof host, &port);
        CHECK(ok == cases[i].ok && (!ok || (!strcmp(host, cases[i].host) && port == cases[i].port)), "server \"%s\" parses as %d %s:%u",
              cases[i].text, ok, host, port);
    }
    char small[4];
    uint16_t port;
    CHECK(!coop_server_parse("relay.example", small, sizeof small, &port), "a host that does not fit is refused");
}

// ---- the byte vectors

static char *next_token(char **at) {
    char *p = *at;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    char *start = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *at = p;
    return start;
}

// "id:flags:namehex" entries into r; their count, -1 when malformed.
static int entries_parse(char **at, CoopRoster *r) {
    memset(r, 0, sizeof *r);
    char *t;
    while ((t = next_token(at))) {
        unsigned id, flags;
        char name_hex[80];
        if (r->n >= COOP_MAX_MEMBERS || sscanf(t, "%u:%u:%79s", &id, &flags, name_hex) != 3) return -1;
        uint8_t raw[40];
        int k = unhex(name_hex, raw, sizeof raw);
        if (k < 0 || k > COOP_NAME_MAX) return -1;
        r->m[r->n].id = (uint8_t)id, r->m[r->n].flags = (uint8_t)flags;
        memcpy(r->m[r->n].name, raw, (size_t)k);
        r->n++;
    }
    return r->n;
}

static int roster_equal(const CoopRoster *a, const CoopRoster *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->m[i].id != b->m[i].id || a->m[i].flags != b->m[i].flags || strcmp(a->m[i].name, b->m[i].name)) return 0;
    return 1;
}

static void check_vector(const char *kind, char *rest, int line) {
    uint8_t a[1300], b[1300], c[1300];
    char *name = next_token(&rest);
    if (!strcmp(kind, "hmac")) {
        char *ks = next_token(&rest), *ms = next_token(&rest), *macs = next_token(&rest);
        int kn = unhex(ks, a, sizeof a), mn = unhex(ms, b, sizeof b), cn = macs ? unhex(macs, c, 32) : -1;
        uint8_t mac[32];
        if (kn < 0 || mn < 0 || cn != 32) FAIL_RET("vector line %d: bad hmac line", line);
        coop_hmac_sha256(a, (size_t)kn, b, (size_t)mn, NULL, 0, mac);
        CHECK(!memcmp(mac, c, 32), "vector hmac %s (line %d)", name, line);
    } else if (!strcmp(kind, "code")) {
        // code <canonical|-> <text, the rest of the line>; name is the canonical form
        char out[COOP_CODE_LEN + 1] = "";
        int ok = coop_code_normalize(rest, out);
        CHECK(!strcmp(name, "-") ? !ok : ok && !strcmp(out, name), "vector code \"%s\" is %s (line %d)", rest, name, line);
    } else if (!strcmp(kind, "key")) {
        char *ks = next_token(&rest);
        uint8_t key[16];
        coop_room_key(name, key);
        CHECK(unhex(ks, a, 16) == 16 && !memcmp(key, a, 16), "vector key %s (line %d)", name, line);
    } else if (!strcmp(kind, "dgram") || !strcmp(kind, "badtag")) {
        int bad = !strcmp(kind, "badtag");
        char *code, *tok[8] = {0};
        char *dg;
        if (bad) {   // badtag <name> <code> <datagram>
            code = next_token(&rest), dg = next_token(&rest);
        } else {     // dgram <name> <code|-> <ver> <type> <room> <src> <dst> <seq> <payload> <datagram>
            code = next_token(&rest);
            for (int i = 0; i < 6; i++) tok[i] = next_token(&rest);
            tok[6] = next_token(&rest), dg = next_token(&rest);
        }
        int n = dg ? unhex(dg, a, sizeof a) : -1;
        CoopHdr h;
        const uint8_t *payload;
        if (n < 0 || !coop_dgram_parse(a, (size_t)n, &h, &payload)) FAIL_RET("vector %s %s does not parse (line %d)", kind, name, line);
        uint8_t key[16];
        int keyed = strcmp(code, "-") != 0;
        if (keyed) coop_room_key(code, key);
        if (bad) {
            CHECK(!coop_dgram_verify(a, (size_t)n, key), "vector badtag %s verifies (line %d)", name, line);
            return;
        }
        int pn = unhex(tok[6], b, sizeof b);
        CHECK(h.ver == atoi(tok[0]) && h.type == atoi(tok[1]) && h.room == (uint32_t)strtoull(tok[2], NULL, 10) &&
                  h.src == atoi(tok[3]) && h.dst == atoi(tok[4]) && h.seq == (uint32_t)strtoull(tok[5], NULL, 10) &&
                  pn == (int)h.len && (!pn || !memcmp(payload, b, (size_t)pn)),
              "vector dgram %s: header fields (line %d)", name, line);
        CHECK(keyed ? coop_dgram_verify(a, (size_t)n, key) : coop_dgram_tag_zero(a), "vector dgram %s: tag (line %d)", name, line);
        size_t k = coop_dgram_pack(c, sizeof c, &h, b, (size_t)(pn < 0 ? 0 : pn), keyed ? key : NULL);
        CHECK(k == (size_t)n && !memcmp(c, a, k), "vector dgram %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "connect")) {
        // connect <name> <intent> <flags> <code|-> <client id hex> <namehex|-> <cookie> <payload>
        char *intent = next_token(&rest), *flags = next_token(&rest), *code = next_token(&rest), *cid = next_token(&rest);
        char *nm = next_token(&rest), *cookie = next_token(&rest), *payload = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), nn = unhex(nm, b, 32), kn = unhex(cookie, c, 16);
        CoopConnect cn;
        if (pn < 0 || nn < 0 || kn != 16 || !coop_connect_parse(a, (size_t)pn, &cn)) FAIL_RET("vector connect %s does not parse (line %d)", name, line);
        CHECK(cn.intent == atoi(intent) && cn.flags == atoi(flags) && !strcmp(cn.code, strcmp(code, "-") ? code : "") &&
                  cn.client_id == strtoull(cid, NULL, 16) && strlen(cn.name) == (size_t)nn && !memcmp(cn.name, b, (size_t)nn) &&
                  !memcmp(cn.cookie, c, 16),
              "vector connect %s: fields (line %d)", name, line);
        uint8_t out[200];
        size_t k = coop_connect_pack(out, sizeof out, &cn);
        CHECK(k == (size_t)pn && !memcmp(out, a, k), "vector connect %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "roster") || !strcmp(kind, "joined")) {
        int joined = !strcmp(kind, "joined");
        char *payload = next_token(&rest);   // joined: <member> <code> follow
        int pn = unhex(payload, a, sizeof a);
        char *member = joined ? next_token(&rest) : NULL, *code = joined ? next_token(&rest) : NULL;
        CoopRoster want;
        if (pn < 0 || entries_parse(&rest, &want) < 0 || (joined && (!member || !code))) FAIL_RET("vector %s %s is malformed (line %d)", kind, name, line);
        uint8_t out[300];
        if (joined) {
            CoopJoined j;
            CHECK(coop_joined_parse(a, (size_t)pn, &j) && j.member == atoi(member) && !strcmp(j.code, code) && roster_equal(&j.roster, &want),
                  "vector joined %s: fields (line %d)", name, line);
            size_t k = coop_joined_pack(out, sizeof out, &j);
            CHECK(k == (size_t)pn && !memcmp(out, a, k), "vector joined %s: packs to the same bytes (line %d)", name, line);
        } else {
            CoopRoster r;
            size_t used = 0;
            CHECK(coop_roster_parse(a, (size_t)pn, &r, &used) && used == (size_t)pn && roster_equal(&r, &want), "vector roster %s: fields (line %d)", name, line);
            size_t k = coop_roster_pack(out, sizeof out, &r);
            CHECK(k == (size_t)pn && !memcmp(out, a, k), "vector roster %s: packs to the same bytes (line %d)", name, line);
        }
    } else if (!strcmp(kind, "error")) {
        char *payload = next_token(&rest), *reason = next_token(&rest), *text = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), tn = text ? unhex(text, b, 64) : -1;
        CoopError e;
        if (pn < 0 || tn < 0 || !coop_error_parse(a, (size_t)pn, &e)) FAIL_RET("vector error %s does not parse (line %d)", name, line);
        CHECK(e.reason == atoi(reason) && strlen(e.text) == (size_t)tn && !memcmp(e.text, b, (size_t)tn), "vector error %s: fields (line %d)", name, line);
        uint8_t out[80];
        size_t k = coop_error_pack(out, sizeof out, &e);
        CHECK(k == (size_t)pn && !memcmp(out, a, k), "vector error %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "peer")) {
        char *payload = next_token(&rest), *k = next_token(&rest), *bytes = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), bn = bytes ? unhex(bytes, b, sizeof b) : -1;
        uint8_t kind_byte = 0;
        const uint8_t *got = NULL;
        size_t ngot = 0;
        int parsed = pn >= 0 && bn >= 0 && coop_peer_parse(a, (size_t)pn, &kind_byte, &got, &ngot);
        CHECK(parsed && kind_byte == atoi(k) && ngot == (size_t)bn && (!bn || !memcmp(got, b, (size_t)bn)), "vector peer %s: fields (line %d)",
              name, line);
        if (!parsed) return;
        size_t k2 = coop_peer_pack(c, sizeof c, kind_byte, got, ngot);
        CHECK(k2 == (size_t)pn && !memcmp(c, a, k2), "vector peer %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "reject")) {
        // reject <kind> <name> <bytes>
        char *what = name, *nm = next_token(&rest), *bytes = next_token(&rest);
        int n = bytes ? unhex(bytes, a, sizeof a) : -1;
        if (n < 0) FAIL_RET("vector reject %s: bad hex (line %d)", nm, line);
        int parsed;
        CoopHdr h;
        const uint8_t *p;
        CoopConnect cn;
        CoopRoster r;
        CoopJoined j;
        CoopError e;
        uint8_t kb;
        size_t ng;
        if (!strcmp(what, "dgram")) parsed = coop_dgram_parse(a, (size_t)n, &h, &p);
        else if (!strcmp(what, "connect")) parsed = coop_connect_parse(a, (size_t)n, &cn);
        else if (!strcmp(what, "roster")) parsed = coop_roster_parse(a, (size_t)n, &r, NULL);
        else if (!strcmp(what, "joined")) parsed = coop_joined_parse(a, (size_t)n, &j);
        else if (!strcmp(what, "error")) parsed = coop_error_parse(a, (size_t)n, &e);
        else parsed = coop_peer_parse(a, (size_t)n, &kb, &p, &ng);
        CHECK(!parsed, "vector reject %s %s parses (line %d)", what, nm, line);
    } else {
        CHECK(0, "vector line %d: unknown kind %s", line, kind);
    }
}

static int probe_vectors(void) {
    const char *path = cfg_get("NX2NAVI_COOP_VECTORS");
    if (!path || !*path) {
        printf("coop-probe: no NX2NAVI_COOP_VECTORS: the byte vectors are not checked\n");
        return 0;
    }
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (!text) {
        CHECK(0, "cannot read the vectors file %s", path);
        return 0;
    }
    int lines = 0, line = 0;
    for (char *p = text, *next; *p; p = next, line++) {
        next = strchr(p, '\n');
        if (next) *next++ = 0;
        else next = p + strlen(p);
        if (!*p || *p == '#') continue;
        char *rest = p;
        char *kind = next_token(&rest);
        if (!kind) continue;
        check_vector(kind, rest, line + 1);
        lines++;
    }
    free(text);
    CHECK(lines > 60, "the vectors file has %d lines of vectors", lines);
    printf("coop-probe: %d byte vectors checked\n", lines);
    return lines;
}

static void probe_fuzz(void) {
    uint8_t buf[1400], out[1400];
    uint8_t key[16];
    coop_room_key("H7K3WM", key);
    // valid datagrams to start from
    uint8_t seeds[6][400];
    size_t seed_len[6];
    CoopRoster r = {.n = 2};
    r.m[0] = (CoopMemberInfo){1, 0, "Alice"}, r.m[1] = (CoopMemberInfo){2, 1, "Bob"};
    uint8_t payload[300];
    CoopHdr h = {.ver = 1, .room = 7, .src = 1, .dst = 2, .seq = 5};
    size_t n = coop_roster_pack(payload, sizeof payload, &r);
    h.type = COOP_T_ROSTER, seed_len[0] = coop_dgram_pack(seeds[0], sizeof seeds[0], &h, payload, n, key);
    CoopJoined j = {.member = 2, .roster = r};
    memcpy(j.code, "H7K3WM", 7);
    n = coop_joined_pack(payload, sizeof payload, &j);
    h.type = COOP_T_JOINED, seed_len[1] = coop_dgram_pack(seeds[1], sizeof seeds[1], &h, payload, n, key);
    CoopConnect c = {.intent = 2, .client_id = 99};
    memcpy(c.code, "H7K3WM", 7);
    strcpy(c.name, "Bob");
    n = coop_connect_pack(payload, sizeof payload, &c);
    h.type = COOP_T_CONNECT, seed_len[2] = coop_dgram_pack(seeds[2], sizeof seeds[2], &h, payload, n, NULL);
    CoopError e = {.reason = 1};
    strcpy(e.text, "no such room");
    n = coop_error_pack(payload, sizeof payload, &e);
    h.type = COOP_T_ERROR, seed_len[3] = coop_dgram_pack(seeds[3], sizeof seeds[3], &h, payload, n, key);
    n = coop_peer_pack(payload, sizeof payload, COOP_K_TEST, "hello", 5);
    h.type = COOP_T_DATA, seed_len[4] = coop_dgram_pack(seeds[4], sizeof seeds[4], &h, payload, n, key);
    h.type = COOP_T_PONG, seed_len[5] = coop_dgram_pack(seeds[5], sizeof seeds[5], &h, "abcd", 4, key);
    long parsed = 0, tag_ok = 0;
    for (int it = 0; it < 400000; it++) {
        size_t len;
        int mode = it % 4;
        if (mode == 0) {   // random bytes
            len = rnd32() % 1300;
            for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rnd32() % 5 ? rnd32() : 0);
        } else {           // a valid datagram with bytes changed, or cut short
            int s = (int)(rnd32() % 6);
            len = seed_len[s];
            memcpy(buf, seeds[s], len);
            for (unsigned k = rnd32() % 4; k && len; k--) buf[rnd32() % len] = (uint8_t)rnd32();
            if (mode == 3) len = rnd32() % (len + 1);
        }
        CoopHdr hh;
        const uint8_t *p;
        if (!coop_dgram_parse(buf, len, &hh, &p)) {
            // the payload parsers on whatever bytes there are
            p = buf, hh.len = (uint16_t)len;
        } else {
            parsed++;
            tag_ok += coop_dgram_verify(buf, len, key);
        }
        CoopConnect cn;
        CoopRoster rr;
        CoopJoined jj;
        CoopError ee;
        uint32_t v;
        uint8_t kind;
        const uint8_t *bytes;
        size_t nb;
        char nameb[COOP_NAME_MAX + 1], codeb[COOP_CODE_LEN + 1];
        if (coop_connect_parse(p, hh.len, &cn)) coop_connect_pack(out, sizeof out, &cn);
        if (coop_roster_parse(p, hh.len, &rr, NULL)) coop_roster_pack(out, sizeof out, &rr);
        if (coop_joined_parse(p, hh.len, &jj)) coop_joined_pack(out, sizeof out, &jj);
        if (coop_error_parse(p, hh.len, &ee)) coop_error_pack(out, sizeof out, &ee);
        coop_u32_parse(p, hh.len, &v);
        coop_peer_parse(p, hh.len, &kind, &bytes, &nb);
        coop_name_clean(nameb, sizeof nameb, p, hh.len);
        char text[40];
        size_t tl = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, buf, tl);
        text[tl] = 0;
        coop_code_normalize(text, codeb);
    }
    CHECK(parsed > 1000, "the fuzz reached the payload parsers (%ld datagrams parsed)", parsed);
    CHECK(tag_ok < parsed, "a changed datagram's tag no longer verifies");
    printf("coop-probe: parser fuzz: 400000 inputs, %ld parsed as datagrams\n", parsed);
}

// ---- 3a. a relay of this file

typedef struct { int used; uint64_t cid; char name[COOP_NAME_MAX + 1]; PlatAddr addr; uint32_t connect_seq; } MockMember;
typedef struct { int used; uint32_t id; char code[COOP_CODE_LEN + 1]; uint8_t key[COOP_KEY_LEN]; MockMember m[COOP_MAX_MEMBERS + 1]; } MockRoom;

typedef struct {
    PlatUdp *sock;
    PlatThread *thread;
    uint16_t port;
    volatile int stop, restart, corrupt_next, fuzz;
    volatile unsigned forwarded;
    MockRoom rooms[4];
    uint8_t secret[32];
    uint32_t seq;
    uint64_t next_roster;
} MockRelay;

static void mock_send(MockRelay *r, const PlatAddr *to, uint8_t type, uint32_t room, uint8_t dst, const void *payload, size_t n, const uint8_t *key) {
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = room, .dst = dst, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, key);
    if (k) plat_udp_send(r->sock, to, d, k);
}

static void mock_cookie(MockRelay *r, const PlatAddr *a, uint8_t out[COOP_COOKIE_LEN]) {
    uint8_t mac[32];
    coop_hmac_sha256(r->secret, sizeof r->secret, a->b, sizeof a->b, NULL, 0, mac);
    memcpy(out, mac, COOP_COOKIE_LEN);
}

static void mock_roster_of(const MockRoom *room, CoopRoster *out) {
    memset(out, 0, sizeof *out);
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used) {
            out->m[out->n].id = (uint8_t)i;
            memcpy(out->m[out->n].name, room->m[i].name, sizeof out->m[out->n].name);
            out->n++;
        }
}

static void mock_roster_to_all(MockRelay *r, MockRoom *room, int skip) {
    CoopRoster ro;
    uint8_t payload[300];
    mock_roster_of(room, &ro);
    size_t n = coop_roster_pack(payload, sizeof payload, &ro);
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used && i != skip) mock_send(r, &room->m[i].addr, COOP_T_ROSTER, room->id, (uint8_t)i, payload, n, room->key);
}

static void mock_connect(MockRelay *r, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, const PlatAddr *from) {
    CoopConnect c;
    if (n < COOP_CONNECT_MIN || !coop_connect_parse(p, h->len, &c) || (c.intent != COOP_INTENT_CREATE && c.intent != COOP_INTENT_JOIN)) return;
    uint8_t cookie[COOP_COOKIE_LEN], key[COOP_KEY_LEN];
    int keyed = c.code[0] != 0;
    if (keyed) coop_room_key(c.code, key);
    mock_cookie(r, from, cookie);
    if (memcmp(cookie, c.cookie, sizeof cookie)) {
        mock_send(r, from, COOP_T_CHALLENGE, 0, 0, cookie, sizeof cookie, keyed ? key : NULL);
        return;
    }
    if (keyed && !coop_dgram_verify(d, n, key)) return;
    MockRoom *room = NULL;
    for (int i = 0; i < 4 && keyed; i++)
        if (r->rooms[i].used && !strcmp(r->rooms[i].code, c.code)) room = &r->rooms[i];
    if (!room && c.intent == COOP_INTENT_JOIN) {
        CoopError e = {.reason = COOP_E_NO_ROOM};
        strcpy(e.text, "no such room");
        uint8_t payload[80];
        mock_send(r, from, COOP_T_ERROR, 0, 0, payload, coop_error_pack(payload, sizeof payload, &e), keyed ? key : NULL);
        return;
    }
    if (!room) {   // a create: the chosen code, or a made-up one
        for (int i = 0; i < 4 && !room; i++)
            if (!r->rooms[i].used) room = &r->rooms[i];
        if (!room) return;
        memset(room, 0, sizeof *room);
        room->used = 1, room->id = 0xC0DE0000u + rnd32() % 0xFFFF + 1;
        if (keyed) memcpy(room->code, c.code, sizeof room->code);
        else memcpy(room->code, "BCDFGH", 7);
        coop_room_key(room->code, room->key);
    }
    int id = 0;
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)   // the same client keeps its id; else the first free one
        if (room->m[i].used && room->m[i].cid == c.client_id) id = i;
    for (int i = 1; i <= COOP_MAX_MEMBERS && !id; i++)
        if (!room->m[i].used) id = i;
    if (!id) {
        CoopError e = {.reason = COOP_E_ROOM_FULL};
        strcpy(e.text, "room full");
        uint8_t payload[80];
        mock_send(r, from, COOP_T_ERROR, 0, 0, payload, coop_error_pack(payload, sizeof payload, &e), room->key);
        return;
    }
    int fresh = !room->m[id].used || room->m[id].connect_seq != h->seq;
    room->m[id] = (MockMember){.used = 1, .cid = c.client_id, .addr = *from, .connect_seq = h->seq};
    memcpy(room->m[id].name, c.name, sizeof room->m[id].name);
    CoopJoined j = {.member = (uint8_t)id};
    memcpy(j.code, room->code, sizeof j.code);
    mock_roster_of(room, &j.roster);
    uint8_t payload[300];
    mock_send(r, from, COOP_T_JOINED, room->id, (uint8_t)id, payload, coop_joined_pack(payload, sizeof payload, &j), room->key);
    if (fresh) mock_roster_to_all(r, room, id);
}

static void mock_handle(MockRelay *r, uint8_t *d, size_t n, const PlatAddr *from) {
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(d, n, &h, &p) || h.ver != COOP_PROTO) return;
    if (h.type == COOP_T_CONNECT) return mock_connect(r, d, n, &h, p, from);
    MockRoom *room = NULL;
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && r->rooms[i].id == h.room) room = &r->rooms[i];
    if (!room || h.src < 1 || h.src > COOP_MAX_MEMBERS || !room->m[h.src].used || !plat_addr_eq(&room->m[h.src].addr, from)) return;
    if (!coop_dgram_verify(d, n, room->key)) return;
    if (h.type == COOP_T_PING) {
        mock_send(r, from, COOP_T_PONG, room->id, h.src, p, h.len > 4 ? 4 : h.len, room->key);
    } else if (h.type == COOP_T_LEAVE) {
        room->m[h.src].used = 0;
        mock_roster_to_all(r, room, 0);
    } else if (h.type == COOP_T_DATA) {
        if (r->corrupt_next) {   // one forwarded datagram with a flipped tag bit: no receiver may deliver it
            r->corrupt_next = 0;
            d[16] ^= 1;
        }
        for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
            if (room->m[i].used && i != h.src && (h.dst == COOP_DST_ALL || h.dst == i)) {
                plat_udp_send(r->sock, &room->m[i].addr, d, n);
                r->forwarded++;
            }
    }
}

// Datagrams of every kind to member 1 of the first room from the relay's own address, as a hostile or broken relay would: bytes
// at random, valid ones changed or cut, and ones carrying the room's tag with a random payload (stale roster counters and
// kinds nothing listens to, so a delivered one changes nothing the probe measures).
static void mock_fuzz(MockRelay *r) {
    MockRoom *room = NULL;
    for (int i = 0; i < 4 && !room; i++)
        if (r->rooms[i].used && r->rooms[i].m[1].used) room = &r->rooms[i];
    if (!room) return;
    PlatAddr to = room->m[1].addr;
    uint8_t d[1400], payload[COOP_MAX_PAYLOAD];   // d holds the random lengths below too
    for (int it = 0; it < 6000 && !r->stop; it++) {
        size_t n;
        int mode = it % 3;
        if (mode == 0) {
            n = rnd32() % 1300;
            for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(rnd32() % 4 ? rnd32() : 0);
            if (n > 12 && rnd32() % 2) d[0] = 0x4E, d[1] = 0x43, d[2] = 1;
        } else {
            size_t pn = rnd32() % 200;
            for (size_t i = 0; i < pn; i++) payload[i] = (uint8_t)rnd32();
            static const uint8_t types[] = {COOP_T_CHALLENGE, COOP_T_JOINED, COOP_T_ERROR, COOP_T_ROSTER, COOP_T_PONG, COOP_T_DATA, COOP_T_PING, 0x77};
            uint8_t type = types[rnd32() % sizeof types];
            if (type == COOP_T_DATA && pn) payload[0] = (uint8_t)(0x10 + rnd32() % 0xD0);   // a kind nothing listens to
            CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = mode == 1 ? room->id : rnd32(), .src = (uint8_t)(rnd32() % 10),
                         .dst = (uint8_t)(rnd32() % 3 == 0 ? COOP_DST_ALL : 1), .seq = type == COOP_T_ROSTER ? 0 : rnd32()};
            n = coop_dgram_pack(d, sizeof d, &h, payload, pn, room->key);
            if (mode == 2 && n) d[rnd32() % n] ^= (uint8_t)(1u << (rnd32() % 8));
        }
        plat_udp_send(r->sock, &to, d, n);
        if (it % 40 == 0) plat_sleep_ms(1);
    }
}

static void mock_main(void *arg) {
    MockRelay *r = arg;
    uint8_t buf[2048];
    PlatAddr from;
    while (!r->stop) {
        if (r->restart) {   // the process went away and came back: rooms gone, a new cookie secret, 400 ms of silence
            memset(r->rooms, 0, sizeof r->rooms);
            plat_random(r->secret, sizeof r->secret);
            uint64_t until = plat_now_ms() + 400;
            while (plat_now_ms() < until) plat_udp_recv(r->sock, buf, sizeof buf, &from, 20);
            r->restart = 0;
        }
        if (r->fuzz) {
            mock_fuzz(r);
            r->fuzz = 0;
        }
        int n = plat_udp_recv(r->sock, buf, sizeof buf, &from, 10);
        if (n > 0) mock_handle(r, buf, (size_t)n, &from);
        if (plat_now_ms() >= r->next_roster) {   // the roster again every second: a lost update repairs itself
            r->next_roster = plat_now_ms() + 1000;
            for (int i = 0; i < 4; i++)
                if (r->rooms[i].used) mock_roster_to_all(r, &r->rooms[i], 0);
        }
    }
}

static MockRelay *mock_start(void) {
    MockRelay *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->sock = plat_udp_open(0, 0);
    if (!r->sock) {
        free(r);
        return NULL;
    }
    r->port = plat_udp_port(r->sock);
    plat_random(r->secret, sizeof r->secret);
    r->thread = plat_thread_new(mock_main, r, "coop-mock-relay");
    return r;
}

static void mock_stop(MockRelay *r) {
    r->stop = 1;
    plat_thread_join(r->thread);
    plat_udp_close(r->sock);
    free(r);
}

// ---- 3b. two sessions

typedef struct {
    const char *name;
    CoopSess *sess;
    PlatThread *thread;
    volatile int stop;
    volatile unsigned views, saw_reconnect;
    volatile unsigned got, dups, byes, from_wrong;   // test messages received, delivered twice, BYEs, from a member other than the expected
    uint32_t seen[512];
    int expect_from;
} Player;

static void player_loop(void *arg) {
    Player *p = arg;
    while (!p->stop) coop_sess_step(p->sess);
}

static void on_test_message(int from, const uint8_t *bytes, size_t n, void *user) {
    Player *p = user;
    uint32_t seq;
    if (n != 9 || memcmp(bytes, "probe", 4) || !coop_u32_parse(bytes + 5, 4, &seq) || seq >= 512 * 32) return;
    if (p->expect_from && from != p->expect_from) __atomic_add_fetch(&p->from_wrong, 1, __ATOMIC_RELAXED);
    uint32_t bit = 1u << (seq % 32);
    if (p->seen[seq / 32] & bit) __atomic_add_fetch(&p->dups, 1, __ATOMIC_RELAXED);
    p->seen[seq / 32] |= bit;
    __atomic_add_fetch(&p->got, 1, __ATOMIC_RELAXED);
}

static void on_bye(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)from, (void)bytes, (void)n;
    __atomic_add_fetch(&((Player *)user)->byes, 1, __ATOMIC_RELAXED);
}

static void on_view_change(const CoopView *v, void *user) {
    Player *p = user;
    __atomic_add_fetch(&p->views, 1, __ATOMIC_RELAXED);
    if (v->state == COOP_RECONNECTING) __atomic_add_fetch(&p->saw_reconnect, 1, __ATOMIC_RELAXED);
}

static uint16_t free_port(void) {
    PlatUdp *u = plat_udp_open(0, 0);
    uint16_t port = u ? plat_udp_port(u) : 0;
    plat_udp_close(u);
    return port;
}

static void player_start(Player *p, const char *name, uint64_t cid, const char *server, unsigned drop, int retry, uint16_t local_port) {
    CoopParams cp = {.tag = name, .server = server, .name = name, .client_id = cid, .on = 1, .drop_pct = drop, .retry = retry,
                     .local_port = local_port, .ping_ms = 200, .silence_ms = 1500, .burst_gap_ms = 100, .backoff_first_ms = 150,
                     .backoff_max_ms = 500};
    memset(p, 0, sizeof *p);
    p->name = name;
    p->sess = coop_sess_new(&cp);
    if (!p->sess) return;
    coop_sess_on_message(p->sess, COOP_K_TEST, on_test_message, p);
    coop_sess_on_message(p->sess, COOP_K_BYE, on_bye, p);
    coop_sess_on_view(p->sess, on_view_change, p);
    p->thread = plat_thread_new(player_loop, p, "coop-probe");
}

static void player_end(Player *p) {
    if (!p->sess) return;
    coop_sess_stop(p->sess);
    coop_sess_wait_off(p->sess, 1000);
    p->stop = 1;
    plat_thread_join(p->thread);
    coop_sess_free(p->sess);
    p->sess = NULL;
}

static CoopView view_of(Player *p) {
    CoopView v;
    coop_sess_view(p->sess, &v);
    return v;
}

static int in_room(Player *p, int members) {
    CoopView v = view_of(p);
    return v.state == COOP_JOINED && v.n == members;
}

// n numbered messages from a to b, 15 ms apart, until b has got the number a count asks for (or they ran out).
// to: the member id they are addressed to, or 0 for all.
static unsigned send_messages(Player *a, Player *b, int to, int n, uint32_t first) {
    unsigned before = b->got;
    for (int i = 0; i < n; i++) {
        uint8_t m[9];
        memcpy(m, "probe", 5);
        coop_u32_pack(m + 5, 4, first + (uint32_t)i);
        m[4] = 0;
        coop_sess_send(a->sess, to, COOP_K_TEST, m, sizeof m);
        plat_sleep_ms(15);
    }
    plat_sleep_ms(300);
    return b->got - before;
}

static void exchange(Player *a, Player *b, unsigned drop, const char *what, uint32_t *first) {
    int ida = view_of(a).self, idb = view_of(b).self;
    a->expect_from = idb, b->expect_from = ida;
    unsigned min = drop ? 20 : 55;
    unsigned ab = send_messages(a, b, 0, 60, *first);
    *first += 60;
    unsigned ba = send_messages(b, a, ida, 60, *first);   // addressed to one member
    *first += 60;
    CHECK(ab >= min, "%s: Alice's messages reached Bob: %u of 60 (at least %u)", what, ab, min);
    CHECK(ba >= min, "%s: Bob's messages reached Alice: %u of 60 (at least %u)", what, ba, min);
    CHECK(!a->dups && !b->dups, "%s: no message was delivered twice (%u, %u)", what, a->dups, b->dups);
    CHECK(!a->from_wrong && !b->from_wrong, "%s: every message came from the right member", what);
    printf("coop-probe: %s: Alice -> Bob %u of 60, Bob -> Alice %u of 60\n", what, ab, ba);
}

// The process's session, driven by the settings as the game does it: COOP, COOP_SERVER, COOP_NAME, COOP_AUTO and COOP_ROOM in the
// environment (cfg_setenv bumps cfg_version, which the coop thread polls), then coop_init, the commands, a rename, switching
// COOP off and on again, and coop_shutdown. alice is in the room of code H7K3WM with Bob.
static void probe_process(const char *server, unsigned drop, Player *alice, uint32_t *first) {
    char drop_text[16];
    snprintf(drop_text, sizeof drop_text, "%u", drop);
    cfg_setenv("NX2NAVI_COOP_NAME", NULL);
    cfg_setenv("NX2NAVI_COOP_SERVER", server);
    cfg_setenv("NX2NAVI_COOP_AUTO", "join");
    cfg_setenv("NX2NAVI_COOP_ROOM", "h7k-3wm");   // a code as a player types it
    cfg_setenv("NX2NAVI_COOP_DROP", drop_text);
    cfg_setenv("NX2NAVI_COOP", "0");
    CoopView v;
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "the process's session is off until COOP is on (state %d)", v.state);
    CHECK(!coop_send(COOP_K_TEST, "x", 1) && !coop_command(COOP_CMD_CREATE, NULL), "nothing is sent and no command taken while off");
    Player proc;
    memset(&proc, 0, sizeof proc);
    proc.name = "process";
    CHECK(coop_on_message(COOP_K_TEST, on_test_message, &proc) && coop_on_view(on_view_change, &proc), "handlers register");
    coop_init();
    coop_init();   // twice is the same as once
    cfg_setenv("NX2NAVI_COOP", "1");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 30000), "COOP_AUTO=join joins the room of COOP_ROOM (state %d, %s)", v.state, v.status);
    CHECK(v.self == 3 && v.n == 3 && !strcmp(v.code, "H7K-3WM"), "as member 3 of H7K-3WM (self %d, %d members, %s)", v.self, v.n, v.code);
    CHECK(!strncmp(v.member[2].name, "Player-", 7) && strlen(v.member[2].name) == 11, "under the default name Player-xxxx (%s)", v.member[2].name);
    CHECK(WAIT_FOR(in_room(alice, 3), ROSTER_WAIT_MS), "Alice sees three members");
    CHECK(proc.views > 0, "the view hook ran");

    // messages both ways through the interface other layers use
    alice->expect_from = 3, proc.expect_from = 1;
    unsigned got_before = proc.got;
    for (int i = 0; i < 40; i++) {
        uint8_t m[9];
        memcpy(m, "probe", 5);
        m[4] = 0;
        coop_u32_pack(m + 5, 4, *first + (uint32_t)i);
        coop_sess_send(alice->sess, 3, COOP_K_TEST, m, sizeof m);
        coop_u32_pack(m + 5, 4, *first + 100 + (uint32_t)i);
        coop_send(COOP_K_TEST, m, sizeof m);   // to everyone: Alice and Bob
        plat_sleep_ms(15);
    }
    plat_sleep_ms(300);
    unsigned min = drop ? 14 : 37;
    CHECK(proc.got - got_before >= min && !proc.dups && !proc.from_wrong, "the process got %u of 40 from Alice (%u twice, %u from the wrong member)",
          proc.got - got_before, proc.dups, proc.from_wrong);
    CHECK(alice->got >= min, "Alice got messages from the process: %u", alice->got);
    *first += 200;
    alice->got = 0;
    CHECK(coop_send_to(1, COOP_K_TEST, "abc", 3) && !coop_send_to(0, COOP_K_TEST, "abc", 3) && !coop_send(COOP_K_TEST, "x", COOP_MAX_PAYLOAD),
          "coop_send_to takes a member, a message that is too long is refused");

    // a new name joins the room again under it
    cfg_setenv("NX2NAVI_COOP_NAME", "Process");
    CHECK(WAIT_FOR(in_room(alice, 3) && !strcmp(view_of(alice).member[2].name, "Process"), ROSTER_WAIT_MS), "a new COOP_NAME shows in Alice's roster (%s)",
          view_of(alice).member[2].name);
    coop_view_get(&v);
    CHECK(v.state == COOP_JOINED || WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 15000), "and the process is back in the room");

    // commands
    CHECK(!coop_command(COOP_CMD_JOIN, "not a code") && !coop_command(COOP_CMD_JOIN, NULL) && !coop_command(COOP_CMD_CREATE, "BCDF"), "a bad room code is refused");
    CHECK(coop_command(COOP_CMD_LEAVE, NULL), "leave is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT) && in_room(alice, 2), ROSTER_WAIT_MS), "the process left (state %d)", v.state);
    CHECK(coop_command(COOP_CMD_JOIN, "H7K 3WM"), "join is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED) && in_room(alice, 3), ROSTER_WAIT_MS), "and the process joined again (state %d, %s)", v.state, v.status);

    // COOP off and on again
    cfg_setenv("NX2NAVI_COOP", "0");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && in_room(alice, 2), ROSTER_WAIT_MS), "COOP = 0 leaves the room and turns the session off (state %d)", v.state);
    cfg_setenv("NX2NAVI_COOP", "1");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED) && in_room(alice, 3), ROSTER_WAIT_MS), "COOP = 1 starts COOP_AUTO again (state %d, %s)", v.state, v.status);

    // the process ends: LEAVE and BYE
    uint64_t t0 = plat_now_ms();
    coop_shutdown();
    CHECK(plat_now_ms() - t0 < 1500, "coop_shutdown returns within a second and a half (%u ms)", (unsigned)(plat_now_ms() - t0));
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "and leaves the session off (state %d)", v.state);
    CHECK(WAIT_FOR(in_room(alice, 2), ROSTER_WAIT_MS), "Alice sees the process go");
    cfg_setenv("NX2NAVI_COOP", "0");
    cfg_setenv("NX2NAVI_COOP_AUTO", NULL);
    cfg_setenv("NX2NAVI_COOP_ROOM", NULL);
}

static void probe_sessions(void) {
    unsigned drop = 0;
    const char *d = cfg_get("NX2NAVI_COOP_DROP");
    if (d) drop = (unsigned)atoi(d);
    const char *relay_env = cfg_get("NX2NAVI_COOP_RELAY");
    int real = relay_env && *relay_env;
    const char *restart_mode = cfg_get("NX2NAVI_COOP_RESTART");
    MockRelay *mock = NULL;
    char server[64];
    if (real) {
        snprintf(server, sizeof server, "%s", relay_env);
    } else {
        mock = mock_start();
        if (!mock) FAIL_RET("the mock relay did not start");
        snprintf(server, sizeof server, "127.0.0.1:%u", mock->port);
    }
    printf("coop-probe: sessions through %s relay %s, %u%% of received datagrams dropped\n", real ? "the real" : "the mock", server, drop);

    Player alice, bob;
    uint16_t alice_port = free_port();
    player_start(&alice, "Alice", 0xA11CE, server, drop, 1, alice_port);
    player_start(&bob, "Bob", 0xB0B, server, drop, 1, 0);
    if (!alice.sess || !bob.sess) FAIL_RET("a session did not start");
    CoopView v = view_of(&alice);
    CHECK(v.state == COOP_LEFT, "a session that is on starts in no room (state %d)", v.state);

    // --- a room is made and joined
    coop_sess_command(alice.sess, COOP_CMD_CREATE, "H7K3WM");   // a relay started with --allow-chosen-codes keeps the code
    CHECK(WAIT_FOR(view_of(&alice).state == COOP_JOINED, 30000), "Alice joined her room (state %d, %s)", view_of(&alice).state, view_of(&alice).status);
    v = view_of(&alice);
    CHECK(!strcmp(v.code, "H7K-3WM") && v.self == 1 && v.n == 1 && !strcmp(v.member[0].name, "Alice"),
          "Alice is member 1 of room %s (self %d, %d members)", v.code, v.self, v.n);
    coop_sess_command(bob.sess, COOP_CMD_JOIN, "H7K3WM");
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "both see two members (Alice %d, Bob %d)", view_of(&alice).n, view_of(&bob).n);
    v = view_of(&bob);
    CHECK(v.self == 2 && !strcmp(v.code, "H7K-3WM") && v.n == 2 && !strcmp(v.member[0].name, "Alice") && !strcmp(v.member[1].name, "Bob"),
          "Bob is member 2 and sees Alice (self %d, %d members)", v.self, v.n);
    CHECK(WAIT_FOR(view_of(&bob).ping_ms > 0, 5000), "the round trip to the relay is measured");
    CHECK(alice.views > 0 && bob.views > 0, "the view hooks ran");
    printf("coop-probe: room %s, Bob's round trip to the relay %u ms\n", v.code, view_of(&bob).ping_ms);

    uint32_t first = 0;
    exchange(&alice, &bob, drop, "in the room", &first);

    // --- hostile datagrams (the mock relay only: it knows the room key)
    if (mock) {
        unsigned got_before = alice.got;
        mock->corrupt_next = 1;
        uint8_t m[9];
        memcpy(m, "probe", 5);
        m[4] = 0;
        coop_u32_pack(m + 5, 4, 500);
        coop_sess_send(bob.sess, 0, COOP_K_TEST, m, sizeof m);
        plat_sleep_ms(300);
        CHECK(alice.got == got_before && !(alice.seen[500 / 32] & (1u << (500 % 32))), "a datagram whose tag was changed in transit is not delivered");
        uint32_t lost_before = view_of(&alice).lost;
        mock->fuzz = 1;
        CHECK(WAIT_FOR(!mock->fuzz, 60000), "the garbage was sent");
        plat_sleep_ms(300);
        CHECK(view_of(&alice).lost > lost_before, "the garbage was counted as thrown away (%u)", view_of(&alice).lost - lost_before);
        CHECK(WAIT_FOR(in_room(&alice, 2) && in_room(&bob, 2), ROSTER_WAIT_MS), "after the garbage both are still in the room (Alice %d, Bob %d)",
              view_of(&alice).state, view_of(&bob).state);
        exchange(&alice, &bob, drop, "after the garbage", &first);
        // an outsider's datagrams to Alice's port are not the relay's: thrown away
        PlatUdp *evil = plat_udp_open(0, 0);
        PlatAddr to;
        plat_net_resolve("127.0.0.1", alice_port, &to);
        uint8_t junk[200];
        lost_before = view_of(&alice).lost;
        for (int i = 0; evil && i < 50; i++) {
            for (size_t k = 0; k < sizeof junk; k++) junk[k] = (uint8_t)rnd32();
            plat_udp_send(evil, &to, junk, 10 + (size_t)(rnd32() % 190));
        }
        plat_udp_close(evil);
        CHECK(WAIT_FOR(view_of(&alice).lost >= lost_before + 20, 5000), "datagrams from another address are thrown away");
        CHECK(in_room(&alice, 2), "and change nothing");
    }

    // --- the relay is lost for good and comes back
    int restarted = 1;
    if (mock) {
        mock->restart = 1;
        printf("coop-probe: the mock relay restarts\n");
    } else if (restart_mode && !strcmp(restart_mode, "stdin")) {
        printf("coop-probe: restart the relay now\n");
        char line[64] = "";
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "restarted", 9)) CHECK(0, "the relay was not restarted (got \"%s\")", line);
    } else {
        printf("coop-probe: restart phase skipped: the relay is not ours (NX2NAVI_COOP_RESTART=stdin to do it by hand)\n");
        restarted = 0;
    }
    if (restarted) {
        CHECK(WAIT_FOR(alice.saw_reconnect && bob.saw_reconnect, 15000), "both sessions noticed that the relay was gone (%u, %u)",
              alice.saw_reconnect, bob.saw_reconnect);
        CHECK(WAIT_FOR(in_room(&alice, 2) && in_room(&bob, 2), 60000),
              "after a relay restart both are in a room again (Alice state %d %s, Bob state %d %s)", view_of(&alice).state,
              view_of(&alice).status, view_of(&bob).state, view_of(&bob).status);
        v = view_of(&bob);
        CHECK(!strcmp(v.code, "H7K-3WM"), "the room is made again under the same code (%s)", v.code);
        exchange(&alice, &bob, drop, "after the restart", &first);
    }

    // --- Bob leaves, and comes back
    coop_sess_command(bob.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&bob).state == COOP_LEFT, 5000), "Bob left (state %d)", view_of(&bob).state);
    CHECK(WAIT_FOR(in_room(&alice, 1), ROSTER_WAIT_MS), "Alice sees the room with only her in it (%d members)", view_of(&alice).n);
    if (!drop) CHECK(alice.byes > 0, "Alice got Bob's BYE");
    coop_sess_command(bob.sess, COOP_CMD_JOIN, "h7k-3wm");   // a code as typed
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "Bob joined again");

    // --- the process's own session, with the settings the game reads
    probe_process(server, drop, &alice, &first);

    // --- a room that is not there, a server that is not there
    Player carol, dave;
    player_start(&carol, "Carol", 0xCA201, server, drop, 0, 0);
    coop_sess_command(carol.sess, COOP_CMD_JOIN, "BCDFGH");
    CHECK(WAIT_FOR(strstr(view_of(&carol).status, "no such room") != NULL, 30000), "Carol, joining a room that does not exist, is told so: %s",
          view_of(&carol).status);
    CHECK(view_of(&carol).state == COOP_LEFT, "and ends in no room (state %d)", view_of(&carol).state);
    player_end(&carol);
    uint16_t dead_port = free_port();
    char dead[40];
    snprintf(dead, sizeof dead, "127.0.0.1:%u", dead_port);
    player_start(&dave, "Dave", 0xDA7E, dead, 0, 0, 0);
    coop_sess_command(dave.sess, COOP_CMD_CREATE, "");
    plat_sleep_ms(2500);
    v = view_of(&dave);
    CHECK(v.state == COOP_CONNECTING && strstr(v.status, "no answer"), "Dave, with nobody at the server, keeps trying: state %d, %s", v.state, v.status);
    coop_sess_command(dave.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT, 5000), "and leaves when told to");
    coop_sess_configure(dave.sess, &(CoopParams){.server = "", .name = "Dave", .on = 1});
    coop_sess_command(dave.sess, COOP_CMD_CREATE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT && strstr(view_of(&dave).status, "no server"), 5000), "with no server configured a create says so: %s", view_of(&dave).status);
    player_end(&dave);

    // --- the end: LEAVE and BYE, state off
    player_end(&bob);
    CHECK(WAIT_FOR(in_room(&alice, 1), ROSTER_WAIT_MS), "Alice sees Bob go when his session ends");
    player_end(&alice);
    if (mock) mock_stop(mock);
}

int probe_coop(void) {
    probe_sockets();
    probe_hmac();
    probe_server_parse();
    probe_vectors();
    probe_fuzz();
    probe_sessions();
    printf("coop-probe: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
