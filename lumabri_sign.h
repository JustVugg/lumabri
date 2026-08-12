/* lumabri_sign.h — SHA-512 and Ed25519, self-contained, no dependencies.
 *
 * Why this file exists: with hashes alone, the tracker IS the authority —
 * it decides which bytes are true, so a compromised tracker rewrites the
 * model. Signing moves the authority to a key the operator keeps offline:
 * the tracker becomes a courier that carries a signature it cannot forge,
 * and a chatter needs to trust only 32 bytes of public key it obtained
 * once, out of band.
 *
 * The Ed25519 implementation follows the TweetNaCl construction (public
 * domain, D. J. Bernstein et al.): 16-bit-limb field arithmetic over
 * 2^255-19, constant-time conditional swaps, no secret-dependent branches.
 * It is verified against OpenSSL in both directions by sign_test.sh —
 * signatures we make verify there, signatures made there verify here.
 */
#ifndef LUMABRI_SIGN_H
#define LUMABRI_SIGN_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* ---- SHA-512 (FIPS 180-4) ---------------------------------------------- */

typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; size_t off; } LmbSha512;

static const uint64_t lmb_k512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };

static void lmb_sha512_init(LmbSha512 *s) {
    static const uint64_t h0[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    memcpy(s->h, h0, sizeof h0);
    s->len = 0; s->off = 0;
}

static void lmb_sha512_block(LmbSha512 *s, const uint8_t *p) {
    uint64_t w[80], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[8 * i + j];
    }
    #define RR(x,n) (((x) >> (n)) | ((x) << (64 - (n))))
    for (i = 16; i < 80; i++) {
        uint64_t s0 = RR(w[i-15],1) ^ RR(w[i-15],8) ^ (w[i-15] >> 7);
        uint64_t s1 = RR(w[i-2],19) ^ RR(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
    e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (i = 0; i < 80; i++) {
        uint64_t S1 = RR(e,14) ^ RR(e,18) ^ RR(e,41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + lmb_k512[i] + w[i];
        uint64_t S0 = RR(a,28) ^ RR(a,34) ^ RR(a,39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    #undef RR
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void lmb_sha512_update(LmbSha512 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n) {
        size_t take = 128 - s->off;
        if (take > n) take = n;
        memcpy(s->buf + s->off, p, take);
        s->off += take; p += take; n -= take;
        if (s->off == 128) { lmb_sha512_block(s, s->buf); s->off = 0; }
    }
}

static void lmb_sha512_final(LmbSha512 *s, uint8_t out[64]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0;
    lmb_sha512_update(s, &pad, 1);
    while (s->off != 112) lmb_sha512_update(s, &z, 1);
    uint8_t l[16];
    memset(l, 0, 8);
    for (int i = 0; i < 8; i++) l[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    lmb_sha512_update(s, l, 16);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) out[8*i+j] = (uint8_t)(s->h[i] >> (56 - 8*j));
}

static void lmb_sha512(const void *data, size_t n, uint8_t out[64]) {
    LmbSha512 s;
    lmb_sha512_init(&s);
    lmb_sha512_update(&s, data, n);
    lmb_sha512_final(&s, out);
}

/* ---- Ed25519 ------------------------------------------------------------ */

typedef int64_t lmb_gf[16];

static const lmb_gf
  lmb_gf0,
  lmb_gf1 = {1},
  lmb_D  = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,
            0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203},
  lmb_D2 = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,
            0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406},
  lmb_X  = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,
            0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169},
  lmb_Y  = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,
            0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666},
  lmb_If = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,
            0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};

static const uint64_t lmb_L[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10 };

static void lmb_set(lmb_gf r, const lmb_gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }

static void lmb_car(lmb_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;          /* not c<<16: c may be negative (UB) */
    }
}

static void lmb_sel(lmb_gf p, lmb_gf q, int b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t; q[i] ^= t;
    }
}

