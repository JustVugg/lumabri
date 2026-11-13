/* Included by tracker.c: the tracker's pairing operations.
 *
 * A computer that does not have the household key yet sends LMB_PAIR_HELLO
 * on an encrypted connection, before AUTH. The exchange that follows proves
 * to the tracker that the other side knows the short code the owner is
 * looking at, proves the same to the other side, and only then delivers the
 * household key under a key derived from the exchange. docs/PAIRING.md has
 * the whole protocol; src/crypto/lumabri_cpace.h the cryptography.
 *
 * Every way an attempt can fail looks the same from outside: the same
 * computation, then one fixed reply after one fixed delay. */
#ifndef LUMABRI_PAIRING_TRACKER_H
#define LUMABRI_PAIRING_TRACKER_H
#include "src/runtime/lumabri_pairing_registry.h"

#define LMB_PAIR_CONFIRM_WAIT_MS 10000u  /* a joiner computes, it does not type */
/* A unit test may replace the registry's clock and shorten the refusal delay
 * when it compiles this file. A built tracker has no switch for either. */
#ifndef LMB_PAIR_FAIL_DELAY_MS
#define LMB_PAIR_FAIL_DELAY_MS   300u    /* from the confirmation frame to the refusal */
#endif
#ifndef LMB_PAIR_NOW_MS
#define LMB_PAIR_NOW_MS() lmb_socket_now_ms()
#endif

static LmbPairRegistry g_pair = LMB_PAIR_REGISTRY_INIT;

static void pair_sleep_until(uint64_t deadline_ms) {
    for (;;) {
        uint64_t now = lmb_socket_now_ms();
        if (!now || now >= deadline_ms) return;
#ifdef _WIN32
        Sleep((DWORD)(deadline_ms - now));
#else
        struct timespec wait = { (time_t)((deadline_ms - now) / 1000u),
                                 (long)((deadline_ms - now) % 1000u) * 1000000L };
        (void)nanosleep(&wait, NULL);
#endif
    }
}

/* One attempt to join with a code. The connection always ends here: a
 * computer that succeeded reconnects and authenticates like any other. */
static int handle_pair_hello(LmbSocket fd, LmbMsg *m) {
    uint8_t joiner[32];
    if (!lmb_secure_enabled() || !g_token[0] || lmb_secure_peer_id(fd, joiner)) {
        send_err(fd, "pairing codes need an encrypted household tracker");
        return -1;
    }
    if (m->body_len != LMB_PAIR_HELLO_LEN || m->pay_len || lmb_get32(m->body) != LMB_PAIR_VERSION) {
        send_err(fd, "unsupported pairing version");
        return -1;
    }
    char source[LMB_PAIR_SOURCE_LEN], code[LMB_PAIR_CODE_LEN + 1];
    connection_source(fd, source);
    if (!source[0]) snprintf(source, sizeof source, "unknown");

    uint8_t sid[LMB_PAIR_SID_LEN], scalar[32], reply[LMB_PAIR_HELLO_R_LEN];
    lmb_random(sid, LMB_PAIR_NONCE_LEN);
    memcpy(sid + LMB_PAIR_NONCE_LEN, m->body + 4, LMB_PAIR_NONCE_LEN);
    lmb_random(scalar, sizeof scalar);
    /* The live code, or a decoy that cannot succeed (ticket 0). */
    uint64_t ticket = lmb_pair_registry_begin(&g_pair, LMB_PAIR_NOW_MS(), source, code);
    LmbPairExchange exchange;
    lmb_pair_begin(&exchange, 1, code, g_sec_pk, joiner, sid, scalar);
    lmb_cpace_wipe(code, sizeof code);
    lmb_cpace_wipe(scalar, sizeof scalar);
    lmb_put32(reply, LMB_PAIR_VERSION);
    memcpy(reply + 4, sid, LMB_PAIR_NONCE_LEN);
    memcpy(reply + 4 + LMB_PAIR_NONCE_LEN, exchange.own, 32);

    LmbMsg confirm = {0};
    int answered = !lmb_send(fd, LMB_PAIR_HELLO_R, reply, sizeof reply, NULL, 0) &&
                   !lmb_recv_bounded(fd, &confirm, 256, LMB_PAIR_CONFIRM_WAIT_MS);
    uint64_t arrived = lmb_socket_now_ms();
    int shaped = answered && confirm.op == LMB_PAIR_CONFIRM &&
                 confirm.body_len == LMB_PAIR_CONFIRM_LEN && !confirm.pay_len;
    uint8_t peer[32] = {0}, tag[LMB_PAIR_TAG_LEN] = {0};
    if (shaped) {
        memcpy(peer, confirm.body, 32);
        memcpy(tag, confirm.body + 32, LMB_PAIR_TAG_LEN);
    }
    if (answered) lmb_msg_free(&confirm);

    /* Derive and compare first, whatever was received; decide afterwards. */
    LmbPairKeys keys;
    int low_order = lmb_pair_finish(&exchange, peer, &keys);
    int proved = lmb_pair_tag_equal(tag, keys.tag_joiner) & shaped & !low_order;
    int delivered = 0;
    if (proved && ticket &&
        !lmb_pair_registry_commit(&g_pair, LMB_PAIR_NOW_MS(), ticket, joiner, source)) {
        /* Key confirmation is complete in this direction, and the code no
         * longer exists. Only now does the household key leave this process. */
        uint8_t secret[LMB_PAIR_SECRET_LEN], done[LMB_PAIR_DONE_LEN];
        if (!lmb_pair_secret_encode(secret, g_token, g_sec_pk)) {
            memcpy(done, keys.tag_tracker, LMB_PAIR_TAG_LEN);
            lmb_pair_seal(&keys, secret, sizeof secret, done + LMB_PAIR_TAG_LEN,
                          done + LMB_PAIR_TAG_LEN + LMB_PAIR_SECRET_LEN);
            delivered = !lmb_send(fd, LMB_PAIR_DONE, done, sizeof done, NULL, 0);
        }
        lmb_cpace_wipe(secret, sizeof secret);
        lmb_cpace_wipe(done, sizeof done);
        char identity[65];
        lmb_hex(identity, joiner, 32);
        fprintf(stderr, "[tracker] pairing code used by identity %.16s from %s%s\n", identity,
                source, delivered ? "" : " (the reply could not be sent)");
    } else {
        if (lmb_pair_registry_fail(&g_pair, LMB_PAIR_NOW_MS(), ticket, source))
            fprintf(stderr, "[tracker] pairing code destroyed after %u failed attempts "
                            "(last from %s)\n", (unsigned)LMB_PAIR_MAX_ATTEMPTS, source);
        if (answered) {
            pair_sleep_until(arrived + LMB_PAIR_FAIL_DELAY_MS);
            send_err(fd, "pairing failed");
        }
    }
    lmb_cpace_wipe(&exchange, sizeof exchange);
    lmb_cpace_wipe(&keys, sizeof keys);
    lmb_cpace_wipe(sid, sizeof sid);
    return -1;
}

