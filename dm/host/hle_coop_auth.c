// coop auth (MIT, our own code): internet play's room code and message authentication (docs/coop-step1d.md). SHA-256 as FIPS 180-4 §6.2 specifies
// it, HMAC-SHA256 as RFC 2104 / FIPS 198-1, HKDF as RFC 5869; the unit tests check all three against the published vectors (FIPS 180-4's examples,
// RFC 4231, RFC 5869). Integrity and authentication only: nothing here encrypts (the user's decision: a vetted library such as Monocypher would).
// - Room code: 16 symbols of Crockford's base32 (0-9 and A-Z without I, L, O, U), 80 bits from the system's random source, shown XXXX-XXXX-XXXX-XXXX.
// - Keys: prk = HKDF-Extract("pokopia-coop room v1", the 16 symbols); k_auth = Expand(prk, "auth"); room id = Expand(prk, "relay room id", 16), the
//   only thing a relay sees (it cannot get the code or k_auth back from it). A link's keys: HKDF over k_auth with both nonces as the salt.
#include "coop_int.h"

// ---------- SHA-256 ----------
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int t = 0; t < 16; t++) w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = ROR(w[t - 15], 7) ^ ROR(w[t - 15], 18) ^ w[t - 15] >> 3, s1 = ROR(w[t - 2], 17) ^ ROR(w[t - 2], 19) ^ w[t - 2] >> 10;
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[t] + w[t];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
void csha_init(csha *s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0); s->len = 0; s->n = 0;
}
void csha_update(csha *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    if (s->n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k); s->n += k; p += k; n -= k;
        if (s->n < 64) return;
        sha_block(s->h, s->buf); s->n = 0;
    }
    for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
    if (n) { memcpy(s->buf, p, n); s->n = n; }
}
void csha_final(csha *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = {0x80};
    size_t padn = (s->n < 56 ? 56 : 120) - s->n;
    for (int i = 0; i < 8; i++) pad[padn + i] = (uint8_t)(bits >> (56 - 8 * i));
    csha_update(s, pad, padn + 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
void csha256(const void *p, size_t n, uint8_t out[32]) { csha s; csha_init(&s); csha_update(&s, p, n); csha_final(&s, out); }

// ---------- HMAC-SHA256: the key's inner and outer pads are hashed once (chmac_key), each message then costs its own blocks plus two ----------
void chmac_key(chmac *k, const void *key, size_t n) {
    uint8_t kb[64] = {0}, pad[64];
    if (n > 64) csha256(key, n, kb); else if (n) memcpy(kb, key, n);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    csha_init(&k->in); csha_update(&k->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
    csha_init(&k->out); csha_update(&k->out, pad, 64);
    cwipe(kb, sizeof kb); cwipe(pad, sizeof pad);
}
void chmac_start(const chmac *k, csha *s) { *s = k->in; }
void chmac_end(const chmac *k, csha *s, uint8_t out[32]) {
    uint8_t ih[32]; csha_final(s, ih);
    csha o = k->out; csha_update(&o, ih, 32); csha_final(&o, out);
}
void chmac_once(const void *key, size_t kn, const void *msg, size_t mn, uint8_t out[32]) {
    chmac k; csha s; chmac_key(&k, key, kn); chmac_start(&k, &s); csha_update(&s, msg, mn); chmac_end(&k, &s, out); cwipe(&k, sizeof k);
}

// ---------- HKDF-SHA256 (RFC 5869) ----------
void chkdf_extract(const void *salt, size_t sn, const void *ikm, size_t in, uint8_t prk[32]) {
    static const uint8_t zero[32];
    if (!salt || !sn) { salt = zero; sn = 32; }
    chmac_once(salt, sn, ikm, in, prk);
}
int chkdf_expand(const uint8_t prk[32], const void *info, size_t infon, uint8_t *out, size_t outn) {
    if (outn > 255 * 32) return -1;
    chmac k; chmac_key(&k, prk, 32);
    uint8_t t[32]; size_t tn = 0;
    for (uint8_t i = 1; outn; i++) {
        csha s; chmac_start(&k, &s);
        csha_update(&s, t, tn); if (infon) csha_update(&s, info, infon); csha_update(&s, &i, 1);
        chmac_end(&k, &s, t); tn = 32;
        size_t m = outn < 32 ? outn : 32;
        memcpy(out, t, m); out += m; outn -= m;
    }
    cwipe(&k, sizeof k); cwipe(t, sizeof t);
    return 0;
}

int cct_eq(const void *a, const void *b, size_t n) {   // constant time in n: no early exit on the first difference
    const volatile uint8_t *x = a, *y = b; uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}
void cwipe(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }

// ---------- the room code ----------
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford's base32
int coop_room_normalize(const char *in, char out[COOP_ROOM_LEN + 1]) {
    int k = 0;
    if (!in) return 0;
    for (; *in; in++) {
        char ch = *in;
        if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch == 'O') ch = '0';
        else if (ch == 'I' || ch == 'L') ch = '1';   // Crockford's reading of the look-alikes
        if (!strchr(B32, ch) || !ch || k == COOP_ROOM_LEN) return 0;
        out[k++] = ch;
    }
    out[k] = 0;
    return k == COOP_ROOM_LEN;
}
void coop_room_new(char out[COOP_ROOM_LEN + 1]) {
    uint8_t r[COOP_ROOM_LEN];
    cplat_random(r, sizeof r);
    for (int i = 0; i < COOP_ROOM_LEN; i++) out[i] = B32[r[i] & 31];   // 256 is a multiple of 32: no bias
    out[COOP_ROOM_LEN] = 0;
    cwipe(r, sizeof r);
}
void coop_room_format(const char *norm, char out[COOP_ROOM_SHOWN]) {
    int k = 0;
    for (int i = 0; i < COOP_ROOM_LEN && norm[i]; i++) { if (i && i % 4 == 0) out[k++] = '-'; out[k++] = norm[i]; }
    out[k] = 0;
}
void croom_keys(const char *norm, uint8_t k_auth[32], uint8_t room_id[16]) {
    static const char salt[] = "pokopia-coop room v1";
    uint8_t prk[32];
    chkdf_extract(salt, sizeof salt - 1, norm, strlen(norm), prk);
    chkdf_expand(prk, "auth", 4, k_auth, 32);
    chkdf_expand(prk, "relay room id", 13, room_id, 16);
    cwipe(prk, sizeof prk);
}

// ---------- a link's handshake and keys ----------
// The station proves the code first and the host answers only after checking it, so a stranger who dials a host learns nothing it could guess
// the code from offline. Both proofs bind both nonces; the station's also binds its HELLO (platform and Uid)
void cauth_proof(const uint8_t k_auth[32], int host, const uint8_t ns[16], const uint8_t nh[16], const uint8_t *hello, size_t hn, uint8_t out[32]) {
    static const char ls[] = "PKP3 station proof", lh[] = "PKP3 host proof";
    chmac k; csha s; chmac_key(&k, k_auth, 32); chmac_start(&k, &s);
    if (host) csha_update(&s, lh, sizeof lh - 1); else csha_update(&s, ls, sizeof ls - 1);
    csha_update(&s, ns, 16); csha_update(&s, nh, 16);
    if (hn) csha_update(&s, hello, hn);
    chmac_end(&k, &s, out);
    cwipe(&k, sizeof k);
}
void cauth_link_keys(conn *c, const uint8_t k_auth[32], const uint8_t ns[16], const uint8_t nh[16], int host) {
    uint8_t salt[32], prk[32], k[32];
    memcpy(salt, ns, 16); memcpy(salt + 16, nh, 16);
    chkdf_extract(salt, 32, k_auth, 32, prk);
    // each direction and each path its own key: a frame cannot be reflected back or replayed as a datagram
    chkdf_expand(prk, host ? "tcp h2s" : "tcp s2h", 7, k, 32); chmac_key(&c->ktx, k, 32);
    chkdf_expand(prk, host ? "tcp s2h" : "tcp h2s", 7, k, 32); chmac_key(&c->krx, k, 32);
    chkdf_expand(prk, host ? "udp h2s" : "udp s2h", 7, k, 32); chmac_key(&c->kutx, k, 32);
    chkdf_expand(prk, host ? "udp s2h" : "udp h2s", 7, k, 32); chmac_key(&c->kurx, k, 32);
    c->seq_tx = c->seq_rx = 0; c->useq_tx = 0; c->useq_max = 0; c->uwin = 0;
    c->keyed = 1;
    cwipe(prk, sizeof prk); cwipe(k, sizeof k);
}
// A TCP frame's tag: the first 16 bytes of HMAC(key, seq || length || type || body). seq is each direction's frame count since the keys were made,
// known to both ends and never sent, so a frame dropped, repeated or moved fails the next check
void cauth_frame_tag(const chmac *k, uint64_t seq, const uint8_t *hdr5, const uint8_t *body, size_t n, uint8_t tag[CAUTH_TAG]) {
    uint8_t sq[8], t[32]; csha s;
    put64(sq, seq);
    chmac_start(k, &s); csha_update(&s, sq, 8); csha_update(&s, hdr5, 5); if (n) csha_update(&s, body, n); chmac_end(k, &s, t);
    memcpy(tag, t, CAUTH_TAG);
}
// A datagram's tag: over its whole header (with its sequence number) and body
void cauth_udp_tag(const chmac *k, const uint8_t *pkt, size_t n, uint8_t tag[CAUTH_TAG]) {
    uint8_t t[32]; csha s;
    chmac_start(k, &s); csha_update(&s, pkt, n); chmac_end(k, &s, t);
    memcpy(tag, t, CAUTH_TAG);
}
// Datagrams arrive unordered: a 64-packet window below the highest sequence number seen; a number seen before or older than the window is refused
int cauth_replay_ok(uint64_t *max, uint64_t *win, uint64_t seq) {
    if (!seq) return 0;
    if (seq > *max) { uint64_t d = seq - *max; *win = d >= 64 ? 1 : (*win << d) | 1; *max = seq; return 1; }
    uint64_t d = *max - seq;
    if (d >= 64 || (*win >> d & 1)) return 0;
    *win |= 1ull << d;
    return 1;
}
