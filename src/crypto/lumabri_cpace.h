/* lumabri_cpace.h — a balanced password-authenticated key exchange for the
 * short household pairing code.
 *
 * A pairing code has about 40 bits. Used as a key, a MAC key or a hash input
 * that travels over the network it could be searched offline in seconds.
 * Here it only selects the base point of a Diffie-Hellman exchange. Someone
 * who watches an exchange learns nothing to test guesses against; someone
 * who takes part in one can test exactly one guess, and the tracker counts
 * those (src/runtime/lumabri_pairing_registry.h).
 *
 * The construction is CPace over X25519 with SHA-512, written from the IRTF
 * CFRG draft "CPace, a balanced composable PAKE" (draft-irtf-cfrg-cpace):
 *
 *   g   = Elligator2( SHA-512( lv(DSI) | lv(PRS) | lv(zero padding) |
 *                              lv(CI) | lv(sid) )[0..31] )
 *   Ya  = X25519(ya, g)            Yb = X25519(yb, g)
 *   K   = X25519(ya, Yb) = X25519(yb, Ya), refused when all zero
 *   ISK = SHA-512( lv(DSI | "_ISK") | lv(sid) | lv(K) |
 *                  lv(Ya) | lv(ADa) | lv(Yb) | lv(ADb) )
 *
 * with DSI = "CPace255", lv(x) the LEB128 length of x followed by x, and the
 * zero padding sized so that the code fills the first SHA-512 block.
 *
 * NOT VERIFIED AGAINST THE SPECIFICATION. This file was written without
 * network access to the draft and has not been run against its published
 * test vectors, so it must not be described as conforming or interoperable.
 * It is tested against an independent big-integer model of the formulas
 * above (tests/integration/pairing_model_test.py), which shows that the C
 * code computes what this comment says, not that this comment matches the
 * draft byte for byte. See docs/PAIRING.md, "Open item".
 *
 * On top of that exchange, the pairing layer below fixes the inputs and adds
 * key confirmation in both directions and one sealed message.
 */
#ifndef LUMABRI_CPACE_H
#define LUMABRI_CPACE_H

#include "lumabri_crypto.h"
#include "src/crypto/lumabri_elligator2.h"
#include "src/crypto/lumabri_random.h"

#define LMB_CPACE_DSI        "CPace255"
#define LMB_CPACE_DSI_ISK    "CPace255_ISK"
#define LMB_CPACE_HASH_BLOCK 128u        /* SHA-512 input block, in bytes */
#define LMB_CPACE_PRS_MAX    100u

static LMB_MAYBE_UNUSED void lmb_cpace_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* Absorb lv(x): unsigned LEB128 length, then the bytes. */
static LMB_MAYBE_UNUSED void lmb_cpace_lv(LmbSha512 *h, const void *p, size_t n) {
    uint8_t len[10];
    size_t k = 0, v = n;
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        len[k++] = b;
    } while (v);
    lmb_sha512_update(h, len, k);
    if (n) lmb_sha512_update(h, p, n);
}

/* The secret base point for one exchange. Returns 0, or -1 when the password
 * string is longer than this implementation pads for. */
static LMB_MAYBE_UNUSED int lmb_cpace_generator(uint8_t g[32],
        const uint8_t *prs, size_t prs_len, const uint8_t *ci, size_t ci_len,
        const uint8_t *sid, size_t sid_len) {
    static const uint8_t zeros[LMB_CPACE_HASH_BLOCK] = {0};
    if (prs_len > LMB_CPACE_PRS_MAX) return -1;
    /* block - 1 - len(lv(PRS)) - len(lv(DSI)); both length prefixes are one byte */
    size_t pad = LMB_CPACE_HASH_BLOCK - 1 - (1 + prs_len) - (1 + sizeof(LMB_CPACE_DSI) - 1);
    uint8_t hash[64];
    LmbSha512 h;
    lmb_sha512_init(&h);
    lmb_cpace_lv(&h, LMB_CPACE_DSI, sizeof(LMB_CPACE_DSI) - 1);
    lmb_cpace_lv(&h, prs, prs_len);
    lmb_cpace_lv(&h, zeros, pad);
    lmb_cpace_lv(&h, ci, ci_len);
    lmb_cpace_lv(&h, sid, sid_len);
    lmb_sha512_final(&h, hash);
    lmb_elligator2(g, hash);                 /* the first 32 bytes, bit 255 ignored */
    lmb_cpace_wipe(&h, sizeof h);
    lmb_cpace_wipe(hash, sizeof hash);
    return 0;
}

