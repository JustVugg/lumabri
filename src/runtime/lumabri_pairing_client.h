/* Pairing with a short code, as seen by the two computers.
 *
 * Owner's computer (the one that runs the tracker):
 *   lmb_pair_owner_open()    create the household's one code
 *   lmb_pair_owner_status()  waiting / used / expired / locked / cancelled
 *   lmb_pair_owner_cancel()  withdraw it
 *
 * Joining computer:
 *   lmb_pair_join()          code + address -> household key + pinned tracker
 *
 * These are the functions a terminal screen, a command or the web gateway
 * call; none of them prints or stores anything. They need the encrypted
 * transport (lmb_secure_init with LUMABRI_ENCRYPT) and, for the owner calls,
 * the household key in LUMABRI_TOKEN, exactly like every other household
 * request. A caller must wipe LmbPairOpened and LmbPairJoined when done. */
#ifndef LUMABRI_PAIRING_CLIENT_H
#define LUMABRI_PAIRING_CLIENT_H
#include "lumabri_secure.h"
#include "src/protocol/lumabri_pairing_wire.h"

enum {
    LMB_PAIR_OK = 0,
    LMB_PAIR_E_CONNECT = -1,      /* no encrypted connection to that address */
    LMB_PAIR_E_UNSUPPORTED = -2,  /* the tracker does not offer pairing codes */
    LMB_PAIR_E_NOT_OWNER = -3,    /* owner call, but the tracker is another computer's */
    LMB_PAIR_E_REFUSED = -4,      /* owner call refused, or handle unknown */
    LMB_PAIR_E_CODE = -5,         /* the text cannot be a code; nothing was sent */
    LMB_PAIR_E_FAILED = -6,       /* wrong, expired, used or withdrawn code: one answer for all */
    LMB_PAIR_E_IDENTITY = -7,     /* the tracker is not the computer that was chosen */
    LMB_PAIR_E_PINNED = -8,       /* this address is already pinned to another identity */
    LMB_PAIR_E_PROTOCOL = -9,     /* malformed or unexpected reply */
};

typedef struct {
    char household_key[LMB_TOKEN_MAX + 1];
    uint8_t tracker_identity[32];
} LmbPairJoined;

static LMB_MAYBE_UNUSED const char *lmb_pair_state_name(uint32_t state) {
    switch (state) {
    case LMB_PAIR_WAITING:   return "waiting";
    case LMB_PAIR_USED:      return "used";
    case LMB_PAIR_EXPIRED:   return "expired";
    case LMB_PAIR_LOCKED:    return "locked";
    case LMB_PAIR_CANCELLED: return "cancelled";
    default:                 return "unknown";
    }
}

/* One authenticated request to this computer's own tracker. On LMB_PAIR_OK
 * the reply has operation `want` and exactly `want_len` body bytes. */
static int lmb_pair_owner_call(const char *tracker, uint32_t op, const uint8_t *body,
                               uint32_t body_len, uint32_t want, uint32_t want_len, LmbMsg *reply) {
    memset(reply, 0, sizeof *reply);
    LmbSocket fd = lmb_connect_ms_io(tracker, 2000, 5000);
    if (fd == LMB_INVALID_SOCKET) return LMB_PAIR_E_CONNECT;
    int rc = LMB_PAIR_OK;
    if (!lmb_secure_peer_matches(fd, g_sec_pk)) rc = LMB_PAIR_E_NOT_OWNER;
    else if (lmb_auth(fd)) rc = LMB_PAIR_E_REFUSED;
    else if (lmb_send(fd, op, body, body_len, NULL, 0) || lmb_recv_bounded(fd, reply, 256, 5000))
        rc = LMB_PAIR_E_CONNECT;
    else if (reply->op == LMB_ERR) {
        /* "unknown op" is the whole answer of a tracker that predates pairing. */
        rc = reply->body_len == 2 + 10 && !memcmp(reply->body + 2, "unknown op", 10) ?
             LMB_PAIR_E_UNSUPPORTED : LMB_PAIR_E_REFUSED;
    } else if (reply->op != want || reply->body_len != want_len || reply->pay_len ||
               (want_len >= 4 && lmb_get32(reply->body) != LMB_PAIR_VERSION))
        rc = LMB_PAIR_E_PROTOCOL;
    lmb_close(fd);
    if (rc) { lmb_msg_free(reply); memset(reply, 0, sizeof *reply); }
    return rc;
}

/* Create the household's code. Any previous code stops working. A lifetime
 * of 0 asks for the default; the tracker never grants more than the default. */
