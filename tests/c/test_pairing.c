/* Pairing with a short code: field additions, Elligator 2, the code alphabet,
 * the exchange C against C, and the tracker's limits under a test clock.
 *
 * With arguments the same binary prints single results, so that
 * tests/integration/pairing_model_test.py can compare them with its own
 * big-integer model:
 *   test_pairing map R                                   Elligator 2
 *   test_pairing begin ROLE CODE IDT IDJ SID SCALAR      public value
 *   test_pairing finish ROLE CODE IDT IDJ SID SCALAR PEER [KEY | SEALED]
 * None of this is a conformance test of the CPace draft: see docs/PAIRING.md.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/runtime/lumabri_pairing_registry.h"

static const uint8_t prime[32] = {
    0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f };

/* u-coordinates of every point of order 1, 2, 4 or 8, and their aliases
 * above the prime: the values X25519 maps to zero. */
static const char *const low_order[] = {
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
};

static void print_hex(const char *label, const uint8_t *p, size_t n) {
    printf("%s ", label);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    putchar('\n');
}
static int parse_hex(uint8_t *out, size_t n, const char *hex) {
    return strlen(hex) != 2 * n || lmb_unhex(out, hex, n);
}

/* ---- field additions ------------------------------------------------------ */
static void random_fe(lmb_gf out) {
    uint8_t bytes[32];
    lmb_random(bytes, sizeof bytes);
    lmb_unpack25519(out, bytes);
}

static void test_field(void) {
    lmb_gf zero, one, two, minus_one, p, a, b, c;
    uint8_t packed[32];
    lmb_set(zero, lmb_gf0); lmb_set(one, lmb_gf1);
    lmb_A(two, one, one); lmb_Z(minus_one, zero, one);
    lmb_unpack25519(p, prime);

    assert(lmb_fe_is_zero(zero) == 1 && lmb_fe_is_zero(one) == 0 && lmb_fe_is_zero(minus_one) == 0);
    assert(lmb_fe_is_zero(p) == 1);                       /* p is zero modulo p */
    lmb_A(a, minus_one, one); assert(lmb_fe_is_zero(a) == 1);
    lmb_A(a, p, p); lmb_A(a, a, one); lmb_Z(a, a, one); assert(lmb_fe_is_zero(a) == 1);

    /* 2 is not a square modulo 2^255-19 and -1 is: p = 5 (mod 8). */
    assert(lmb_fe_is_square(zero) == 1 && lmb_fe_is_square(one) == 1);
    assert(lmb_fe_is_square(two) == 0 && lmb_fe_is_square(minus_one) == 1);
    lmb_fe_legendre(a, zero); assert(lmb_fe_is_zero(a));
    lmb_fe_legendre(a, two); assert(!lmb_neq(a, minus_one));
    lmb_fe_legendre(a, minus_one); assert(!lmb_neq(a, one));

    for (int i = 0; i < 300; i++) {
        random_fe(a);
        if (lmb_fe_is_zero(a)) continue;
        /* Every square is recognised, in any partly reduced form. */
        lmb_S(b, a); assert(lmb_fe_is_square(b) == 1);
        lmb_A(c, b, p); assert(lmb_fe_is_square(c) == 1);
        /* Multiplying by the non-square 2 flips the answer, so exactly one
         * of a and 2a is a square: the test cannot pass by always saying 1. */
        lmb_M(b, a, two);
        assert(lmb_fe_is_square(a) + lmb_fe_is_square(b) == 1);
        lmb_fe_legendre(c, a);
        assert(!lmb_neq(c, one) || !lmb_neq(c, minus_one));

        random_fe(b);
        lmb_fe_select(c, a, b, 1); assert(!lmb_neq(c, a));
        lmb_fe_select(c, a, b, 0); assert(!lmb_neq(c, b));
        lmb_fe_select(a, a, b, 0); assert(!lmb_neq(a, b));   /* aliasing */
    }
    lmb_pack25519(packed, p);
    for (int i = 0; i < 32; i++) assert(!packed[i]);
    puts("  ok   field: zero test, square test, selection");
}

/* ---- Elligator 2 ---------------------------------------------------------- */
/* A v with v^2 = u^3 + 486662 u^2 + u, found by the usual square root for
 * p = 5 (mod 8): the point is exhibited, not merely declared to exist. */
