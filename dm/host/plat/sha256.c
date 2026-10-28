// SHA-256 (FIPS 180-4) for the platforms without CommonCrypto (Linux, Windows): NRO hashes (hle_ro.c), DM_LOADHASH (loader.c). Apple keeps CommonCrypto (plat_apple.c)
#include "plat.h"
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t nbuf; } sha_t;
_Static_assert(sizeof(sha_t) <= sizeof(plat_sha256_ctx), "plat_sha256_ctx holds the state");

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }
static void block(sha_t *s, const uint8_t *p) {
    uint32_t w[64], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3, s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
void plat_sha256_init(plat_sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    sha_t *s = (sha_t *)c; memcpy(s->h, iv, sizeof iv); s->len = 0; s->nbuf = 0;
}
void plat_sha256_update(plat_sha256_ctx *c, const void *data, size_t n) {
    sha_t *s = (sha_t *)c; const uint8_t *p = data;
    s->len += n;
    if (s->nbuf) {
        size_t k = 64 - s->nbuf < n ? 64 - s->nbuf : n;
        memcpy(s->buf + s->nbuf, p, k); s->nbuf += (uint32_t)k; p += k; n -= k;
        if (s->nbuf < 64) return;
        block(s, s->buf); s->nbuf = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(s, p);
    memcpy(s->buf, p, n); s->nbuf = (uint32_t)n;
}
void plat_sha256_final(plat_sha256_ctx *c, uint8_t out[32]) {
    sha_t *s = (sha_t *)c; uint64_t bits = s->len * 8;
    s->buf[s->nbuf++] = 0x80;
    if (s->nbuf > 56) { memset(s->buf + s->nbuf, 0, 64 - s->nbuf); block(s, s->buf); s->nbuf = 0; }
    memset(s->buf + s->nbuf, 0, 56 - s->nbuf);
    for (int i = 0; i < 8; i++) s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    block(s, s->buf);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
void plat_sha256(const void *p, size_t n, uint8_t out[32]) { plat_sha256_ctx c; plat_sha256_init(&c); plat_sha256_update(&c, p, n); plat_sha256_final(&c, out); }