static LMB_MAYBE_UNUSED int lmb_pair_owner_open(const char *tracker, uint32_t lifetime_s,
                                                LmbPairOpened *out) {
    uint8_t body[LMB_PAIR_OPEN_LEN];
    LmbMsg reply;
    memset(out, 0, sizeof *out);
    lmb_put32(body, LMB_PAIR_VERSION); lmb_put32(body + 4, lifetime_s);
    int rc = lmb_pair_owner_call(tracker, LMB_PAIR_OPEN, body, sizeof body,
                                 LMB_PAIR_OPEN_R, LMB_PAIR_OPEN_R_LEN, &reply);
    if (rc) return rc;
    const uint8_t *p = reply.body + 4;
    memcpy(out->id, p, LMB_PAIR_ID_LEN); p += LMB_PAIR_ID_LEN;
    memcpy(out->code, p, LMB_PAIR_CODE_LEN); p += LMB_PAIR_CODE_LEN;
    out->lifetime_s = lmb_get32(p); out->max_attempts = lmb_get32(p + 4);
    char checked[LMB_PAIR_CODE_LEN + 1];
    if (lmb_pair_code_normalize(checked, out->code) || memcmp(checked, out->code, LMB_PAIR_CODE_LEN))
        rc = LMB_PAIR_E_PROTOCOL;
    lmb_cpace_wipe(checked, sizeof checked);
    lmb_cpace_wipe(reply.storage ? reply.storage : reply.body, reply.body_len);
    lmb_msg_free(&reply);
    if (rc) lmb_cpace_wipe(out, sizeof *out);
    return rc;
}

