// The co-op wire format (coop_proto.h): room codes, HMAC tags, the datagram header, the client and relay messages and
// the peer message envelope. Pure functions on bytes.
#include "coop_proto.h"

#include <string.h>

#include "sha256.h"

static const char code_alphabet[] = "23456789BCDFGHJKMNPRSTVWXYZ";

static int code_char(char c) { return c && strchr(code_alphabet, c) != NULL; }

// The length of the string at s, at most max.
static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int coop_code_normalize(const char *text, char out[COOP_CODE_LEN + 1]) {
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (n == COOP_CODE_LEN || !code_char(c)) return 0;
        out[n++] = c;
    }
    if (n != COOP_CODE_LEN) return 0;
    out[n] = 0;
    return 1;
}

void coop_code_format(const char code[COOP_CODE_LEN], char out[COOP_CODE_LEN + 2]) {
    memcpy(out, code, 3);
    out[3] = '-';
    memcpy(out + 4, code + 3, 3);
    out[7] = 0;
}

// ---- tags

static void sha_two(Sha256 *s, const void *a, size_t an, const void *b, size_t bn) {
    if (an) sha256_update(s, a, an);
    if (bn) sha256_update(s, b, bn);
}

void coop_hmac_sha256(const void *key, size_t klen, const void *a, size_t an, const void *b, size_t bn, uint8_t out[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    Sha256 s;
    if (klen > sizeof k) sha256(key, klen, k);
    else if (klen) memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s);
    sha256_update(&s, pad, sizeof pad);
    sha_two(&s, a, an, b, bn);
    sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s);
    sha256_update(&s, pad, sizeof pad);
    sha256_update(&s, inner, sizeof inner);
    sha256_final(&s, out);
}

void coop_room_key(const char code[COOP_CODE_LEN], uint8_t key[COOP_KEY_LEN]) {
    uint8_t full[32];
    Sha256 s;
    sha256_init(&s);
    sha_two(&s, "nx2navi-coop/1", 14, code, COOP_CODE_LEN);
    sha256_final(&s, full);
    memcpy(key, full, COOP_KEY_LEN);
}

static void make_tag(const uint8_t key[COOP_KEY_LEN], const uint8_t *hdr16, const uint8_t *payload, size_t n,
                     uint8_t tag[COOP_TAG_LEN]) {
    uint8_t full[32];
    coop_hmac_sha256(key, COOP_KEY_LEN, hdr16, 16, payload, n, full);
    memcpy(tag, full, COOP_TAG_LEN);
}

// ---- little-endian fields

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

// ---- datagrams

size_t coop_dgram_pack(uint8_t *out, size_t cap, const CoopHdr *h, const void *payload, size_t n,
                       const uint8_t key[COOP_KEY_LEN]) {
    if (n > COOP_MAX_PAYLOAD || cap < COOP_HDR_LEN + n) return 0;
    put16(out, COOP_MAGIC);
    out[2] = h->ver, out[3] = h->type;
    put32(out + 4, h->room);
    out[8] = h->src, out[9] = h->dst;
    put16(out + 10, (uint32_t)n);
    put32(out + 12, h->seq);
    if (n) memmove(out + COOP_HDR_LEN, payload, n);   // the payload may already sit where it goes
    if (key) make_tag(key, out, out + COOP_HDR_LEN, n, out + 16);
    else memset(out + 16, 0, COOP_TAG_LEN);
    return COOP_HDR_LEN + n;
}

int coop_dgram_parse(const uint8_t *d, size_t n, CoopHdr *h, const uint8_t **payload) {
    if (n < COOP_HDR_LEN || n > COOP_MAX_DGRAM || get16(d) != COOP_MAGIC) return 0;
    uint32_t len = get16(d + 10);
    if (len != n - COOP_HDR_LEN) return 0;
    h->ver = d[2], h->type = d[3];
    h->room = get32(d + 4);
    h->src = d[8], h->dst = d[9];
    h->len = (uint16_t)len;
    h->seq = get32(d + 12);
    *payload = d + COOP_HDR_LEN;
    return 1;
}