static int on_curve(const uint8_t u_bytes[32]) {
    static const lmb_gf A = { 0x6d06, 7 };
    lmb_gf u, rhs, t, root, check;
    lmb_unpack25519(u, u_bytes);
    lmb_A(t, u, A); lmb_M(t, t, u); lmb_A(t, t, lmb_gf1); lmb_M(rhs, t, u);
    lmb_pow2523(root, rhs); lmb_M(root, root, rhs);          /* rhs^((p+3)/8) */
    lmb_S(check, root);
    if (!lmb_neq(check, rhs)) return 1;
    lmb_M(root, root, lmb_If);                               /* times sqrt(-1) */
    lmb_S(check, root);
    return !lmb_neq(check, rhs);
}

static int canonical(const uint8_t u[32]) {
    for (int i = 31; i >= 0; i--) {
        if (u[i] < prime[i]) return 1;
        if (u[i] > prime[i]) return 0;
    }
    return 0;
}

static void test_elligator(void) {
    uint8_t r[32], u[32], other[32];
    /* Fixed inputs first: zero, one, the prime's neighbours, all ones. */
    for (int fixed = 0; fixed < 6; fixed++) {
        memset(r, 0, sizeof r);
        if (fixed == 1) r[0] = 1;
        if (fixed == 2) { memcpy(r, prime, 32); r[0]--; }
        if (fixed == 3) memcpy(r, prime, 32);
        if (fixed == 4) { memcpy(r, prime, 32); r[0]++; }
        if (fixed == 5) memset(r, 0xff, sizeof r);
        lmb_elligator2(u, r);
        assert(on_curve(u) && canonical(u));
    }
    /* r = 0 and r = p are the same field element. */
    memset(r, 0, sizeof r); lmb_elligator2(u, r);
    lmb_elligator2(other, prime); assert(!memcmp(u, other, 32));
    for (int i = 0; i < 2000; i++) {
        lmb_random(r, sizeof r);
        lmb_elligator2(u, r);
        assert(on_curve(u) && canonical(u));
        /* Bit 255 is not part of a field element. */
        memcpy(other, r, 32); other[31] ^= 0x80;
        lmb_elligator2(other, other); assert(!memcmp(u, other, 32));
        /* The map depends on r only through r^2: -r gives the same point. */
        lmb_gf e, negated;
        lmb_unpack25519(e, r); lmb_Z(negated, lmb_gf0, e); lmb_pack25519(other, negated);
        lmb_elligator2(other, other); assert(!memcmp(u, other, 32));
    }
    puts("  ok   Elligator 2: output is a canonical u-coordinate of a curve point");
}

