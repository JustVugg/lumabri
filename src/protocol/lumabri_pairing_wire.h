/* Household pairing with a short code: the code itself and the fixed-size
 * bodies of the LMB_PAIR_* frames. The exchange they carry is specified in
 * src/crypto/lumabri_cpace.h and described in docs/PAIRING.md.
 *
 * Every body has one exact length for body version 1. A peer that does not
 * know the operation answers ERR ("unknown op") and closes; a peer that
 * knows it but not the version answers ERR too. Nothing is guessed. */
#ifndef LUMABRI_PAIRING_WIRE_H
#define LUMABRI_PAIRING_WIRE_H
#include "lumabri_proto.h"
#include "src/crypto/lumabri_cpace.h"

#define LMB_PAIR_VERSION        1u
#define LMB_PAIR_NONCE_LEN      16
#define LMB_PAIR_ID_LEN         16
#define LMB_PAIR_LIFETIME_S     300u   /* default and upper bound */
#define LMB_PAIR_MAX_ATTEMPTS   5u

/* joiner -> tracker, before AUTH: version, joiner nonce */
#define LMB_PAIR_HELLO_LEN      (4 + LMB_PAIR_NONCE_LEN)
/* tracker -> joiner: version, tracker nonce, tracker public value */
#define LMB_PAIR_HELLO_R_LEN    (4 + LMB_PAIR_NONCE_LEN + 32)
/* joiner -> tracker: joiner public value, joiner confirmation tag */
#define LMB_PAIR_CONFIRM_LEN    (32 + LMB_PAIR_TAG_LEN)
/* The sealed secret: version, key length, key padded to its maximum, tracker
 * identity. Fixed size, so its length says nothing about the key. */
#define LMB_PAIR_SECRET_LEN     (2 + LMB_TOKEN_MAX + 32)
/* tracker -> joiner: tracker confirmation tag, sealed secret, AEAD tag */
#define LMB_PAIR_DONE_LEN       (LMB_PAIR_TAG_LEN + LMB_PAIR_SECRET_LEN + 16)
/* owner -> tracker, authenticated: version, requested lifetime (0: default) */
#define LMB_PAIR_OPEN_LEN       8
/* tracker -> owner: version, handle, code, lifetime, attempt limit */
#define LMB_PAIR_OPEN_R_LEN     (4 + LMB_PAIR_ID_LEN + LMB_PAIR_CODE_LEN + 8)
/* owner -> tracker: version, handle (STATUS and CANCEL) */
#define LMB_PAIR_HANDLE_LEN     (4 + LMB_PAIR_ID_LEN)
/* tracker -> owner: version, state, seconds left, failed attempts, attempt
 * limit, identity of the computer that joined, its source address */
#define LMB_PAIR_SOURCE_LEN     64
#define LMB_PAIR_STATUS_R_LEN   (20 + 32 + LMB_PAIR_SOURCE_LEN)

enum {
    LMB_PAIR_UNKNOWN = 0,      /* no code with this handle */
    LMB_PAIR_WAITING = 1,      /* live: not used yet, not expired */
    LMB_PAIR_USED = 2,         /* one computer joined with it */
    LMB_PAIR_EXPIRED = 3,
    LMB_PAIR_LOCKED = 4,       /* destroyed after too many failed attempts */
    LMB_PAIR_CANCELLED = 5,    /* withdrawn by the owner */
};

typedef struct {
    uint8_t id[LMB_PAIR_ID_LEN];
    char code[LMB_PAIR_CODE_LEN + 1];
    uint32_t lifetime_s, max_attempts;
} LmbPairOpened;

typedef struct {
    uint32_t state, remaining_s, failed, max_attempts;
    uint8_t joiner[32];
    char source[LMB_PAIR_SOURCE_LEN];
} LmbPairStatus;

/* ---- the code ------------------------------------------------------------
 * Eight symbols of Crockford's base 32: digits and upper-case letters without
 * I, L, O and U, so nothing on screen can be read two ways. 32 symbols are
 * exactly five bits each, so forty random bits map onto a code with no
 * rejection and no modulo bias. */
