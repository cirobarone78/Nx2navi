// The co-op wire format, as pure functions on bytes: no sockets, no threads, no game state. A datagram is a 24-byte
// header (little-endian) and a payload; the header carries an 8-byte tag, the start of an HMAC-SHA256 over the first 16
// header bytes and the payload, keyed from the room code. The first part of the protocol is between a client and the
// relay (tools/coop_relay.py, the same format in Python), the second is between the clients, carried as the payload of
// a DATA datagram that the relay forwards unread: one kind byte and bytes whose meaning the layers above coop.c define.
// tests/coop_vectors.txt holds byte vectors that this file and the relay's Python are checked against (--probe-coop,
// tests/coop_relay_test.py). Every parser checks every length against the bytes it was given and fails closed.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    COOP_MAGIC = 0x434E,         // bytes 4E 43
    COOP_PROTO = 1,              // protocol major: a relay and a client talk only when it is equal
    COOP_HDR_LEN = 24,
    COOP_MAX_DGRAM = 1200,       // a datagram's size at most: it crosses any path without fragmenting
    COOP_MAX_PAYLOAD = COOP_MAX_DGRAM - COOP_HDR_LEN,
    COOP_TAG_LEN = 8,
    COOP_KEY_LEN = 16,
    COOP_DST_ALL = 0xFF,         // dst: every other member of the room
    COOP_CODE_LEN = 6,
    COOP_MAX_MEMBERS = 8,
    COOP_NAME_MAX = 24,          // a player's name in bytes of UTF-8
    COOP_COOKIE_LEN = 16,
    COOP_CONNECT_MIN = 64,       // a CONNECT datagram's smallest size, so that no answer to it is larger
};

// Datagram types. CONNECT .. LEAVE are between a client and the relay, DATA carries the peers' messages.
enum { COOP_T_CONNECT = 1, COOP_T_CHALLENGE = 2, COOP_T_JOINED = 3, COOP_T_ERROR = 5, COOP_T_ROSTER = 6, COOP_T_PING = 7,
       COOP_T_PONG = 8, COOP_T_LEAVE = 9, COOP_T_DATA = 0x10 };
enum { COOP_INTENT_CREATE = 1, COOP_INTENT_JOIN = 2 };   // CONNECT: make a room (the relay picks the code) or join one
enum { COOP_E_NO_ROOM = 1, COOP_E_ROOM_FULL = 2, COOP_E_PROTO = 3, COOP_E_RATE = 4, COOP_E_BAD_TAG = 5, COOP_E_SERVER_FULL = 6 };
enum { COOP_FLAG_AWAY = 1 };   // roster flag: the relay has not heard from the member for a while

// The kinds of peer messages (the first byte of a DATA payload). coop.c only carries them; HELLO, POS, DIGEST, SECTION,
// SYNC_NEED and NOTICE belong to the layers that send them. A kind of 0xF0 and above is free for tests.
enum { COOP_K_HELLO = 1, COOP_K_POS = 2, COOP_K_DIGEST = 3, COOP_K_SECTION = 4, COOP_K_SYNC_NEED = 5, COOP_K_NOTICE = 6,
       COOP_K_BYE = 7, COOP_K_TEST = 0xF0 };

typedef struct {
    uint8_t ver, type;
    uint8_t src, dst;       // member ids 1..8; src 0 is the relay; dst COOP_DST_ALL: every other member
    uint16_t len;           // payload bytes
    uint32_t room, seq;     // room 0 until joined; seq counts the sender's datagrams
} CoopHdr;

// ---- room codes: 6 characters of "23456789BCDFGHJKMNPRSTVWXYZ" (27 symbols, about 28.5 bits; no vowels, 0/1/O/I/L, Q
// or E, so a code spoken or typed does not read as a word), shown as XXX-XXX.
// Any ASCII case, '-' and spaces ignored. 1 and the canonical 6 characters plus a NUL in out when text is a code.
int coop_code_normalize(const char *text, char out[COOP_CODE_LEN + 1]);
void coop_code_format(const char code[COOP_CODE_LEN], char out[COOP_CODE_LEN + 2]);   // "XXX-XXX" and a NUL

