/* lumabri_crypto.h — the transport crypto: ChaCha20-Poly1305 (RFC 8439),
 * X25519 (RFC 7748), and HKDF-SHA512. Self-contained C, no dependencies,
 * every primitive checked against its RFC test vectors by crypto_test.sh.
 *
 * X25519 reuses the Curve25519 field arithmetic already in lumabri_sign.h
 * (both come from the TweetNaCl lineage), so only the scalar-mult ladder is
 * new here. HKDF uses this file's HMAC over the SHA-512 in lumabri_sign.h.
 *
 * These are the primitives for an authenticated encrypted transport; the
 * handshake and framing that use them live in lumabri_secure.h.
 */
#ifndef LUMABRI_CRYPTO_H
#define LUMABRI_CRYPTO_H

#include <stdint.h>
#include <string.h>
#include "lumabri_sign.h"          /* SHA-512 + Curve25519 field ops */

/* ---- ChaCha20 (RFC 8439 §2.3) ------------------------------------------ */

static uint32_t lc_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define LC_QR(a,b,c,d) \
    a += b; d ^= a; d = lc_rotl(d,16); \
    c += d; b ^= c; b = lc_rotl(b,12); \
    a += b; d ^= a; d = lc_rotl(d, 8); \
    c += d; b ^= c; b = lc_rotl(b, 7)

static uint32_t lc_ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void lc_st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void lc_chacha_block(const uint8_t key[32], uint32_t counter,
                            const uint8_t nonce[12], uint8_t out[64]) {
    static const char sigma[16] = "expand 32-byte k";
    uint32_t s[16], x[16];
    s[0] = lc_ld32((const uint8_t *)sigma);
    s[1] = lc_ld32((const uint8_t *)sigma + 4);
    s[2] = lc_ld32((const uint8_t *)sigma + 8);
    s[3] = lc_ld32((const uint8_t *)sigma + 12);
    for (int i = 0; i < 8; i++) s[4 + i] = lc_ld32(key + 4 * i);
    s[12] = counter;
    s[13] = lc_ld32(nonce);
    s[14] = lc_ld32(nonce + 4);
    s[15] = lc_ld32(nonce + 8);
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        LC_QR(x[0], x[4], x[8],  x[12]);
        LC_QR(x[1], x[5], x[9],  x[13]);
        LC_QR(x[2], x[6], x[10], x[14]);
        LC_QR(x[3], x[7], x[11], x[15]);
        LC_QR(x[0], x[5], x[10], x[15]);
        LC_QR(x[1], x[6], x[11], x[12]);
        LC_QR(x[2], x[7], x[8],  x[13]);
        LC_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) lc_st32(out + 4 * i, x[i] + s[i]);
}

static void lc_chacha20(const uint8_t key[32], uint32_t counter,
                        const uint8_t nonce[12], const uint8_t *in,
                        uint8_t *out, size_t n) {
    uint8_t blk[64];
    size_t off = 0;
    while (off < n) {
        lc_chacha_block(key, counter++, nonce, blk);
        size_t m = n - off < 64 ? n - off : 64;
        for (size_t i = 0; i < m; i++) out[off + i] = in[off + i] ^ blk[i];
        off += m;
    }
}

/* ---- Poly1305 (RFC 8439 §2.5), 32-bit limbs ---------------------------- */

typedef struct {
    uint32_t r[5], h[5], pad[4];
    size_t leftover;
    uint8_t buffer[16];
    int final;
} lc_poly;

static void lc_poly_init(lc_poly *st, const uint8_t key[32]) {
    st->r[0] = (lc_ld32(key)        ) & 0x3ffffff;
    st->r[1] = (lc_ld32(key + 3) >> 2) & 0x3ffff03;
    st->r[2] = (lc_ld32(key + 6) >> 4) & 0x3ffc0ff;
    st->r[3] = (lc_ld32(key + 9) >> 6) & 0x3f03fff;
    st->r[4] = (lc_ld32(key + 12) >> 8) & 0x00fffff;
    st->h[0] = st->h[1] = st->h[2] = st->h[3] = st->h[4] = 0;
    st->pad[0] = lc_ld32(key + 16);
    st->pad[1] = lc_ld32(key + 20);
    st->pad[2] = lc_ld32(key + 24);
    st->pad[3] = lc_ld32(key + 28);
    st->leftover = 0;
    st->final = 0;
}