/* ---- the code ------------------------------------------------------------- */
static void test_code(void) {
    char code[LMB_PAIR_CODE_LEN + 1], back[LMB_PAIR_CODE_LEN + 1], shown[LMB_PAIR_CODE_LEN + 2];
    const uint8_t zeros[5] = {0}, ones[5] = {0xff,0xff,0xff,0xff,0xff};
    const uint8_t counting[5] = {0x00,0x44,0x32,0x14,0xc7};   /* five-bit groups 0..7 */
    assert(strlen(lmb_pair_alphabet) == 32);
    for (int i = 0; i < 32; i++) {
        assert(!strchr("ILOU", lmb_pair_alphabet[i]));
        for (int j = 0; j < i; j++) assert(lmb_pair_alphabet[i] != lmb_pair_alphabet[j]);
    }
    lmb_pair_code_from_bits(code, zeros); assert(!strcmp(code, "00000000"));
    lmb_pair_code_from_bits(code, ones); assert(!strcmp(code, "ZZZZZZZZ"));
    lmb_pair_code_from_bits(code, counting); assert(!strcmp(code, "01234567"));
    /* Forty bits in, forty bits out: every symbol carries exactly five. */
    unsigned seen[LMB_PAIR_CODE_LEN][32];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < 6400; i++) {
        uint8_t bits[5], decoded[5]; uint64_t v = 0;
        lmb_random(bits, sizeof bits);
        lmb_pair_code_from_bits(code, bits);
        for (int k = 0; k < LMB_PAIR_CODE_LEN; k++) {
            const char *at = memchr(lmb_pair_alphabet, code[k], 32);
            assert(at);
            v = (v << 5) | (uint64_t)(at - lmb_pair_alphabet);
            seen[k][at - lmb_pair_alphabet]++;
        }
        for (int k = 0; k < 5; k++) decoded[k] = (uint8_t)(v >> (32 - 8 * k));
        assert(!memcmp(bits, decoded, 5));
    }
    /* 200 expected per cell; a bias of one bit would leave cells empty. */
    for (int k = 0; k < LMB_PAIR_CODE_LEN; k++)
        for (int j = 0; j < 32; j++) assert(seen[k][j] > 120 && seen[k][j] < 300);
    lmb_pair_code_new(code); lmb_pair_code_new(back);
    assert(strlen(code) == LMB_PAIR_CODE_LEN && strcmp(code, back));

    assert(!lmb_pair_code_normalize(back, "7k3m-9qxt") && !strcmp(back, "7K3M9QXT"));
    assert(!lmb_pair_code_normalize(back, " 7K3M 9QXT ") && !strcmp(back, "7K3M9QXT"));
    assert(!lmb_pair_code_normalize(back, "oIl0-1Li0") && !strcmp(back, "01101110"));
    assert(lmb_pair_code_normalize(back, "7K3M-9QX") && lmb_pair_code_normalize(back, "7K3M-9QXTA"));
    assert(lmb_pair_code_normalize(back, "7K3M-9QXU") && lmb_pair_code_normalize(back, "7K3M_9QXT"));
    assert(lmb_pair_code_normalize(back, "") && lmb_pair_code_normalize(back, "--------"));
    lmb_pair_code_format(shown, "7K3M9QXT"); assert(!strcmp(shown, "7K3M-9QXT"));

    uint8_t secret[LMB_PAIR_SECRET_LEN], identity[32], got_identity[32];
    char key[LMB_TOKEN_MAX + 1], longest[LMB_TOKEN_MAX + 2];
    lmb_random(identity, sizeof identity);
    assert(!lmb_pair_secret_encode(secret, "0123456789abcdef0123456789abcdef", identity));
    assert(!lmb_pair_secret_decode(secret, key, got_identity));
    assert(!strcmp(key, "0123456789abcdef0123456789abcdef") && !memcmp(identity, got_identity, 32));
    memset(longest, 'k', LMB_TOKEN_MAX); longest[LMB_TOKEN_MAX] = 0;
    assert(!lmb_pair_secret_encode(secret, longest, identity) && !lmb_pair_secret_decode(secret, key, got_identity));
    assert(!strcmp(key, longest));
    longest[LMB_TOKEN_MAX] = 'k'; longest[LMB_TOKEN_MAX + 1] = 0;
    assert(lmb_pair_secret_encode(secret, longest, identity) && lmb_pair_secret_encode(secret, "", identity));
    assert(lmb_pair_secret_encode(secret, "two\nlines", identity));
    assert(!lmb_pair_secret_encode(secret, "key", identity));
    secret[0] = 2; assert(lmb_pair_secret_decode(secret, key, got_identity)); secret[0] = 1;
    secret[1] = 0; assert(lmb_pair_secret_decode(secret, key, got_identity)); secret[1] = 3;
    secret[2 + 9] = 'x'; assert(lmb_pair_secret_decode(secret, key, got_identity)); secret[2 + 9] = 0;
    secret[3] = '\n'; assert(lmb_pair_secret_decode(secret, key, got_identity));
    puts("  ok   code: 40 uniform bits, forgiving input, fixed-size secret");
}

/* ---- the exchange, C against C --------------------------------------------- */
typedef struct {
    uint8_t id_tracker[32], id_joiner[32], sid[LMB_PAIR_SID_LEN];
    LmbPairExchange tracker, joiner;
    LmbPairKeys tracker_keys, joiner_keys;
    int tracker_rc, joiner_rc;
} Run;