// ---- tags. HMAC-SHA256 with a key of any length; the room key is SHA256("nx2navi-coop/1" + code)[0:16] of the
// canonical code. The tag of a datagram: HMAC(room key, header[0:16] + payload)[0:8].
void coop_hmac_sha256(const void *key, size_t klen, const void *a, size_t an, const void *b, size_t bn, uint8_t out[32]);
void coop_room_key(const char code[COOP_CODE_LEN], uint8_t key[COOP_KEY_LEN]);

// ---- datagrams
// The datagram of hdr (its len is ignored: n is the payload length) and payload into out. key NULL: a zero tag (a CONNECT
// without a code yet, and what the relay answers to it). Bytes written, 0 when it does not fit cap or n is too long.
size_t coop_dgram_pack(uint8_t *out, size_t cap, const CoopHdr *hdr, const void *payload, size_t n,
                       const uint8_t key[COOP_KEY_LEN]);
// 1 when d is exactly one datagram of this format (magic, a length that matches); *payload points into d. The version
// and the tag are not checked here.
int coop_dgram_parse(const uint8_t *d, size_t n, CoopHdr *hdr, const uint8_t **payload);
// 1 when the tag of the parsed datagram d (n bytes) is the one key makes; time independent of where they differ.
int coop_dgram_verify(const uint8_t *d, size_t n, const uint8_t key[COOP_KEY_LEN]);
int coop_dgram_tag_zero(const uint8_t *d);

// ---- names: a player's name from untrusted bytes. At most COOP_NAME_MAX bytes of valid UTF-8; a control character and
// each byte that is not valid UTF-8 become '?'. out is NUL-terminated (cap at least COOP_NAME_MAX + 1); the length.
size_t coop_name_clean(char *out, size_t cap, const uint8_t *raw, size_t n);

// ---- client and relay messages
typedef struct {
    uint8_t intent, flags;
    char code[COOP_CODE_LEN + 1];   // the room, canonical; "" for none (a create: the relay picks it)
    uint64_t client_id;
    char name[COOP_NAME_MAX + 1];
    uint8_t cookie[COOP_COOKIE_LEN];   // zero in the first CONNECT; the relay's CHALLENGE hands out the cookie
} CoopConnect;
size_t coop_connect_pack(uint8_t *out, size_t cap, const CoopConnect *c);   // zero-padded to COOP_CONNECT_MIN as a datagram
int coop_connect_parse(const uint8_t *p, size_t n, CoopConnect *c);

typedef struct { uint8_t id, flags; char name[COOP_NAME_MAX + 1]; } CoopMemberInfo;
typedef struct { uint8_t n; CoopMemberInfo m[COOP_MAX_MEMBERS]; } CoopRoster;
size_t coop_roster_pack(uint8_t *out, size_t cap, const CoopRoster *r);
// used (may be NULL): the bytes the roster took, JOINED carries one after its first fields.
int coop_roster_parse(const uint8_t *p, size_t n, CoopRoster *r, size_t *used);

typedef struct { uint8_t member; char code[COOP_CODE_LEN + 1]; CoopRoster roster; } CoopJoined;
size_t coop_joined_pack(uint8_t *out, size_t cap, const CoopJoined *j);
int coop_joined_parse(const uint8_t *p, size_t n, CoopJoined *j);

typedef struct { uint8_t reason; char text[61]; } CoopError;   // reason: COOP_E_*; text for the log, at most 60 bytes
size_t coop_error_pack(uint8_t *out, size_t cap, const CoopError *e);
int coop_error_parse(const uint8_t *p, size_t n, CoopError *e);

size_t coop_u32_pack(uint8_t *out, size_t cap, uint32_t v);   // PING and PONG: the token
int coop_u32_parse(const uint8_t *p, size_t n, uint32_t *v);

// ---- peer messages: the payload of a DATA datagram is one kind byte and the message
size_t coop_peer_pack(uint8_t *out, size_t cap, uint8_t kind, const void *bytes, size_t n);
int coop_peer_parse(const uint8_t *p, size_t n, uint8_t *kind, const uint8_t **bytes, size_t *nbytes);