int coop_dgram_verify(const uint8_t *d, size_t n, const uint8_t key[COOP_KEY_LEN]) {
    uint8_t tag[COOP_TAG_LEN], diff = 0;
    if (n < COOP_HDR_LEN) return 0;
    make_tag(key, d, d + COOP_HDR_LEN, n - COOP_HDR_LEN, tag);
    for (int i = 0; i < COOP_TAG_LEN; i++) diff |= (uint8_t)(tag[i] ^ d[16 + i]);
    return diff == 0;
}

int coop_dgram_tag_zero(const uint8_t *d) {
    uint8_t any = 0;
    for (int i = 0; i < COOP_TAG_LEN; i++) any |= d[16 + i];
    return any == 0;
}

// ---- names

// The length of the valid UTF-8 character at p (n bytes left), 0 when the bytes there are not one: no overlong forms,
// no surrogates, nothing above U+10FFFF.
static size_t utf8_len(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!len || len > n) return 0;
    for (size_t i = 1; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;    // overlong
    if (c == 0xED && p[1] > 0x9F) return 0;    // a surrogate
    if (c == 0xF0 && p[1] < 0x90) return 0;    // overlong
    if (c == 0xF4 && p[1] > 0x8F) return 0;    // above U+10FFFF
    return len;
}

// At most max bytes of raw as valid UTF-8 into out (cap bytes, NUL-terminated): a control character and each byte that is
// not valid UTF-8 become '?', and the text is cut before the character that would not fit.
static size_t text_clean(char *out, size_t cap, const uint8_t *raw, size_t n, size_t max) {
    size_t at = 0, i = 0;
    if (!cap) return 0;
    while (i < n) {
        size_t len = utf8_len(raw + i, n - i);
        if (len == 1 && (raw[i] < 0x20 || raw[i] == 0x7F)) len = 0;
        size_t take = len ? len : 1;
        if (at + take > max || at + take + 1 > cap) break;
        if (len) memcpy(out + at, raw + i, len);
        else out[at] = '?';
        at += take;
        i += take;
    }
    out[at] = 0;
    return at;
}

size_t coop_name_clean(char *out, size_t cap, const uint8_t *raw, size_t n) {
    return text_clean(out, cap, raw, n, COOP_NAME_MAX);
}

// ---- client and relay messages

size_t coop_connect_pack(uint8_t *out, size_t cap, const CoopConnect *c) {
    size_t nlen = bounded_len(c->name, COOP_NAME_MAX);
    size_t len = 17 + nlen + COOP_COOKIE_LEN;
    size_t padded = len < COOP_CONNECT_MIN - COOP_HDR_LEN ? COOP_CONNECT_MIN - COOP_HDR_LEN : len;
    if (cap < padded) return 0;
    memset(out, 0, padded);
    out[0] = c->intent, out[1] = c->flags;
    if (c->code[0]) memcpy(out + 2, c->code, COOP_CODE_LEN);
    put64(out + 8, c->client_id);
    out[16] = (uint8_t)nlen;
    memcpy(out + 17, c->name, nlen);
    memcpy(out + 17 + nlen, c->cookie, COOP_COOKIE_LEN);
    return padded;
}

int coop_connect_parse(const uint8_t *p, size_t n, CoopConnect *c) {
    if (n < 17) return 0;
    size_t nlen = p[16];
    if (nlen > COOP_NAME_MAX || 17 + nlen + COOP_COOKIE_LEN > n) return 0;
    memset(c, 0, sizeof *c);
    c->intent = p[0], c->flags = p[1];
    static const uint8_t zero[COOP_CODE_LEN];
    if (memcmp(p + 2, zero, COOP_CODE_LEN)) {
        char raw[COOP_CODE_LEN + 1];
        memcpy(raw, p + 2, COOP_CODE_LEN);
        raw[COOP_CODE_LEN] = 0;
        if (!coop_code_normalize(raw, c->code) || memcmp(raw, c->code, COOP_CODE_LEN)) return 0;   // canonical only
    }
    c->client_id = get64(p + 8);
    coop_name_clean(c->name, sizeof c->name, p + 17, nlen);
    memcpy(c->cookie, p + 17 + nlen, COOP_COOKIE_LEN);
    return 1;
}