static void fresh(Run *run) {
    memset(run, 0, sizeof *run);
    lmb_random(run->id_tracker, 32); lmb_random(run->id_joiner, 32);
    lmb_random(run->sid, sizeof run->sid);
}
static void begin_tracker(Run *run, const char *code) {
    uint8_t scalar[32]; lmb_random(scalar, sizeof scalar);
    lmb_pair_begin(&run->tracker, 1, code, run->id_tracker, run->id_joiner, run->sid, scalar);
}
static void begin_joiner(Run *run, const char *code, const uint8_t *id_tracker,
                         const uint8_t *id_joiner, const uint8_t *sid) {
    uint8_t scalar[32]; lmb_random(scalar, sizeof scalar);
    lmb_pair_begin(&run->joiner, 0, code, id_tracker, id_joiner, sid, scalar);
}
static void finish_both(Run *run, const uint8_t *to_tracker, const uint8_t *to_joiner) {
    uint8_t yt[32], yj[32];
    memcpy(yt, run->tracker.own, 32); memcpy(yj, run->joiner.own, 32);
    run->tracker_rc = lmb_pair_finish(&run->tracker, to_tracker ? to_tracker : yj, &run->tracker_keys);
    run->joiner_rc = lmb_pair_finish(&run->joiner, to_joiner ? to_joiner : yt, &run->joiner_keys);
}
/* What each side checks on the wire. */
static int tracker_accepts(const Run *run) {
    return !run->tracker_rc && lmb_pair_tag_equal(run->joiner_keys.tag_joiner, run->tracker_keys.tag_joiner);
}
static int joiner_accepts(const Run *run) {
    return !run->joiner_rc && lmb_pair_tag_equal(run->tracker_keys.tag_tracker, run->joiner_keys.tag_tracker);
}
/* The transcript hash covers public values only, so it may well be equal;
 * what must differ is everything derived from the shared point. */
static int nothing_agrees(const Run *run) {
    return !tracker_accepts(run) && !joiner_accepts(run) &&
           memcmp(run->tracker_keys.seal_key, run->joiner_keys.seal_key, 32);
}