static void lmb_pack25519(uint8_t *o, const lmb_gf n) {
    lmb_gf m, t;
    lmb_set(t, n);
    lmb_car(t); lmb_car(t); lmb_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        int i;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        lmb_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i]   = (uint8_t)(t[i] & 0xff);
        o[2*i+1] = (uint8_t)(t[i] >> 8);
    }
}

static int lmb_vrfy32(const uint8_t *x, const uint8_t *y) {
    uint32_t d = 0;
    for (int i = 0; i < 32; i++) d |= (uint32_t)(x[i] ^ y[i]);
    return (int)((1 & ((d - 1) >> 8)) - 1);      /* 0 if equal */
}

static int lmb_neq(const lmb_gf a, const lmb_gf b) {
    uint8_t c[32], d[32];
    lmb_pack25519(c, a); lmb_pack25519(d, b);
    return lmb_vrfy32(c, d);
}

static uint8_t lmb_par(const lmb_gf a) {
    uint8_t d[32];
    lmb_pack25519(d, a);
    return d[0] & 1;
}

static void lmb_unpack25519(lmb_gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void lmb_A(lmb_gf o, const lmb_gf a, const lmb_gf b) { for (int i=0;i<16;i++) o[i]=a[i]+b[i]; }
static void lmb_Z(lmb_gf o, const lmb_gf a, const lmb_gf b) { for (int i=0;i<16;i++) o[i]=a[i]-b[i]; }

static void lmb_M(lmb_gf o, const lmb_gf a, const lmb_gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    lmb_car(o); lmb_car(o);
}

static void lmb_S(lmb_gf o, const lmb_gf a) { lmb_M(o, a, a); }

static void lmb_inv25519(lmb_gf o, const lmb_gf i) {
    lmb_gf c;
    lmb_set(c, i);
    for (int a = 253; a >= 0; a--) {
        lmb_S(c, c);
        if (a != 2 && a != 4) lmb_M(c, c, i);
    }
    lmb_set(o, c);
}

static void lmb_pow2523(lmb_gf o, const lmb_gf i) {
    lmb_gf c;
    lmb_set(c, i);
    for (int a = 250; a >= 0; a--) {
        lmb_S(c, c);
        if (a != 1) lmb_M(c, c, i);
    }
    lmb_set(o, c);
}

static void lmb_add(lmb_gf p[4], lmb_gf q[4]) {
    lmb_gf a, b, c, d, t, e, f, g, h;
    lmb_Z(a, p[1], p[0]);
    lmb_Z(t, q[1], q[0]);
    lmb_M(a, a, t);
    lmb_A(b, p[0], p[1]);
    lmb_A(t, q[0], q[1]);
    lmb_M(b, b, t);
    lmb_M(c, p[3], q[3]);
    lmb_M(c, c, lmb_D2);
    lmb_M(d, p[2], q[2]);
    lmb_A(d, d, d);
    lmb_Z(e, b, a);
    lmb_Z(f, d, c);
    lmb_A(g, d, c);
    lmb_A(h, b, a);
    lmb_M(p[0], e, f);
    lmb_M(p[1], h, g);
    lmb_M(p[2], g, f);
    lmb_M(p[3], e, h);
}

static void lmb_cswap(lmb_gf p[4], lmb_gf q[4], uint8_t b) {
    for (int i = 0; i < 4; i++) lmb_sel(p[i], q[i], b);
}

static void lmb_pack(uint8_t *r, lmb_gf p[4]) {
    lmb_gf tx, ty, zi;
    lmb_inv25519(zi, p[2]);
    lmb_M(tx, p[0], zi);
    lmb_M(ty, p[1], zi);
    lmb_pack25519(r, ty);
    r[31] ^= lmb_par(tx) << 7;
}

static void lmb_scalarmult(lmb_gf p[4], lmb_gf q[4], const uint8_t *s) {
    lmb_set(p[0], lmb_gf0); lmb_set(p[1], lmb_gf1);
    lmb_set(p[2], lmb_gf1); lmb_set(p[3], lmb_gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (uint8_t)((s[i/8] >> (i & 7)) & 1);
        lmb_cswap(p, q, b);
        lmb_add(q, p);
        lmb_add(p, p);
        lmb_cswap(p, q, b);
    }
}

static void lmb_scalarbase(lmb_gf p[4], const uint8_t *s) {
    lmb_gf q[4];
    lmb_set(q[0], lmb_X); lmb_set(q[1], lmb_Y); lmb_set(q[2], lmb_gf1);
    lmb_M(q[3], lmb_X, lmb_Y);
    lmb_scalarmult(p, q, s);
}

static void lmb_modL(uint8_t *r, int64_t x[64]) {
    int64_t carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (int64_t)lmb_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;     /* not carry<<8: carry may be negative,
                                        and left-shifting a negative is UB */
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (int64_t)lmb_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * (int64_t)lmb_L[j];
    for (i = 0; i < 32; i++) {
        x[i+1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void lmb_reduce(uint8_t *r) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)(uint64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    lmb_modL(r, x);
}

static int lmb_unpackneg(lmb_gf r[4], const uint8_t p[32]) {
    lmb_gf t, chk, num, den, den2, den4, den6;
    lmb_set(r[2], lmb_gf1);
    lmb_unpack25519(r[1], p);
    lmb_S(num, r[1]);
    lmb_M(den, num, lmb_D);
    lmb_Z(num, num, r[2]);
    lmb_A(den, r[2], den);
    lmb_S(den2, den);
    lmb_S(den4, den2);
    lmb_M(den6, den4, den2);
    lmb_M(t, den6, num);
    lmb_M(t, t, den);
    lmb_pow2523(t, t);
    lmb_M(t, t, num);
    lmb_M(t, t, den);
    lmb_M(t, t, den);
    lmb_M(r[0], t, den);
    lmb_S(chk, r[0]);
    lmb_M(chk, chk, den);
    if (lmb_neq(chk, num)) lmb_M(r[0], r[0], lmb_If);
    lmb_S(chk, r[0]);
    lmb_M(chk, chk, den);
    if (lmb_neq(chk, num)) return -1;
    if (lmb_par(r[0]) == (p[31] >> 7)) lmb_Z(r[0], lmb_gf0, r[0]);
    lmb_M(r[3], r[0], r[1]);
    return 0;
}

/* ---- public API ---------------------------------------------------------
 * Keys are the standard Ed25519 shapes: 32-byte public key, 64-byte secret
 * key laid out as seed(32) || public(32) — the same convention OpenSSL,
 * libsodium and ssh-ed25519 use, so keys are portable. */

static void lmb_sign_keypair(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]) {
    uint8_t d[64];
    lmb_gf p[4];
    memcpy(sk, seed, 32);
    lmb_sha512(sk, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    lmb_scalarbase(p, d);
    lmb_pack(pk, p);
    memcpy(sk + 32, pk, 32);
}

/* detached signature over msg */
static void lmb_sign(uint8_t sig[64], const uint8_t *msg, size_t n,
                     const uint8_t sk[64]) {
    uint8_t d[64], h[64], r[64];
    int64_t x[64];
    lmb_gf p[4];
    LmbSha512 s;

    lmb_sha512(sk, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;

    lmb_sha512_init(&s);
    lmb_sha512_update(&s, d + 32, 32);
    lmb_sha512_update(&s, msg, n);
    lmb_sha512_final(&s, r);
    lmb_reduce(r);
    lmb_scalarbase(p, r);
    lmb_pack(sig, p);                      /* R */

    lmb_sha512_init(&s);
    lmb_sha512_update(&s, sig, 32);
    lmb_sha512_update(&s, sk + 32, 32);
    lmb_sha512_update(&s, msg, n);
    lmb_sha512_final(&s, h);
    lmb_reduce(h);

    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (int64_t)(uint64_t)r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i+j] += (int64_t)h[i] * (int64_t)d[j];
    lmb_modL(sig + 32, x);                 /* S */
}

/* 0 = the signature is genuine for this key and message */
static int lmb_sign_verify(const uint8_t sig[64], const uint8_t *msg, size_t n,
                           const uint8_t pk[32]) {
    uint8_t h[64], t[32];
    lmb_gf p[4], q[4];
    LmbSha512 s;

    if (sig[63] & 224) return -1;          /* S must be reduced */
    if (lmb_unpackneg(q, pk)) return -1;

    lmb_sha512_init(&s);
    lmb_sha512_update(&s, sig, 32);
    lmb_sha512_update(&s, pk, 32);
    lmb_sha512_update(&s, msg, n);
    lmb_sha512_final(&s, h);
    lmb_reduce(h);

    lmb_scalarmult(p, q, h);
    lmb_scalarbase(q, sig + 32);
    lmb_add(p, q);
    lmb_pack(t, p);
    return lmb_vrfy32(sig, t) ? -1 : 0;
}

/* ---- what a signature actually covers -----------------------------------
 * The canonical message binds the hashes to the file they describe: domain
 * tag, model, path, chunk size, file size, then the hash vector. Without
 * the binding a valid signature could be replayed onto another file of the
 * same swarm — the classic mistake. Domain tag included so a lumabri
 * signature can never be mistaken for a signature of anything else. */

#define LMB_TRUTH_TAG "lumabri-truth-v1"

static uint8_t *lmb_truth_msg(const char *model, const char *path,
                              uint32_t chunk, uint64_t size,
                              const uint8_t *hashes, uint32_t nh, size_t *out_len) {
    size_t ml = strlen(model), pl = strlen(path);
    size_t n = sizeof(LMB_TRUTH_TAG) + ml + 1 + pl + 1 + 4 + 8 + (size_t)nh * 32;
    uint8_t *m = (uint8_t *)malloc(n);
    if (!m) return NULL;
    size_t o = 0;
    memcpy(m + o, LMB_TRUTH_TAG, sizeof(LMB_TRUTH_TAG)); o += sizeof(LMB_TRUTH_TAG);
    memcpy(m + o, model, ml); o += ml; m[o++] = 0;
    memcpy(m + o, path, pl);  o += pl; m[o++] = 0;
    for (int i = 0; i < 4; i++) m[o++] = (uint8_t)(chunk >> (8 * i));
    for (int i = 0; i < 8; i++) m[o++] = (uint8_t)(size >> (8 * i));
    if (nh) { memcpy(m + o, hashes, (size_t)nh * 32); o += (size_t)nh * 32; }
    *out_len = o;
    return m;
}

/* A separate domain binds the signature to a complete-model root. It cannot
 * be replayed as a per-file truth signature, and the model name prevents a
 * valid root from being relabelled by a tracker. */
#define LMB_MODEL_ID_TAG "lumabri-model-id-v1"

static uint8_t *lmb_model_id_msg(const char *model, const uint8_t root[32],
                                 size_t *out_len) {
    size_t ml = strlen(model);
    if (ml > SIZE_MAX - sizeof(LMB_MODEL_ID_TAG) - 1 - 32) return NULL;
    size_t n = sizeof(LMB_MODEL_ID_TAG) + ml + 1 + 32;
    uint8_t *m = (uint8_t *)malloc(n);
    if (!m) return NULL;
    size_t o = 0;
    memcpy(m + o, LMB_MODEL_ID_TAG, sizeof(LMB_MODEL_ID_TAG));
    o += sizeof(LMB_MODEL_ID_TAG);
    memcpy(m + o, model, ml); o += ml; m[o++] = 0;
    memcpy(m + o, root, 32); o += 32;
    *out_len = o;
    return m;
}

/* ---- hex helpers (keys live in files as hex, easy to paste) ------------- */

static void lmb_hex(char *dst, const uint8_t *src, size_t n) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        dst[2*i]   = H[src[i] >> 4];
        dst[2*i+1] = H[src[i] & 15];
    }
    dst[2*n] = 0;
}

static int lmb_unhex(uint8_t *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int hi = -1, lo = -1;
        char a = src[2*i], b = src[2*i+1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return -1;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ---- trust sets / manual key rotation ---------------------------------
 *
 * A signed swarm may trust more than one operator key during a planned
 * rotation.  The wire format deliberately stays unchanged: each object is
 * signed by one key, while verifiers accept it if ANY key in this small,
 * out-of-band trust set validates it.  Rollout is therefore:
 *
 *   publish old+new keys -> start signing with new -> remove old key
 *
 * KMS/HSM policy, automatic revocation and audit belong above this primitive;
 * this is the dependency-free public floor needed to rotate safely by hand.
 */
#define LMB_MAX_TRUST_KEYS 16
typedef struct {
    uint8_t key[LMB_MAX_TRUST_KEYS][32];
    size_t n;
} LmbTrustKeys;

static int lmb_trust_add_hex(LmbTrustKeys *t, const char *hex) {
    if (!t || !hex || strlen(hex) != 64) return -1;
    uint8_t key[32];
    if (lmb_unhex(key, hex, sizeof key)) return -1;
    for (size_t i = 0; i < t->n; i++)
        if (!memcmp(t->key[i], key, sizeof key)) return 0;
    if (t->n >= LMB_MAX_TRUST_KEYS) return -1;
    memcpy(t->key[t->n++], key, sizeof key);
    return 0;
}

static int lmb_trust_match(const LmbTrustKeys *t, const uint8_t sig[64],
                           const uint8_t *msg, size_t n) {
    if (!t) return -1;
    for (size_t i = 0; i < t->n; i++)
        if (lmb_sign_verify(sig, msg, n, t->key[i]) == 0) return (int)i;
    return -1;
}

static int lmb_trust_verify(const LmbTrustKeys *t, const uint8_t sig[64],
                            const uint8_t *msg, size_t n) {
    return lmb_trust_match(t, sig, msg, n) >= 0 ? 0 : -1;
}

/* A keyring file contains one 64-hex public key per whitespace-separated
 * field.  A literal spec may contain the same keys separated by commas.
 * Calls append, so command-line programs may also repeat --pubkey. */
static int lmb_trust_load_stream(LmbTrustKeys *t, FILE *fp) {
    char word[256];
    LmbTrustKeys add = {0};
    while (t && fp && fscanf(fp, "%255s", word) == 1) {
        if (word[0] == '#') { int ch; while ((ch = fgetc(fp)) != '\n' && ch != EOF) {} continue; }
        if (lmb_trust_add_hex(&add, word)) return -1;
    }
    if (!t || !add.n) return -1;
    size_t before = t->n;
    for (size_t i = 0; i < add.n; i++) {
        char hex[65]; lmb_hex(hex, add.key[i], 32);
        if (lmb_trust_add_hex(t, hex)) { t->n = before; return -1; }
    }
    return 0;
}

static int lmb_trust_load_spec(LmbTrustKeys *t, const char *spec) {
    if (!t || !spec || !*spec) return -1;
    FILE *fp = fopen(spec, "r");
    if (fp) {
        int rc = lmb_trust_load_stream(t, fp);
        fclose(fp);
        return rc;
    }
    char *copy = strdup(spec);
    if (!copy) return -1;
    LmbTrustKeys add = {0};
    size_t before = t->n;
    char *save = NULL;
    for (char *p = strtok_r(copy, ",", &save); p; p = strtok_r(NULL, ",", &save))
        if (lmb_trust_add_hex(&add, p)) { free(copy); return -1; }
    free(copy);
    if (!add.n) return -1;
    for (size_t i = 0; i < add.n; i++) {
        char hex[65]; lmb_hex(hex, add.key[i], 32);
        if (lmb_trust_add_hex(t, hex)) { t->n = before; return -1; }
    }
    return 0;
}

/* ---- peer identity ------------------------------------------------------
 * A peer authenticates its own name to the tracker with an Ed25519 key that
 * is NOT the operator key: the operator key signs the model, this key only
 * says "this name is mine". The tracker binds a name to the first key that
 * claims it and refuses any later claim under a different key, so a stranger
 * can no longer register under an honest peer's name and evict it.
 */

/* 32 random bytes, or abort: a weak nonce or seed is worse than none. */
static void lmb_random(void *buf, size_t n) {
    FILE *ur = fopen("/dev/urandom", "rb");
    if (!ur || fread(buf, 1, n, ur) != n) {
        fprintf(stderr, "[lumabri] cannot read %zu random bytes from "
                        "/dev/urandom\n", n);
        if (ur) fclose(ur);
        abort();
    }
    fclose(ur);
}

/* Load this machine's peer key from `path`, or create it on first use (0600).
 * One key per machine: every role it runs shares the identity, and each of
 * their names is bound to it. Returns 0, or -1 if the file is unreadable and
 * cannot be created. */
static int lmb_peer_identity(const char *path, uint8_t sk[64], uint8_t pk[32]) {
    /* Two roles on one machine (a serve's maintainer and expert node) start
     * together and both may find no key. Exclusive-create on the real path so
     * exactly one writes it; the loser re-reads the winner's file instead of
     * writing a second, different key that would fail its own name after a
     * restart. A brief spin covers the window where the file exists but the
     * winner has not finished writing it. */
    for (int attempt = 0; attempt < 200; attempt++) {
        char hex[200] = "";
        FILE *f = fopen(path, "r");
        if (f) {
            int got = fscanf(f, "%198s", hex) == 1;
            fclose(f);
            if (got && strlen(hex) == 128 && !lmb_unhex(sk, hex, 64)) {
                memcpy(pk, sk + 32, 32);
                return 0;
            }
            usleep(3000);        /* created but not yet written: wait and retry */
            continue;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) { usleep(3000); continue; }
            return -1;
        }
        uint8_t seed[32];
        lmb_random(seed, sizeof seed);
        lmb_sign_keypair(pk, sk, seed);
        char out[130];
        lmb_hex(out, sk, 64);
        out[128] = '\n'; out[129] = 0;
        int wrote = (int)write(fd, out, 129) == 129;
        if (fsync(fd)) wrote = 0;
        close(fd);
        if (!wrote) { unlink(path); return -1; }
        return 0;
    }
    return -1;
}

/* This machine's default peer-key path: $LUMABRI_PEER_KEY, else
 * $HOME/.lumabri/peer.key, else ./.lumabri_peer.key. */
static const char *lmb_peer_key_path(char *buf, size_t cap) {
    const char *e = getenv("LUMABRI_PEER_KEY");
    if (e && e[0]) { snprintf(buf, cap, "%s", e); return buf; }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[400];
        snprintf(dir, sizeof dir, "%s/.lumabri", home);
        mkdir(dir, 0700);
        snprintf(buf, cap, "%s/peer.key", dir);
    } else snprintf(buf, cap, "./.lumabri_peer.key");
    return buf;
}

/* The exact bytes both sides sign for a registration: a domain tag so this
 * signature can never be replayed as any other, the tracker's per-connection
 * nonce so it cannot be replayed on another connection, then the identity
 * this REGISTER claims. Built in one place so the tracker and the peer cannot
 * disagree by a byte. Returns the length, or 0 if it would not fit. */
#define LMB_PEER_AUTH_TAG "lumabri-peer-auth-v1"
static size_t lmb_peer_auth_msg(const uint8_t nonce[32], const char *name,
                                const char *model, const char *addr,
                                uint8_t *out, size_t cap) {
    size_t nl = strlen(name), ml = strlen(model), al = strlen(addr);
    size_t need = sizeof(LMB_PEER_AUTH_TAG) + 32 + nl + 1 + ml + 1 + al + 1;
    if (need > cap) return 0;
    size_t o = 0;
    memcpy(out + o, LMB_PEER_AUTH_TAG, sizeof(LMB_PEER_AUTH_TAG));
    o += sizeof(LMB_PEER_AUTH_TAG);
    memcpy(out + o, nonce, 32); o += 32;
    memcpy(out + o, name, nl);  o += nl; out[o++] = 0;
    memcpy(out + o, model, ml); o += ml; out[o++] = 0;
    memcpy(out + o, addr, al);  o += al; out[o++] = 0;
    return o;
}

#endif /* LUMABRI_SIGN_H */