/* The intermediate session key. `a` is the side that sends its value first. */
static LMB_MAYBE_UNUSED void lmb_cpace_isk(uint8_t isk[64],
        const uint8_t *sid, size_t sid_len, const uint8_t k[32],
        const uint8_t ya[32], const uint8_t *ada, size_t ada_len,
        const uint8_t yb[32], const uint8_t *adb, size_t adb_len) {
    LmbSha512 h;
    lmb_sha512_init(&h);
    lmb_cpace_lv(&h, LMB_CPACE_DSI_ISK, sizeof(LMB_CPACE_DSI_ISK) - 1);
    lmb_cpace_lv(&h, sid, sid_len);
    lmb_cpace_lv(&h, k, 32);
    lmb_cpace_lv(&h, ya, 32);
    lmb_cpace_lv(&h, ada, ada_len);
    lmb_cpace_lv(&h, yb, 32);
    lmb_cpace_lv(&h, adb, adb_len);
    lmb_sha512_final(&h, isk);
    lmb_cpace_wipe(&h, sizeof h);
}

/* ---- the household pairing exchange -------------------------------------
 *
 * Party a is the tracker (it sends its value first), party b the joiner.
 *
 *   PRS = the eight code symbols, upper case, no separator
 *   CI  = lv("lumabri-pair-v1") | lv(id_tracker) | lv(id_joiner)
 *   sid = tracker nonce[16] | joiner nonce[16]
 *   ADa = id_tracker, ADb = id_joiner
 *
 * id_tracker and id_joiner are the Ed25519 identities that signed the
 * encrypted connection the exchange runs inside. A relay in the middle shows
 * each side a different identity, so the two sides derive different base
 * points and nothing after that agrees.
 *
 *   th            = SHA-512( lv("lumabri-pair-v1 transcript") | lv(sid) |
 *                            lv(id_tracker) | lv(id_joiner) | lv(Yt) | lv(Yj) )
 *   key(label)    = HKDF-SHA512(salt = "lumabri-pair-v1", ikm = ISK,
 *                               info = label)[0..31]
 *   tag_joiner    = HMAC-SHA512(key("confirm joiner"),  th)[0..31]
 *   tag_tracker   = HMAC-SHA512(key("confirm tracker"), th)[0..31]
 *   sealed secret = ChaCha20-Poly1305(key("household key"), nonce = 0,
 *                                     aad = th, plaintext)
 *
 * The three keys have different labels, so a confirmation tag is never a
 * value under the key that protects the household key. The sealing key is
 * used for exactly one message, which is why a fixed nonce is safe.
 */
#define LMB_PAIR_LABEL    "lumabri-pair-v1"
#define LMB_PAIR_CODE_LEN 8
#define LMB_PAIR_SID_LEN  32
#define LMB_PAIR_TAG_LEN  32

typedef struct {
    int tracker;                          /* 1: party a, 0: party b */
    uint8_t scalar[32];
    uint8_t own[32];                      /* this side's public value */
    uint8_t sid[LMB_PAIR_SID_LEN];
    uint8_t id_tracker[32], id_joiner[32];
} LmbPairExchange;

typedef struct {
    uint8_t tag_tracker[LMB_PAIR_TAG_LEN];
    uint8_t tag_joiner[LMB_PAIR_TAG_LEN];
    uint8_t seal_key[32];
    uint8_t transcript[64];
} LmbPairKeys;

/* First step for either side. `scalar` is 32 fresh bytes from the system
 * random generator; it is copied and the caller should wipe its own copy.
 * x->own is the value to send. */
static LMB_MAYBE_UNUSED void lmb_pair_begin(LmbPairExchange *x, int tracker,
        const char code[LMB_PAIR_CODE_LEN],
        const uint8_t id_tracker[32], const uint8_t id_joiner[32],
        const uint8_t sid[LMB_PAIR_SID_LEN], const uint8_t scalar[32]) {
    uint8_t ci[3 + sizeof(LMB_PAIR_LABEL) - 1 + 64], g[32];
    size_t n = 0;
    memset(x, 0, sizeof *x);
    x->tracker = tracker ? 1 : 0;
    memcpy(x->scalar, scalar, 32);
    memcpy(x->sid, sid, LMB_PAIR_SID_LEN);
    memcpy(x->id_tracker, id_tracker, 32);
    memcpy(x->id_joiner, id_joiner, 32);
    ci[n++] = (uint8_t)(sizeof(LMB_PAIR_LABEL) - 1);
    memcpy(ci + n, LMB_PAIR_LABEL, sizeof(LMB_PAIR_LABEL) - 1); n += sizeof(LMB_PAIR_LABEL) - 1;
    ci[n++] = 32; memcpy(ci + n, id_tracker, 32); n += 32;
    ci[n++] = 32; memcpy(ci + n, id_joiner, 32); n += 32;
    (void)lmb_cpace_generator(g, (const uint8_t *)code, LMB_PAIR_CODE_LEN, ci, n,
                              sid, LMB_PAIR_SID_LEN);
    lc_x25519(x->own, x->scalar, g);
    /* The base point is as good as the code to anyone who can search 2^40. */
    lmb_cpace_wipe(g, sizeof g);
}