/* Codes are created and watched only from the computer that runs this
 * tracker: an authenticated connection whose identity is the tracker's own. */
static int pair_owner_request(LmbSocket fd, const LmbMsg *m, uint32_t length) {
    if (!lmb_secure_enabled() || !g_token[0] || !lmb_secure_peer_matches(fd, g_sec_pk)) {
        send_err(fd, "pairing codes are managed on the household owner's computer");
        return -1;
    }
    if (m->body_len != length || m->pay_len || lmb_get32(m->body) != LMB_PAIR_VERSION) {
        send_err(fd, "unsupported pairing version");
        return -1;
    }
    return 0;
}

static int handle_pair_open(LmbSocket fd, LmbMsg *m) {
    if (pair_owner_request(fd, m, LMB_PAIR_OPEN_LEN)) return -1;
    if (!lmb_pair_key_ok(g_token, strlen(g_token))) {
        send_err(fd, "this household key cannot be delivered by a pairing code");
        return -1;
    }
    LmbPairOpened opened;
    uint8_t body[LMB_PAIR_OPEN_R_LEN];
    lmb_pair_registry_open(&g_pair, LMB_PAIR_NOW_MS(), lmb_get32(m->body + 4), &opened);
    lmb_put32(body, LMB_PAIR_VERSION);
    memcpy(body + 4, opened.id, LMB_PAIR_ID_LEN);
    memcpy(body + 4 + LMB_PAIR_ID_LEN, opened.code, LMB_PAIR_CODE_LEN);
    lmb_put32(body + 4 + LMB_PAIR_ID_LEN + LMB_PAIR_CODE_LEN, opened.lifetime_s);
    lmb_put32(body + 8 + LMB_PAIR_ID_LEN + LMB_PAIR_CODE_LEN, opened.max_attempts);
    int rc = lmb_send(fd, LMB_PAIR_OPEN_R, body, sizeof body, NULL, 0);
    fprintf(stderr, "[tracker] pairing code created, valid for %u s\n", (unsigned)opened.lifetime_s);
    lmb_cpace_wipe(&opened, sizeof opened);
    lmb_cpace_wipe(body, sizeof body);
    return rc;
}

static int handle_pair_status(LmbSocket fd, LmbMsg *m) {
    if (pair_owner_request(fd, m, LMB_PAIR_HANDLE_LEN)) return -1;
    LmbPairStatus status;
    uint8_t body[LMB_PAIR_STATUS_R_LEN];
    lmb_pair_registry_status(&g_pair, LMB_PAIR_NOW_MS(), m->body + 4, &status);
    lmb_put32(body, LMB_PAIR_VERSION);
    lmb_put32(body + 4, status.state);
    lmb_put32(body + 8, status.remaining_s);
    lmb_put32(body + 12, status.failed);
    lmb_put32(body + 16, status.max_attempts);
    memcpy(body + 20, status.joiner, 32);
    memcpy(body + 52, status.source, LMB_PAIR_SOURCE_LEN);
    return lmb_send(fd, LMB_PAIR_STATUS_R, body, sizeof body, NULL, 0);
}

static int handle_pair_cancel(LmbSocket fd, LmbMsg *m) {
    if (pair_owner_request(fd, m, LMB_PAIR_HANDLE_LEN)) return -1;
    if (lmb_pair_registry_cancel(&g_pair, m->body + 4)) {
        send_err(fd, "no pairing code with this handle");
        return -1;
    }
    return lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
}

#endif /* LUMABRI_PAIRING_TRACKER_H */
