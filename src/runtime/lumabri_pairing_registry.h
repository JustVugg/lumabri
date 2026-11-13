/* The tracker's side of a pairing code: one code at a time, in memory only,
 * with the limits that keep a short code safe against guessing online.
 *
 *   - one live code; opening another destroys the previous one
 *   - a lifetime (300 s by default; a request may shorten it, never extend it)
 *   - at most LMB_PAIR_MAX_ATTEMPTS exchanges ever run against a code, and
 *     the code is destroyed when that many have failed
 *   - one use: the first proven exchange consumes it
 *   - attempts are rate-limited per source address
 *
 * An attempt that may not use the live code (none is live, the limit was
 * reached, the source is over its rate) is given a random decoy instead and
 * runs the same exchange to the same failure, so the answer does not say
 * which of those it was.
 *
 * The code is never written to a file or a log. It is wiped as soon as it is
 * used, expired, locked, cancelled or replaced. The clock is a parameter so
 * that the limits can be tested without waiting. */
#ifndef LUMABRI_PAIRING_REGISTRY_H
#define LUMABRI_PAIRING_REGISTRY_H
#include <pthread.h>
#include "src/protocol/lumabri_pairing_wire.h"

#define LMB_PAIR_RATE_SLOTS     64
#define LMB_PAIR_RATE_BURST     5u
#define LMB_PAIR_RATE_REFILL_MS 2000u   /* then one more attempt per source every 2 s */

typedef struct {
    char source[LMB_PAIR_SOURCE_LEN];
    uint32_t tokens;
    uint64_t refilled_ms, used_ms;
} LmbPairRate;

typedef struct {
    pthread_mutex_t lock;
    uint32_t state;
    uint64_t generation;                  /* names the code an attempt started against */
    char code[LMB_PAIR_CODE_LEN + 1];
    uint8_t id[LMB_PAIR_ID_LEN];          /* the owner's handle; not a secret from the owner */
    uint64_t expires_ms;
    uint32_t lifetime_s, started, failed;
    uint8_t joiner[32];
    char source[LMB_PAIR_SOURCE_LEN];     /* who joined, or who failed last */
    LmbPairRate rates[LMB_PAIR_RATE_SLOTS];
} LmbPairRegistry;
#define LMB_PAIR_REGISTRY_INIT { .lock = PTHREAD_MUTEX_INITIALIZER }

static void lmb_pair_registry_end_locked(LmbPairRegistry *r, uint32_t state) {
    lmb_cpace_wipe(r->code, sizeof r->code);
    r->state = state;
}

static void lmb_pair_registry_expire_locked(LmbPairRegistry *r, uint64_t now_ms) {
    if (r->state == LMB_PAIR_WAITING && now_ms >= r->expires_ms)
        lmb_pair_registry_end_locked(r, LMB_PAIR_EXPIRED);
}