/* Second step: take the other side's value and derive every key. Returns 0,
 * or -1 when the shared point is the all-zero value a low-order input
 * produces. In that case the keys are replaced by random bytes, so a caller
 * that forgot to look at the result still cannot agree with anyone. The
 * exchange is finished either way and its secrets are wiped. */
static LMB_MAYBE_UNUSED int lmb_pair_finish(LmbPairExchange *x, const uint8_t peer[32],
                                            LmbPairKeys *keys) {
    static const char transcript_label[] = LMB_PAIR_LABEL " transcript";
    static const char *const labels[3] = { "confirm tracker", "confirm joiner", "household key" };
    uint8_t k[32], isk[64], derived[3][32], mac[64], any = 0;
    const uint8_t *yt = x->tracker ? x->own : peer, *yj = x->tracker ? peer : x->own;
    lc_x25519(k, x->scalar, peer);
    for (int i = 0; i < 32; i++) any |= k[i];
    lmb_cpace_isk(isk, x->sid, LMB_PAIR_SID_LEN, k, yt, x->id_tracker, 32, yj, x->id_joiner, 32);
    LmbSha512 h;
    lmb_sha512_init(&h);
    lmb_cpace_lv(&h, transcript_label, sizeof transcript_label - 1);
    lmb_cpace_lv(&h, x->sid, LMB_PAIR_SID_LEN);
    lmb_cpace_lv(&h, x->id_tracker, 32);
    lmb_cpace_lv(&h, x->id_joiner, 32);
    lmb_cpace_lv(&h, yt, 32);
    lmb_cpace_lv(&h, yj, 32);
    lmb_sha512_final(&h, keys->transcript);
    for (int i = 0; i < 3; i++)
        (void)lc_hkdf((const uint8_t *)LMB_PAIR_LABEL, sizeof(LMB_PAIR_LABEL) - 1, isk, sizeof isk,
                      (const uint8_t *)labels[i], strlen(labels[i]), derived[i], 32);
    lc_hmac_sha512(derived[0], 32, keys->transcript, 64, mac);
    memcpy(keys->tag_tracker, mac, LMB_PAIR_TAG_LEN);
    lc_hmac_sha512(derived[1], 32, keys->transcript, 64, mac);
    memcpy(keys->tag_joiner, mac, LMB_PAIR_TAG_LEN);
    memcpy(keys->seal_key, derived[2], 32);
    int low_order = !any;
    if (low_order) lmb_random(keys, sizeof *keys);
    lmb_cpace_wipe(k, sizeof k); lmb_cpace_wipe(isk, sizeof isk);
    lmb_cpace_wipe(derived, sizeof derived); lmb_cpace_wipe(mac, sizeof mac);
    lmb_cpace_wipe(&h, sizeof h);
    lmb_cpace_wipe(x->scalar, sizeof x->scalar);
    return low_order ? -1 : 0;
}

/* 1 when the two tags are equal. Constant time. */
static LMB_MAYBE_UNUSED int lmb_pair_tag_equal(const uint8_t a[LMB_PAIR_TAG_LEN],
                                               const uint8_t b[LMB_PAIR_TAG_LEN]) {
    return lmb_vrfy32(a, b) == 0;
}

/* The one message sent under the exchange: tracker to joiner. */
static LMB_MAYBE_UNUSED void lmb_pair_seal(const LmbPairKeys *keys, const uint8_t *plain,
                                           size_t n, uint8_t *sealed, uint8_t tag[16]) {
    static const uint8_t nonce[12] = {0};
    lc_aead_seal(keys->seal_key, nonce, keys->transcript, 64, plain, n, sealed, tag);
}

/* 0 and the plaintext, or -1 with `plain` untouched. */
static LMB_MAYBE_UNUSED int lmb_pair_open(const LmbPairKeys *keys, const uint8_t *sealed,
                                          size_t n, const uint8_t tag[16], uint8_t *plain) {
    static const uint8_t nonce[12] = {0};
    return lc_aead_open(keys->seal_key, nonce, keys->transcript, 64, sealed, n, tag, plain);
}

#endif /* LUMABRI_CPACE_H */