static void lc_poly_blocks(lc_poly *st, const uint8_t *m, size_t bytes) {
    const uint32_t hibit = st->final ? 0 : (1u << 24);
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    while (bytes >= 16) {
        uint64_t d0, d1, d2, d3, d4;
        uint32_t c;
        h0 += (lc_ld32(m)        ) & 0x3ffffff;
        h1 += (lc_ld32(m + 3) >> 2) & 0x3ffffff;
        h2 += (lc_ld32(m + 6) >> 4) & 0x3ffffff;
        h3 += (lc_ld32(m + 9) >> 6) & 0x3ffffff;
        h4 += (lc_ld32(m + 12) >> 8) | hibit;
        d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        m += 16; bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void lc_poly_update(lc_poly *st, const uint8_t *m, size_t bytes) {
    if (st->leftover) {
        size_t want = 16 - st->leftover;
        if (want > bytes) want = bytes;
        memcpy(st->buffer + st->leftover, m, want);
        bytes -= want; m += want; st->leftover += want;
        if (st->leftover < 16) return;
        lc_poly_blocks(st, st->buffer, 16);
        st->leftover = 0;
    }
    if (bytes >= 16) {
        size_t want = bytes & ~(size_t)15;
        lc_poly_blocks(st, m, want);
        m += want; bytes -= want;
    }
    if (bytes) { memcpy(st->buffer + st->leftover, m, bytes); st->leftover += bytes; }
}

static void lc_poly_finish(lc_poly *st, uint8_t mac[16]) {
    uint32_t h0, h1, h2, h3, h4, c, g0, g1, g2, g3, g4;
    uint64_t f;
    uint32_t mask;
    if (st->leftover) {
        size_t i = st->leftover;
        st->buffer[i++] = 1;
        for (; i < 16; i++) st->buffer[i] = 0;
        st->final = 1;
        lc_poly_blocks(st, st->buffer, 16);
    }
    h0 = st->h[0]; h1 = st->h[1]; h2 = st->h[2]; h3 = st->h[3]; h4 = st->h[4];
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c; c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c; c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);
    mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0        | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12)| (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18)| (h4 << 8))  & 0xffffffff;
    f = (uint64_t)h0 + st->pad[0];            h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;
    lc_st32(mac,      h0); lc_st32(mac + 4,  h1);
    lc_st32(mac + 8,  h2); lc_st32(mac + 12, h3);
}

/* constant-time 16-byte compare */
static int lc_ct_eq16(const uint8_t *a, const uint8_t *b) {
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* ---- ChaCha20-Poly1305 AEAD (RFC 8439 §2.8) ---------------------------- */

static void lc_poly_pad16(lc_poly *st, size_t len) {
    static const uint8_t z[16] = {0};
    if (len % 16) lc_poly_update(st, z, 16 - (len % 16));
}

static void lc_aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *pt, size_t pt_len,
                         uint8_t *ct, uint8_t tag[16]) {
    uint8_t polykey[64];
    lc_chacha_block(key, 0, nonce, polykey);
    lc_chacha20(key, 1, nonce, pt, ct, pt_len);
    lc_poly st;
    lc_poly_init(&st, polykey);
    if (aad_len) { lc_poly_update(&st, aad, aad_len); lc_poly_pad16(&st, aad_len); }
    lc_poly_update(&st, ct, pt_len); lc_poly_pad16(&st, pt_len);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i]     = (uint8_t)((uint64_t)aad_len >> (8 * i));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)((uint64_t)pt_len  >> (8 * i));
    lc_poly_update(&st, lens, 16);
    lc_poly_finish(&st, tag);
}