static int lmb_pair_registry_id_equal(const uint8_t a[LMB_PAIR_ID_LEN],
                                      const uint8_t b[LMB_PAIR_ID_LEN]) {
    uint8_t d = 0;
    for (int i = 0; i < LMB_PAIR_ID_LEN; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* 1 when this source may start an attempt now. A source seen for the first
 * time takes an empty slot, or else the least recently used one. Someone who
 * controls more than LMB_PAIR_RATE_SLOTS addresses can therefore renew their
 * allowance; the limit on attempts per code does not depend on this table. */
static int lmb_pair_registry_rate_locked(LmbPairRegistry *r, uint64_t now_ms, const char *source) {
    LmbPairRate *slot = NULL, *victim = &r->rates[0];
    for (int i = 0; i < LMB_PAIR_RATE_SLOTS; i++) {
        LmbPairRate *candidate = &r->rates[i];
        if (candidate->source[0] && !strcmp(candidate->source, source)) { slot = candidate; break; }
        if (!victim->source[0]) continue;                 /* already an empty slot */
        if (!candidate->source[0] || candidate->used_ms < victim->used_ms) victim = candidate;
    }
    if (!slot) {
        slot = victim;
        memset(slot, 0, sizeof *slot);
        snprintf(slot->source, sizeof slot->source, "%s", source);
        slot->tokens = LMB_PAIR_RATE_BURST;
        slot->refilled_ms = now_ms;
    }
    uint64_t earned = (now_ms - slot->refilled_ms) / LMB_PAIR_RATE_REFILL_MS;
    if (earned) {
        uint64_t total = (uint64_t)slot->tokens + earned;
        slot->tokens = total > LMB_PAIR_RATE_BURST ? LMB_PAIR_RATE_BURST : (uint32_t)total;
        slot->refilled_ms = slot->tokens == LMB_PAIR_RATE_BURST ? now_ms :
                            slot->refilled_ms + earned * LMB_PAIR_RATE_REFILL_MS;
    }
    slot->used_ms = now_ms;
    if (!slot->tokens) return 0;
    slot->tokens--;
    return 1;
}

/* Create the household's code, destroying any previous one. A requested
 * lifetime of zero, or longer than the default, means the default. */
static LMB_MAYBE_UNUSED void lmb_pair_registry_open(LmbPairRegistry *r, uint64_t now_ms,
                                                    uint32_t lifetime_s, LmbPairOpened *out) {
    char code[LMB_PAIR_CODE_LEN + 1];
    uint8_t id[LMB_PAIR_ID_LEN];
    lmb_pair_code_new(code);
    lmb_random(id, sizeof id);
    if (!lifetime_s || lifetime_s > LMB_PAIR_LIFETIME_S) lifetime_s = LMB_PAIR_LIFETIME_S;
    pthread_mutex_lock(&r->lock);
    lmb_pair_registry_end_locked(r, LMB_PAIR_WAITING);
    r->generation++;
    memcpy(r->code, code, sizeof r->code);
    memcpy(r->id, id, sizeof r->id);
    r->lifetime_s = lifetime_s;
    r->expires_ms = now_ms + (uint64_t)lifetime_s * 1000u;
    r->started = r->failed = 0;
    memset(r->joiner, 0, sizeof r->joiner);
    memset(r->source, 0, sizeof r->source);
    pthread_mutex_unlock(&r->lock);
    memset(out, 0, sizeof *out);
    memcpy(out->id, id, sizeof id);
    memcpy(out->code, code, sizeof code);
    out->lifetime_s = lifetime_s;
    out->max_attempts = LMB_PAIR_MAX_ATTEMPTS;
    lmb_cpace_wipe(code, sizeof code);
}

/* What the owner may know about the code with this handle. */
static LMB_MAYBE_UNUSED void lmb_pair_registry_status(LmbPairRegistry *r, uint64_t now_ms,
        const uint8_t id[LMB_PAIR_ID_LEN], LmbPairStatus *out) {
    memset(out, 0, sizeof *out);
    out->max_attempts = LMB_PAIR_MAX_ATTEMPTS;
    pthread_mutex_lock(&r->lock);
    lmb_pair_registry_expire_locked(r, now_ms);
    if (r->state != LMB_PAIR_UNKNOWN && lmb_pair_registry_id_equal(id, r->id)) {
        out->state = r->state;
        out->failed = r->failed;
        if (r->state == LMB_PAIR_WAITING)
            out->remaining_s = (uint32_t)((r->expires_ms - now_ms + 999u) / 1000u);
        memcpy(out->joiner, r->joiner, 32);
        memcpy(out->source, r->source, sizeof out->source);
    }
    pthread_mutex_unlock(&r->lock);
}

/* 0 when the handle names the current code (which is now withdrawn if it was
 * still live), -1 otherwise. */
static LMB_MAYBE_UNUSED int lmb_pair_registry_cancel(LmbPairRegistry *r,
                                                     const uint8_t id[LMB_PAIR_ID_LEN]) {
    pthread_mutex_lock(&r->lock);
    int known = r->state != LMB_PAIR_UNKNOWN && lmb_pair_registry_id_equal(id, r->id);
    if (known && r->state == LMB_PAIR_WAITING) lmb_pair_registry_end_locked(r, LMB_PAIR_CANCELLED);
    pthread_mutex_unlock(&r->lock);
    return known ? 0 : -1;
}

/* Start one attempt from `source`. `code` always receives eight symbols to
 * run the exchange with: the live code and a non-zero ticket when this
 * attempt may test it, a random decoy and ticket 0 otherwise. */
static LMB_MAYBE_UNUSED uint64_t lmb_pair_registry_begin(LmbPairRegistry *r, uint64_t now_ms,
        const char *source, char code[LMB_PAIR_CODE_LEN + 1]) {
    uint64_t ticket = 0;
    lmb_pair_code_new(code);              /* drawn every time, used or not */
    pthread_mutex_lock(&r->lock);
    lmb_pair_registry_expire_locked(r, now_ms);
    int allowed = lmb_pair_registry_rate_locked(r, now_ms, source);
    if (allowed && r->state == LMB_PAIR_WAITING && r->started < LMB_PAIR_MAX_ATTEMPTS) {
        r->started++;
        memcpy(code, r->code, LMB_PAIR_CODE_LEN + 1);
        ticket = r->generation;
    }
    pthread_mutex_unlock(&r->lock);
    return ticket;
}

/* The joiner proved it knows the code. 0: this is the one use of the code,
 * which no longer exists. -1: the code was used, expired, withdrawn or
 * replaced while the exchange ran; nothing may be sent. */
static LMB_MAYBE_UNUSED int lmb_pair_registry_commit(LmbPairRegistry *r, uint64_t now_ms,
        uint64_t ticket, const uint8_t joiner[32], const char *source) {
    pthread_mutex_lock(&r->lock);
    lmb_pair_registry_expire_locked(r, now_ms);
    int accepted = ticket && ticket == r->generation && r->state == LMB_PAIR_WAITING;
    if (accepted) {
        lmb_pair_registry_end_locked(r, LMB_PAIR_USED);
        memcpy(r->joiner, joiner, 32);
        snprintf(r->source, sizeof r->source, "%s", source);
    }
    pthread_mutex_unlock(&r->lock);
    return accepted ? 0 : -1;
}

/* An attempt against the live code ended without proof. Returns 1 when this
 * failure destroyed the code. */
static LMB_MAYBE_UNUSED int lmb_pair_registry_fail(LmbPairRegistry *r, uint64_t now_ms,
        uint64_t ticket, const char *source) {
    int destroyed = 0;
    pthread_mutex_lock(&r->lock);
    lmb_pair_registry_expire_locked(r, now_ms);
    if (ticket && ticket == r->generation && r->state == LMB_PAIR_WAITING) {
        r->failed++;
        snprintf(r->source, sizeof r->source, "%s", source);
        if (r->failed >= LMB_PAIR_MAX_ATTEMPTS) {
            lmb_pair_registry_end_locked(r, LMB_PAIR_LOCKED);
            destroyed = 1;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return destroyed;
}

#endif /* LUMABRI_PAIRING_REGISTRY_H */