static LMB_MAYBE_UNUSED int lmb_pair_owner_status(const char *tracker,
        const uint8_t id[LMB_PAIR_ID_LEN], LmbPairStatus *out) {
    uint8_t body[LMB_PAIR_HANDLE_LEN];
    LmbMsg reply;
    memset(out, 0, sizeof *out);
    lmb_put32(body, LMB_PAIR_VERSION); memcpy(body + 4, id, LMB_PAIR_ID_LEN);
    int rc = lmb_pair_owner_call(tracker, LMB_PAIR_STATUS, body, sizeof body,
                                 LMB_PAIR_STATUS_R, LMB_PAIR_STATUS_R_LEN, &reply);
    if (rc) return rc;
    out->state = lmb_get32(reply.body + 4);
    out->remaining_s = lmb_get32(reply.body + 8);
    out->failed = lmb_get32(reply.body + 12);
    out->max_attempts = lmb_get32(reply.body + 16);
    memcpy(out->joiner, reply.body + 20, 32);
    memcpy(out->source, reply.body + 52, LMB_PAIR_SOURCE_LEN);
    out->source[LMB_PAIR_SOURCE_LEN - 1] = 0;
    for (char *c = out->source; *c; c++) if ((unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e) *c = '?';
    lmb_msg_free(&reply);
    return out->state > LMB_PAIR_CANCELLED ? LMB_PAIR_E_PROTOCOL : LMB_PAIR_OK;
}

static LMB_MAYBE_UNUSED int lmb_pair_owner_cancel(const char *tracker,
                                                  const uint8_t id[LMB_PAIR_ID_LEN]) {
    uint8_t body[LMB_PAIR_HANDLE_LEN];
    LmbMsg reply;
    lmb_put32(body, LMB_PAIR_VERSION); memcpy(body + 4, id, LMB_PAIR_ID_LEN);
    int rc = lmb_pair_owner_call(tracker, LMB_PAIR_CANCEL, body, sizeof body, LMB_OK, 0, &reply);
    if (!rc) lmb_msg_free(&reply);
    return rc;
}

/* Join the household at `tracker` ("host:port") with the code its owner
 * reads out. `expected_identity`, when not NULL, is the identity LAN
 * discovery reported for the chosen computer; a different one is refused
 * before the code is used.
 *
 * On LMB_PAIR_OK the tracker has proven that it knows the code, `out` holds
 * the household key and the tracker's identity, and that identity is pinned
 * for `tracker` (the operator's pin file, or known_hosts) so that every later
 * connection to this address must present it. Nothing is written to the
 * household settings; that is the caller's decision. */
static LMB_MAYBE_UNUSED int lmb_pair_join(const char *tracker, const char *typed_code,
        const uint8_t *expected_identity, LmbPairJoined *out) {
    char code[LMB_PAIR_CODE_LEN + 1];
    memset(out, 0, sizeof *out);
    if (lmb_pair_code_normalize(code, typed_code)) return LMB_PAIR_E_CODE;
    LmbSocket fd = lmb_secure_connect_unpinned(tracker, 3000, 15000);
    if (fd == LMB_INVALID_SOCKET) { lmb_cpace_wipe(code, sizeof code); return LMB_PAIR_E_CONNECT; }

    int rc = LMB_PAIR_OK;
    uint8_t id_tracker[32], hello[LMB_PAIR_HELLO_LEN], sid[LMB_PAIR_SID_LEN], scalar[32];
    uint8_t confirm[LMB_PAIR_CONFIRM_LEN], secret[LMB_PAIR_SECRET_LEN];
    LmbPairExchange exchange; LmbPairKeys keys; LmbMsg reply = {0};
    memset(&exchange, 0, sizeof exchange); memset(&keys, 0, sizeof keys);
    memset(secret, 0, sizeof secret);
    lmb_put32(hello, LMB_PAIR_VERSION);
    lmb_random(hello + 4, LMB_PAIR_NONCE_LEN);
    if (lmb_secure_peer_id(fd, id_tracker)) rc = LMB_PAIR_E_CONNECT;
    else if (expected_identity && lmb_vrfy32(expected_identity, id_tracker)) rc = LMB_PAIR_E_IDENTITY;
    else if (lmb_send(fd, LMB_PAIR_HELLO, hello, sizeof hello, NULL, 0) ||
             lmb_recv_bounded(fd, &reply, 256, 10000)) rc = LMB_PAIR_E_CONNECT;
    else if (reply.op == LMB_ERR) rc = LMB_PAIR_E_UNSUPPORTED;
    else if (reply.op != LMB_PAIR_HELLO_R || reply.body_len != LMB_PAIR_HELLO_R_LEN ||
             reply.pay_len || lmb_get32(reply.body) != LMB_PAIR_VERSION) rc = LMB_PAIR_E_PROTOCOL;
    if (!rc) {
        memcpy(sid, reply.body + 4, LMB_PAIR_NONCE_LEN);
        memcpy(sid + LMB_PAIR_NONCE_LEN, hello + 4, LMB_PAIR_NONCE_LEN);
        lmb_random(scalar, sizeof scalar);
        lmb_pair_begin(&exchange, 0, code, id_tracker, g_sec_pk, sid, scalar);
        lmb_cpace_wipe(scalar, sizeof scalar);
        /* A low-order value from the tracker ends the attempt here. */
        if (lmb_pair_finish(&exchange, reply.body + 4 + LMB_PAIR_NONCE_LEN, &keys))
            rc = LMB_PAIR_E_FAILED;
    }
    lmb_msg_free(&reply); memset(&reply, 0, sizeof reply);
    if (!rc) {
        memcpy(confirm, exchange.own, 32);
        memcpy(confirm + 32, keys.tag_joiner, LMB_PAIR_TAG_LEN);
        if (lmb_send(fd, LMB_PAIR_CONFIRM, confirm, sizeof confirm, NULL, 0) ||
            lmb_recv_bounded(fd, &reply, 512, 10000)) rc = LMB_PAIR_E_FAILED;
        else if (reply.op == LMB_ERR) rc = LMB_PAIR_E_FAILED;
        else if (reply.op != LMB_PAIR_DONE || reply.body_len != LMB_PAIR_DONE_LEN || reply.pay_len)
            rc = LMB_PAIR_E_PROTOCOL;
        /* The tracker's proof comes first: without it nothing is decrypted. */
        else if (!lmb_pair_tag_equal(reply.body, keys.tag_tracker) ||
                 lmb_pair_open(&keys, reply.body + LMB_PAIR_TAG_LEN, LMB_PAIR_SECRET_LEN,
                               reply.body + LMB_PAIR_TAG_LEN + LMB_PAIR_SECRET_LEN, secret))
            rc = LMB_PAIR_E_FAILED;
        else if (lmb_pair_secret_decode(secret, out->household_key, out->tracker_identity))
            rc = LMB_PAIR_E_PROTOCOL;
        else if (lmb_vrfy32(out->tracker_identity, id_tracker)) rc = LMB_PAIR_E_IDENTITY;
        if (reply.body) lmb_cpace_wipe(reply.storage ? reply.storage : reply.body, reply.body_len);
        lmb_msg_free(&reply);
    }
    lmb_close(fd);
    /* The exchange authenticated this identity; from here on the address is
     * held to it, under the same rules as every other outbound connection. */
    if (!rc && lmb_sec_check_peer(tracker, id_tracker)) rc = LMB_PAIR_E_PINNED;
    lmb_cpace_wipe(code, sizeof code); lmb_cpace_wipe(sid, sizeof sid);
    lmb_cpace_wipe(&exchange, sizeof exchange); lmb_cpace_wipe(&keys, sizeof keys);
    lmb_cpace_wipe(secret, sizeof secret); lmb_cpace_wipe(confirm, sizeof confirm);
    if (rc) lmb_cpace_wipe(out, sizeof *out);
    return rc;
}

#endif /* LUMABRI_PAIRING_CLIENT_H */