static const char lmb_pair_alphabet[33] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static LMB_MAYBE_UNUSED void lmb_pair_code_from_bits(char code[LMB_PAIR_CODE_LEN + 1],
                                                     const uint8_t bits[5]) {
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | bits[i];
    for (int i = 0; i < LMB_PAIR_CODE_LEN; i++)
        code[i] = lmb_pair_alphabet[(v >> (35 - 5 * i)) & 31];
    code[LMB_PAIR_CODE_LEN] = 0;
    v = 0;
}

static LMB_MAYBE_UNUSED void lmb_pair_code_new(char code[LMB_PAIR_CODE_LEN + 1]) {
    uint8_t bits[5];
    lmb_random(bits, sizeof bits);
    lmb_pair_code_from_bits(code, bits);
    lmb_cpace_wipe(bits, sizeof bits);
}

/* What a person typed -> the eight canonical symbols. Spaces and hyphens are
 * ignored, case is folded, and the letters the alphabet leaves out are read
 * as the digits they resemble (O as 0, I and L as 1). Returns 0, or -1 when
 * the text cannot be a code; nothing is sent to anyone in that case. */
static LMB_MAYBE_UNUSED int lmb_pair_code_normalize(char out[LMB_PAIR_CODE_LEN + 1],
                                                    const char *typed) {
    size_t n = 0;
    for (const char *p = typed; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (!memchr(lmb_pair_alphabet, c, 32) || n == LMB_PAIR_CODE_LEN) {
            lmb_cpace_wipe(out, LMB_PAIR_CODE_LEN + 1); return -1;
        }
        out[n++] = c;
    }
    if (n != LMB_PAIR_CODE_LEN) { lmb_cpace_wipe(out, LMB_PAIR_CODE_LEN + 1); return -1; }
    out[n] = 0;
    return 0;
}

/* "ABCD-EFGH", for people. */
static LMB_MAYBE_UNUSED void lmb_pair_code_format(char out[LMB_PAIR_CODE_LEN + 2],
                                                  const char code[LMB_PAIR_CODE_LEN + 1]) {
    memcpy(out, code, 4); out[4] = '-'; memcpy(out + 5, code + 4, 4); out[9] = 0;
}

/* ---- the sealed secret --------------------------------------------------- */

/* A household key that fits the frame and one line of the settings file. */
static LMB_MAYBE_UNUSED int lmb_pair_key_ok(const char *key, size_t n) {
    if (!n || n > LMB_TOKEN_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)key[i] < 0x20 || (unsigned char)key[i] > 0x7e) return 0;
    return 1;
}

static LMB_MAYBE_UNUSED int lmb_pair_secret_encode(uint8_t out[LMB_PAIR_SECRET_LEN],
        const char *household_key, const uint8_t tracker_identity[32]) {
    size_t n = strlen(household_key);
    if (!lmb_pair_key_ok(household_key, n)) return -1;
    memset(out, 0, LMB_PAIR_SECRET_LEN);
    out[0] = (uint8_t)LMB_PAIR_VERSION; out[1] = (uint8_t)n;
    memcpy(out + 2, household_key, n);
    memcpy(out + 2 + LMB_TOKEN_MAX, tracker_identity, 32);
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_pair_secret_decode(const uint8_t in[LMB_PAIR_SECRET_LEN],
        char household_key[LMB_TOKEN_MAX + 1], uint8_t tracker_identity[32]) {
    size_t n = in[1];
    if (in[0] != LMB_PAIR_VERSION || !lmb_pair_key_ok((const char *)in + 2, n)) return -1;
    for (size_t i = n; i < LMB_TOKEN_MAX; i++) if (in[2 + i]) return -1;
    memcpy(household_key, in + 2, n); household_key[n] = 0;
    memcpy(tracker_identity, in + 2 + LMB_TOKEN_MAX, 32);
    return 0;
}

#endif /* LUMABRI_PAIRING_WIRE_H */