static void test_exchange(void) {
    Run run; uint8_t secret[LMB_PAIR_SECRET_LEN], sealed[LMB_PAIR_SECRET_LEN], opened[LMB_PAIR_SECRET_LEN], tag[16];
    char key[LMB_TOKEN_MAX + 1]; uint8_t identity[32];

    /* The same code on both sides: both proofs hold and the secret arrives. */
    fresh(&run); begin_tracker(&run, "7K3M9QXT");
    begin_joiner(&run, "7K3M9QXT", run.id_tracker, run.id_joiner, run.sid);
    assert(memcmp(run.tracker.own, run.joiner.own, 32));
    finish_both(&run, NULL, NULL);
    assert(tracker_accepts(&run) && joiner_accepts(&run));
    assert(!memcmp(run.tracker_keys.seal_key, run.joiner_keys.seal_key, 32));
    assert(!memcmp(run.tracker_keys.transcript, run.joiner_keys.transcript, 64));
    /* Three different keys: a tag is never a value under the sealing key. */
    assert(memcmp(run.tracker_keys.tag_tracker, run.tracker_keys.tag_joiner, 32));
    assert(memcmp(run.tracker_keys.tag_tracker, run.tracker_keys.seal_key, 32));
    assert(memcmp(run.tracker_keys.tag_joiner, run.tracker_keys.seal_key, 32));
    { uint8_t any = 0; for (int i = 0; i < 32; i++) any |= run.tracker.scalar[i] | run.joiner.scalar[i]; assert(!any); }
    assert(!lmb_pair_secret_encode(secret, "0123456789abcdef0123456789abcdef", run.id_tracker));
    lmb_pair_seal(&run.tracker_keys, secret, sizeof secret, sealed, tag);
    assert(memcmp(sealed, secret, sizeof secret));
    assert(!lmb_pair_open(&run.joiner_keys, sealed, sizeof sealed, tag, opened));
    assert(!lmb_pair_secret_decode(opened, key, identity));
    assert(!strcmp(key, "0123456789abcdef0123456789abcdef") && !memcmp(identity, run.id_tracker, 32));
    sealed[40] ^= 1; assert(lmb_pair_open(&run.joiner_keys, sealed, sizeof sealed, tag, opened)); sealed[40] ^= 1;
    tag[3] ^= 0x10; assert(lmb_pair_open(&run.joiner_keys, sealed, sizeof sealed, tag, opened)); tag[3] ^= 0x10;
    assert(lmb_pair_open(&run.joiner_keys, sealed, sizeof sealed - 1, tag, opened));          /* truncated */
    Run first = run;                                    /* kept for the replay cases */

    /* A wrong code, differing in one symbol. */
    fresh(&run); begin_tracker(&run, "7K3M9QXT");
    begin_joiner(&run, "7K3M9QXV", run.id_tracker, run.id_joiner, run.sid);
    finish_both(&run, NULL, NULL);
    assert(nothing_agrees(&run));
    lmb_pair_seal(&run.tracker_keys, secret, sizeof secret, sealed, tag);
    assert(lmb_pair_open(&run.joiner_keys, sealed, sizeof sealed, tag, opened));

    /* A relay in the middle: each side sees the relay's identity as the other's. */
    { uint8_t relay[32]; lmb_random(relay, sizeof relay);
      fresh(&run); begin_tracker(&run, "7K3M9QXT");                       /* tracker thinks joiner = id_joiner */
      begin_joiner(&run, "7K3M9QXT", relay, run.id_joiner, run.sid);       /* joiner thinks tracker = relay */
      finish_both(&run, NULL, NULL); assert(nothing_agrees(&run));
      fresh(&run); begin_tracker(&run, "7K3M9QXT");
      begin_joiner(&run, "7K3M9QXT", run.id_tracker, relay, run.sid);      /* tracker saw another joiner identity */
      finish_both(&run, NULL, NULL); assert(nothing_agrees(&run)); }
    /* The two identities in each other's place. */
    fresh(&run); begin_tracker(&run, "7K3M9QXT");
    begin_joiner(&run, "7K3M9QXT", run.id_joiner, run.id_tracker, run.sid);
    finish_both(&run, NULL, NULL); assert(nothing_agrees(&run));
    /* Another session identifier. */
    { uint8_t other[LMB_PAIR_SID_LEN];
      fresh(&run); begin_tracker(&run, "7K3M9QXT");
      memcpy(other, run.sid, sizeof other); other[20] ^= 1;
      begin_joiner(&run, "7K3M9QXT", run.id_tracker, run.id_joiner, other);
      finish_both(&run, NULL, NULL); assert(nothing_agrees(&run)); }

    /* A public value changed in transit, in either direction. */
    for (int direction = 0; direction < 2; direction++) {
        uint8_t changed[32];
        fresh(&run); begin_tracker(&run, "7K3M9QXT");
        begin_joiner(&run, "7K3M9QXT", run.id_tracker, run.id_joiner, run.sid);
        memcpy(changed, direction ? run.tracker.own : run.joiner.own, 32); changed[7] ^= 4;
        finish_both(&run, direction ? NULL : changed, direction ? changed : NULL);
        assert(nothing_agrees(&run));
    }

    /* Replay. The joiner's recorded value and proof, sent to a new session of
     * the same tracker with the same code and identities... */
    fresh(&run); memcpy(run.id_tracker, first.id_tracker, 32); memcpy(run.id_joiner, first.id_joiner, 32);
    begin_tracker(&run, "7K3M9QXT");
    run.tracker_rc = lmb_pair_finish(&run.tracker, first.joiner.own, &run.tracker_keys);
    assert(!run.tracker_rc && !lmb_pair_tag_equal(first.joiner_keys.tag_joiner, run.tracker_keys.tag_joiner));
    /* ...even when the attacker also forces the old session identifier: the
     * tracker's fresh scalar still changes the shared point. */
    memcpy(run.sid, first.sid, sizeof run.sid); begin_tracker(&run, "7K3M9QXT");
    run.tracker_rc = lmb_pair_finish(&run.tracker, first.joiner.own, &run.tracker_keys);
    assert(!run.tracker_rc && !lmb_pair_tag_equal(first.joiner_keys.tag_joiner, run.tracker_keys.tag_joiner));
    /* The tracker's recorded value, proof and sealed secret, sent to a new joiner. */
    lmb_pair_seal(&first.tracker_keys, secret, sizeof secret, sealed, tag);
    begin_joiner(&run, "7K3M9QXT", first.id_tracker, first.id_joiner, first.sid);
    run.joiner_rc = lmb_pair_finish(&run.joiner, first.tracker.own, &run.joiner_keys);
    assert(!run.joiner_rc && !lmb_pair_tag_equal(first.tracker_keys.tag_tracker, run.joiner_keys.tag_tracker));
    assert(lmb_pair_open(&run.joiner_keys, sealed, sizeof sealed, tag, opened));

    /* Low-order values: refused by both roles, and the keys left behind are
     * not the ones an attacker could compute from an all-zero shared point. */
    for (size_t i = 0; i < sizeof low_order / sizeof *low_order; i++) {
        uint8_t point[32]; LmbPairKeys again;
        assert(!parse_hex(point, 32, low_order[i]));
        fresh(&run); begin_tracker(&run, "7K3M9QXT");
        begin_joiner(&run, "7K3M9QXT", run.id_tracker, run.id_joiner, run.sid);
        LmbPairExchange copy = run.tracker;
        finish_both(&run, point, point);
        assert(run.tracker_rc == -1 && run.joiner_rc == -1 && nothing_agrees(&run));
        assert(lmb_pair_finish(&copy, point, &again) == -1);
        assert(memcmp(again.seal_key, run.tracker_keys.seal_key, 32));
        assert(memcmp(again.tag_tracker, run.tracker_keys.tag_tracker, 32));
    }
    puts("  ok   exchange: agreement, wrong code, relay, replay, tampering, low-order values");
}