/* 0 on success (tag verified, pt written); -1 on tamper (pt left untouched). */
static int lc_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, size_t ct_len,
                        const uint8_t tag[16], uint8_t *pt) {
    uint8_t polykey[64], want[16];
    lc_chacha_block(key, 0, nonce, polykey);
    lc_poly st;
    lc_poly_init(&st, polykey);
    if (aad_len) { lc_poly_update(&st, aad, aad_len); lc_poly_pad16(&st, aad_len); }
    lc_poly_update(&st, ct, ct_len); lc_poly_pad16(&st, ct_len);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i]     = (uint8_t)((uint64_t)aad_len >> (8 * i));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)((uint64_t)ct_len  >> (8 * i));
    lc_poly_update(&st, lens, 16);
    lc_poly_finish(&st, want);
    if (!lc_ct_eq16(want, tag)) return -1;
    lc_chacha20(key, 1, nonce, ct, pt, ct_len);
    return 0;
}

/* ---- X25519 (RFC 7748), on lumabri_sign.h's field arithmetic ----------- */

static const lmb_gf lc_121665 = { 0xDB41, 1 };

static void lc_x25519(uint8_t out[32], const uint8_t scalar[32],
                      const uint8_t point[32]) {
    uint8_t z[32];
    lmb_gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    lmb_unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        lmb_sel(a, b, r); lmb_sel(c, d, r);
        lmb_A(e, a, c); lmb_Z(a, a, c);
        lmb_A(c, b, d); lmb_Z(b, b, d);
        lmb_S(d, e); lmb_S(f, a);
        lmb_M(a, c, a); lmb_M(c, b, e);
        lmb_A(e, a, c); lmb_Z(a, a, c);
        lmb_S(b, a); lmb_Z(c, d, f);
        lmb_M(a, c, lc_121665); lmb_A(a, a, d);
        lmb_M(c, c, a); lmb_M(a, d, f);
        lmb_M(d, b, x); lmb_S(b, e);
        lmb_sel(a, b, r); lmb_sel(c, d, r);
    }
    lmb_inv25519(c, c);
    lmb_M(a, a, c);
    lmb_pack25519(out, a);
}

/* public key from a secret scalar: X25519(scalar, 9) */
static void lc_x25519_base(uint8_t pub[32], const uint8_t scalar[32]) {
    static const uint8_t nine[32] = { 9 };
    lc_x25519(pub, scalar, nine);
}

/* ---- HMAC-SHA512 and HKDF (RFC 5869) ----------------------------------- */

static void lc_hmac_sha512(const uint8_t *key, size_t klen,
                           const uint8_t *msg, size_t mlen, uint8_t out[64]) {
    uint8_t k[128] = {0}, ip[128], op[128], inner[64];
    if (klen > 128) { lmb_sha512(key, klen, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 128; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    LmbSha512 s;
    lmb_sha512_init(&s); lmb_sha512_update(&s, ip, 128);
    lmb_sha512_update(&s, msg, mlen); lmb_sha512_final(&s, inner);
    lmb_sha512_init(&s); lmb_sha512_update(&s, op, 128);
    lmb_sha512_update(&s, inner, 64); lmb_sha512_final(&s, out);
}

/* HKDF-Extract+Expand to `n` bytes.  This transport only needs one SHA-512
 * block; reject larger requests and oversized info instead of truncating the
 * output or overflowing the fixed expand buffer. */
static int lc_hkdf(const uint8_t *salt, size_t slen,
                   const uint8_t *ikm, size_t ilen,
                   const uint8_t *info, size_t inflen,
                   uint8_t *out, size_t n) {
    uint8_t prk[64], t[64 + 64];
    if (n > 64 || inflen > sizeof(t) - 1) return -1;
    lc_hmac_sha512(salt, slen, ikm, ilen, prk);      /* extract */
    size_t tl = 0;
    memcpy(t + tl, info, inflen); tl += inflen;
    t[tl++] = 1;
    uint8_t okm[64];
    lc_hmac_sha512(prk, 64, t, tl, okm);             /* expand, block 1 */
    memcpy(out, okm, n);
    return 0;
}

#endif /* LUMABRI_CRYPTO_H */