size_t coop_roster_pack(uint8_t *out, size_t cap, const CoopRoster *r) {
    size_t at = 1;
    if (!cap || r->n > COOP_MAX_MEMBERS) return 0;
    out[0] = r->n;
    for (int i = 0; i < r->n; i++) {
        size_t nlen = bounded_len(r->m[i].name, COOP_NAME_MAX);
        if (at + 3 + nlen > cap) return 0;
        out[at] = r->m[i].id, out[at + 1] = r->m[i].flags, out[at + 2] = (uint8_t)nlen;
        memcpy(out + at + 3, r->m[i].name, nlen);
        at += 3 + nlen;
    }
    return at;
}

int coop_roster_parse(const uint8_t *p, size_t n, CoopRoster *r, size_t *used) {
    if (n < 1 || p[0] > COOP_MAX_MEMBERS) return 0;
    size_t at = 1;
    memset(r, 0, sizeof *r);
    r->n = p[0];
    for (int i = 0; i < r->n; i++) {
        if (at + 3 > n) return 0;
        size_t nlen = p[at + 2];
        if (nlen > COOP_NAME_MAX || at + 3 + nlen > n) return 0;
        r->m[i].id = p[at], r->m[i].flags = p[at + 1];
        coop_name_clean(r->m[i].name, sizeof r->m[i].name, p + at + 3, nlen);
        at += 3 + nlen;
    }
    if (used) *used = at;
    return 1;
}

size_t coop_joined_pack(uint8_t *out, size_t cap, const CoopJoined *j) {
    if (cap < 1 + COOP_CODE_LEN) return 0;
    out[0] = j->member;
    memcpy(out + 1, j->code, COOP_CODE_LEN);
    size_t k = coop_roster_pack(out + 1 + COOP_CODE_LEN, cap - 1 - COOP_CODE_LEN, &j->roster);
    return k ? 1 + COOP_CODE_LEN + k : 0;
}

int coop_joined_parse(const uint8_t *p, size_t n, CoopJoined *j) {
    if (n < 1 + COOP_CODE_LEN) return 0;
    char raw[COOP_CODE_LEN + 1];
    memcpy(raw, p + 1, COOP_CODE_LEN);
    raw[COOP_CODE_LEN] = 0;
    memset(j, 0, sizeof *j);
    if (!coop_code_normalize(raw, j->code) || memcmp(raw, j->code, COOP_CODE_LEN)) return 0;
    j->member = p[0];
    return coop_roster_parse(p + 1 + COOP_CODE_LEN, n - 1 - COOP_CODE_LEN, &j->roster, NULL);
}

size_t coop_error_pack(uint8_t *out, size_t cap, const CoopError *e) {
    size_t tlen = bounded_len(e->text, sizeof e->text - 1);
    if (cap < 2 + tlen) return 0;
    out[0] = e->reason, out[1] = (uint8_t)tlen;
    memcpy(out + 2, e->text, tlen);
    return 2 + tlen;
}

int coop_error_parse(const uint8_t *p, size_t n, CoopError *e) {
    if (n < 2 || 2u + p[1] > n) return 0;
    memset(e, 0, sizeof *e);
    e->reason = p[0];
    text_clean(e->text, sizeof e->text, p + 2, p[1], sizeof e->text - 1);   // for the log: printable UTF-8 only
    return 1;
}

size_t coop_u32_pack(uint8_t *out, size_t cap, uint32_t v) {
    if (cap < 4) return 0;
    put32(out, v);
    return 4;
}

int coop_u32_parse(const uint8_t *p, size_t n, uint32_t *v) {
    if (n < 4) return 0;
    *v = get32(p);
    return 1;
}

// ---- peer messages

size_t coop_peer_pack(uint8_t *out, size_t cap, uint8_t kind, const void *bytes, size_t n) {
    if (n + 1 > COOP_MAX_PAYLOAD || cap < n + 1) return 0;
    if (n) memmove(out + 1, bytes, n);
    out[0] = kind;
    return n + 1;
}

int coop_peer_parse(const uint8_t *p, size_t n, uint8_t *kind, const uint8_t **bytes, size_t *nbytes) {
    if (n < 1) return 0;
    *kind = p[0], *bytes = p + 1, *nbytes = n - 1;
    return 1;
}
