/* lumabri_sha.h — SHA-256, FIPS 180-4, self-contained. No dependencies:
 * the whole trust story of the open swarm rests on 32 bytes per MiB, so the
 * function computing them lives here where it can be read in one sitting. */
#ifndef LUMABRI_SHA_H
#define LUMABRI_SHA_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef LMB_MAYBE_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define LMB_MAYBE_UNUSED __attribute__((unused))
#else
#define LMB_MAYBE_UNUSED
#endif
#endif

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t off; } LmbSha;

static const uint32_t lmb_sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static void lmb_sha_init(LmbSha *s) {
    static const uint32_t h0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0);
    s->len = 0; s->off = 0;
}

static void lmb_sha_block(LmbSha *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
               (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    #define R(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = R(w[i-15],7) ^ R(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = R(w[i-2],17) ^ R(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
    e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = R(e,6) ^ R(e,11) ^ R(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + lmb_sha_k[i] + w[i];
        uint32_t s0 = R(a,2) ^ R(a,13) ^ R(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    #undef R
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void lmb_sha_update(LmbSha *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->off;
        if (take > n) take = n;
        memcpy(s->buf + s->off, p, take);
        s->off += take; p += take; n -= take;
        if (s->off == 64) { lmb_sha_block(s, s->buf); s->off = 0; }
    }
}

static void lmb_sha_final(LmbSha *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    lmb_sha_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->off != 56) lmb_sha_update(s, &z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    lmb_sha_update(s, l, 8);
    for (int i = 0; i < 8; i++) {
        out[4*i]   = (uint8_t)(s->h[i] >> 24);
        out[4*i+1] = (uint8_t)(s->h[i] >> 16);
        out[4*i+2] = (uint8_t)(s->h[i] >> 8);
        out[4*i+3] = (uint8_t)(s->h[i]);
    }
}

static LMB_MAYBE_UNUSED void lmb_sha256(const void *data, size_t n,
                                        uint8_t out[32]) {
    LmbSha s;
    lmb_sha_init(&s);
    lmb_sha_update(&s, data, n);
    lmb_sha_final(&s, out);
}

/* Canonical identity of a complete model. Paths are sorted here, rather than
 * trusting readdir or registration order, and every field has an explicit
 * little-endian encoding. The root commits to the inventory as well as all
 * per-MiB hashes, so replacing, adding or removing a file changes it. */
typedef struct {
    const char *path;
    uint64_t size;
    uint32_t nh;
    const uint8_t *hashes;
} LmbModelItem;

static int lmb_model_item_cmp(const void *a, const void *b) {
    const LmbModelItem *const *aa = (const LmbModelItem *const *)a;
    const LmbModelItem *const *bb = (const LmbModelItem *const *)b;
    return strcmp((*aa)->path, (*bb)->path);
}

static void lmb_sha_le32(LmbSha *s, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    lmb_sha_update(s, b, sizeof b);
}

static void lmb_sha_le64(LmbSha *s, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    lmb_sha_update(s, b, sizeof b);
}

static LMB_MAYBE_UNUSED int lmb_model_root(const char *model,
                                           const LmbModelItem *items, size_t n,
                                           uint8_t out[32]) {
    static const char tag[] = "lumabri-model-root-v1";
    if (!model || (!items && n) || n > UINT32_MAX) return -1;
    const LmbModelItem **ord = (const LmbModelItem **)malloc((n ? n : 1) * sizeof *ord);
    if (!ord) return -1;
    for (size_t i = 0; i < n; i++) {
        if (!items[i].path || (items[i].nh && !items[i].hashes)) { free(ord); return -1; }
        ord[i] = &items[i];
    }
    qsort(ord, n, sizeof *ord, lmb_model_item_cmp);
    LmbSha s;
    lmb_sha_init(&s);
    lmb_sha_update(&s, tag, sizeof tag);
    size_t ml = strlen(model);
    if (ml > UINT32_MAX) { free(ord); return -1; }
    lmb_sha_le32(&s, (uint32_t)ml);
    lmb_sha_update(&s, model, ml);
    lmb_sha_le32(&s, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        const LmbModelItem *it = ord[i];
        size_t pl = strlen(it->path);
        /* nh is a uint32_t, so nh * 32 cannot overflow size_t on the
         * supported 64-bit targets.  Keep the path conversion explicit. */
        if (pl > UINT32_MAX) { free(ord); return -1; }
        lmb_sha_le32(&s, (uint32_t)pl);
        lmb_sha_update(&s, it->path, pl);
        lmb_sha_le64(&s, it->size);
        lmb_sha_le32(&s, it->nh);
        lmb_sha_update(&s, it->hashes, (size_t)it->nh * 32);
    }
    free(ord);
    lmb_sha_final(&s, out);
    return 0;
}

#endif /* LUMABRI_SHA_H */