/* ---- the tracker's limits -------------------------------------------------- */
static void test_registry(void) {
    static LmbPairRegistry reg = LMB_PAIR_REGISTRY_INIT;
    LmbPairOpened opened, second; LmbPairStatus status;
    char code[LMB_PAIR_CODE_LEN + 1], source[32]; uint8_t joiner[32], blank[LMB_PAIR_ID_LEN] = {0};
    uint64_t now = 1000000, ticket;
    lmb_random(joiner, sizeof joiner);

    /* Nothing open: a decoy, and no status for any handle. */
    assert(!lmb_pair_registry_begin(&reg, now, "10.0.0.1", code) && strlen(code) == LMB_PAIR_CODE_LEN);
    lmb_pair_registry_status(&reg, now, blank, &status); assert(status.state == LMB_PAIR_UNKNOWN);
    assert(lmb_pair_registry_cancel(&reg, blank) == -1);

    /* One use. */
    lmb_pair_registry_open(&reg, now, 0, &opened);
    assert(opened.lifetime_s == LMB_PAIR_LIFETIME_S && opened.max_attempts == LMB_PAIR_MAX_ATTEMPTS);
    lmb_pair_registry_status(&reg, now, opened.id, &status);
    assert(status.state == LMB_PAIR_WAITING && status.remaining_s == LMB_PAIR_LIFETIME_S && !status.failed);
    ticket = lmb_pair_registry_begin(&reg, now, "10.0.0.2", code);
    assert(ticket && !strcmp(code, opened.code));
    uint64_t rival = lmb_pair_registry_begin(&reg, now, "10.0.0.3", code);
    assert(rival == ticket && !strcmp(code, opened.code));
    assert(!lmb_pair_registry_commit(&reg, now + 5, ticket, joiner, "10.0.0.2"));
    assert(lmb_pair_registry_commit(&reg, now + 6, rival, joiner, "10.0.0.3") == -1);   /* not twice */
    assert(!reg.code[0]);                                                              /* wiped */
    lmb_pair_registry_status(&reg, now + 7, opened.id, &status);
    assert(status.state == LMB_PAIR_USED && !memcmp(status.joiner, joiner, 32) && !strcmp(status.source, "10.0.0.2"));
    assert(!lmb_pair_registry_begin(&reg, now + 8, "10.0.0.4", code) && strcmp(code, opened.code));
    assert(!lmb_pair_registry_fail(&reg, now + 9, rival, "10.0.0.3"));

    /* Five failures destroy the code; the sixth attempt meets a decoy. */
    now += 100000;
    lmb_pair_registry_open(&reg, now, 0, &opened);
    for (unsigned i = 1; i <= LMB_PAIR_MAX_ATTEMPTS; i++) {
        snprintf(source, sizeof source, "10.0.1.%u", i);
        ticket = lmb_pair_registry_begin(&reg, now + i, source, code);
        assert(ticket && !strcmp(code, opened.code));
        assert(lmb_pair_registry_fail(&reg, now + i, ticket, source) == (i == LMB_PAIR_MAX_ATTEMPTS));
        lmb_pair_registry_status(&reg, now + i, opened.id, &status);
        assert(status.failed == i && !strcmp(status.source, source));
        assert(status.state == (i == LMB_PAIR_MAX_ATTEMPTS ? LMB_PAIR_LOCKED : LMB_PAIR_WAITING));
    }
    assert(!reg.code[0]);
    assert(!lmb_pair_registry_begin(&reg, now + 10, "10.0.1.9", code) && strcmp(code, opened.code));
    assert(lmb_pair_registry_commit(&reg, now + 11, ticket, joiner, "10.0.1.9") == -1);

    /* The limit counts exchanges started, not only those that have failed:
     * a burst of parallel attempts gets five, however slowly they end. */
    now += 100000;
    lmb_pair_registry_open(&reg, now, 0, &opened);
    for (unsigned i = 1; i <= LMB_PAIR_MAX_ATTEMPTS + 3; i++) {
        snprintf(source, sizeof source, "10.0.2.%u", i);
        ticket = lmb_pair_registry_begin(&reg, now, source, code);
        assert(!ticket == (i > LMB_PAIR_MAX_ATTEMPTS));
    }
    lmb_pair_registry_status(&reg, now, opened.id, &status); assert(status.state == LMB_PAIR_WAITING);

    /* Expiry, at the second; an exchange that started in time cannot finish late. */
    now += 100000;
    lmb_pair_registry_open(&reg, now, 60, &opened); assert(opened.lifetime_s == 60);
    ticket = lmb_pair_registry_begin(&reg, now + 59999, "10.0.3.1", code); assert(ticket);
    lmb_pair_registry_status(&reg, now + 59999, opened.id, &status);
    assert(status.state == LMB_PAIR_WAITING && status.remaining_s == 1);
    assert(lmb_pair_registry_commit(&reg, now + 60000, ticket, joiner, "10.0.3.1") == -1);
    lmb_pair_registry_status(&reg, now + 60000, opened.id, &status);
    assert(status.state == LMB_PAIR_EXPIRED && !reg.code[0]);
    assert(!lmb_pair_registry_begin(&reg, now + 60001, "10.0.3.2", code));
    /* A request can shorten the lifetime and cannot extend it. */
    lmb_pair_registry_open(&reg, now, LMB_PAIR_LIFETIME_S + 1, &opened); assert(opened.lifetime_s == LMB_PAIR_LIFETIME_S);
    lmb_pair_registry_open(&reg, now, 86400, &opened); assert(opened.lifetime_s == LMB_PAIR_LIFETIME_S);
    lmb_pair_registry_open(&reg, now, 1, &opened); assert(opened.lifetime_s == 1);

    /* One live code: a new one replaces the old, whose handle then names nothing. */
    now += 100000;
    lmb_pair_registry_open(&reg, now, 0, &opened);
    ticket = lmb_pair_registry_begin(&reg, now, "10.0.4.1", code);
    lmb_pair_registry_open(&reg, now + 1, 0, &second);
    assert(strcmp(opened.code, second.code) && memcmp(opened.id, second.id, LMB_PAIR_ID_LEN));
    lmb_pair_registry_status(&reg, now + 2, opened.id, &status); assert(status.state == LMB_PAIR_UNKNOWN);
    assert(lmb_pair_registry_commit(&reg, now + 2, ticket, joiner, "10.0.4.1") == -1);
    assert(!lmb_pair_registry_fail(&reg, now + 2, ticket, "10.0.4.1"));
    lmb_pair_registry_status(&reg, now + 2, second.id, &status);
    assert(status.state == LMB_PAIR_WAITING && !status.failed);
    assert(lmb_pair_registry_cancel(&reg, opened.id) == -1 && !lmb_pair_registry_cancel(&reg, second.id));
    lmb_pair_registry_status(&reg, now + 3, second.id, &status);
    assert(status.state == LMB_PAIR_CANCELLED && !reg.code[0]);
    assert(!lmb_pair_registry_begin(&reg, now + 3, "10.0.4.2", code));

    /* Rate per source: a burst, then one attempt per refill interval. An
     * attempt over the rate never reaches the code and is not counted. */
    now += 100000;
    for (unsigned i = 0; i < LMB_PAIR_RATE_BURST; i++)                                    /* no code is live: decoys */
        assert(!lmb_pair_registry_begin(&reg, now + i, "10.0.5.2", code));
    lmb_pair_registry_open(&reg, now + 10, 0, &opened);
    assert(!lmb_pair_registry_begin(&reg, now + 11, "10.0.5.2", code) && strcmp(code, opened.code));
    assert(!lmb_pair_registry_begin(&reg, now + LMB_PAIR_RATE_REFILL_MS - 1, "10.0.5.2", code));
    lmb_pair_registry_status(&reg, now + LMB_PAIR_RATE_REFILL_MS - 1, opened.id, &status);
    assert(status.state == LMB_PAIR_WAITING && !status.failed && reg.started == 0);
    assert(lmb_pair_registry_begin(&reg, now + LMB_PAIR_RATE_REFILL_MS - 1, "10.0.5.3", code)); /* another source is unaffected */
    ticket = lmb_pair_registry_begin(&reg, now + LMB_PAIR_RATE_REFILL_MS + 5, "10.0.5.2", code);
    assert(ticket && !strcmp(code, opened.code));
    assert(!lmb_pair_registry_begin(&reg, now + LMB_PAIR_RATE_REFILL_MS + 6, "10.0.5.2", code));
    /* More sources than slots: the table recycles, it does not overflow. */
    for (unsigned i = 0; i < 3 * LMB_PAIR_RATE_SLOTS; i++) {
        snprintf(source, sizeof source, "10.9.%u.%u", i / 200, i % 200);
        (void)lmb_pair_registry_begin(&reg, now + 20000 + i, source, code);
    }
    puts("  ok   registry: one use, five attempts, lifetime, one live code, rate per source");
}

/* ---- single results for the Python model ---------------------------------- */
static int model(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "map")) {
        uint8_t r[32], u[32];
        if (parse_hex(r, 32, argv[2])) return 2;
        lmb_elligator2(u, r); print_hex("u", u, 32);
        return 0;
    }
    int finish = argc >= 9 && !strcmp(argv[1], "finish");
    if (!finish && !(argc == 8 && !strcmp(argv[1], "begin"))) return 2;
    uint8_t id_tracker[32], id_joiner[32], sid[LMB_PAIR_SID_LEN], scalar[32], peer[32];
    char code[LMB_PAIR_CODE_LEN + 1];
    int tracker = !strcmp(argv[2], "tracker");
    if ((!tracker && strcmp(argv[2], "joiner")) || lmb_pair_code_normalize(code, argv[3]) ||
        parse_hex(id_tracker, 32, argv[4]) || parse_hex(id_joiner, 32, argv[5]) ||
        parse_hex(sid, sizeof sid, argv[6]) || parse_hex(scalar, 32, argv[7])) return 2;
    LmbPairExchange exchange; LmbPairKeys keys;
    lmb_pair_begin(&exchange, tracker, code, id_tracker, id_joiner, sid, scalar);
    print_hex("public", exchange.own, 32);
    if (!finish) return 0;
    if (parse_hex(peer, 32, argv[8])) return 2;
    int rc = lmb_pair_finish(&exchange, peer, &keys);
    printf("low_order %d\n", rc ? 1 : 0);
    if (rc) return 0;
    print_hex("tag_tracker", keys.tag_tracker, LMB_PAIR_TAG_LEN);
    print_hex("tag_joiner", keys.tag_joiner, LMB_PAIR_TAG_LEN);
    print_hex("seal_key", keys.seal_key, 32);
    print_hex("transcript", keys.transcript, 64);
    if (argc == 10 && tracker) {                       /* household key -> sealed secret */
        uint8_t secret[LMB_PAIR_SECRET_LEN], sealed[LMB_PAIR_SECRET_LEN + 16];
        if (lmb_pair_secret_encode(secret, argv[9], id_tracker)) return 2;
        lmb_pair_seal(&keys, secret, sizeof secret, sealed, sealed + LMB_PAIR_SECRET_LEN);
        print_hex("sealed", sealed, sizeof sealed);
    } else if (argc == 10) {                           /* sealed secret -> household key */
        uint8_t sealed[LMB_PAIR_SECRET_LEN + 16], secret[LMB_PAIR_SECRET_LEN], identity[32];
        char key[LMB_TOKEN_MAX + 1];
        if (parse_hex(sealed, sizeof sealed, argv[9])) return 2;
        if (lmb_pair_open(&keys, sealed, LMB_PAIR_SECRET_LEN, sealed + LMB_PAIR_SECRET_LEN, secret) ||
            lmb_pair_secret_decode(secret, key, identity)) { puts("opened 0"); return 0; }
        printf("opened 1\nkey %s\n", key);
        print_hex("identity", identity, 32);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) return model(argc, argv);
    test_field();
    test_elligator();
    test_code();
    test_exchange();
    test_registry();
    puts("pairing: PASS");
    return 0;
}
