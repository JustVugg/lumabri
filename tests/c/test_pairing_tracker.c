/* The pairing protocol on real connections: this file embeds the tracker,
 * runs its connection threads on a loopback listener, and talks to them
 * through the encrypted transport, both with the real client functions and
 * with a hand-driven peer that owns another identity and can send what an
 * attacker would. A second, scripted tracker covers what the joining side
 * must refuse.
 *
 * Time does not pass by waiting here: the registry's clock is a test clock.
 * That hook, and the shorter refusal delay, exist only in this compilation. */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
static _Atomic uint64_t test_clock_skew;
#define LMB_PAIR_NOW_MS() (lmb_socket_now_ms() + atomic_load(&test_clock_skew))
#define LMB_PAIR_FAIL_DELAY_MS 60u
#define LMB_TRACKER_NO_ENTRY
#include "tracker.c"
#include "src/runtime/lumabri_pairing_client.h"
#include <assert.h>
#include <ftw.h>

static const char household_key[] = "0123456789abcdef0123456789abcdef";
static char tracker_address[64], fake_address[64], known_hosts[600], directory[512];
static int tracker_port;

static void advance(uint64_t ms) { atomic_fetch_add(&test_clock_skew, ms); }

static LmbSocket listen_loopback(int *port) {
    LmbSocket listener = lmb_listen(0);
    assert(listener != LMB_INVALID_SOCKET);
    struct sockaddr_in addr; LmbSocketLength length = sizeof addr;
    assert(!getsockname(listener, (struct sockaddr *)&addr, &length));
    *port = ntohs(addr.sin_port);
    return listener;
}

static void *accept_loop(void *arg) {
    LmbSocket listener = (LmbSocket)(uintptr_t)arg;
    for (;;) {
        LmbSocket fd = lmb_socket_accept(listener, NULL, NULL);
        if (fd == LMB_INVALID_SOCKET) break;
        lmb_set_io_timeout(fd, 5000);
        assert(lmb_conn_gate_enter(&g_conn_gate));
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, conn_thread, (void *)(uintptr_t)fd));
        pthread_detach(thread);
    }
    return NULL;
}

static int file_has(const char *path, const char *text) {
    char data[4096] = ""; FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(data, 1, sizeof data - 1, f); fclose(f); data[n] = 0;
    return strstr(data, text) != NULL;
}

static int all_zero(const void *p, size_t n) {
    uint8_t any = 0;
    for (size_t i = 0; i < n; i++) any |= ((const uint8_t *)p)[i];
    return !any;
}

static LmbPairStatus status_of(const LmbPairOpened *opened) {
    LmbPairStatus status;
    assert(!lmb_pair_owner_status(tracker_address, opened->id, &status));
    return status;
}

static LmbPairOpened open_code(uint32_t lifetime_s) {
    LmbPairOpened opened;
    assert(!lmb_pair_owner_open(tracker_address, lifetime_s, &opened));
    assert(strlen(opened.code) == LMB_PAIR_CODE_LEN && opened.max_attempts == LMB_PAIR_MAX_ATTEMPTS);
    return opened;
}

/* ---- a peer with its own identity, driven by hand -------------------------- */
typedef struct { LmbSocket fd; LmbSecure channel; uint8_t sk[64], pk[32]; } Raw;

static void raw_identity(Raw *raw) {
    uint8_t seed[32];
    memset(raw, 0, sizeof *raw);
    lmb_random(seed, sizeof seed); lmb_sign_keypair(raw->pk, raw->sk, seed);
}
static void raw_open(Raw *raw, int port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    raw->fd = lmb_socket_open(AF_INET, SOCK_STREAM, 0);
    assert(raw->fd != LMB_INVALID_SOCKET && !connect(raw->fd, (struct sockaddr *)&addr, sizeof addr));
    assert(!lmb_socket_nodelay(raw->fd) && !lmb_socket_timeout(raw->fd, 5000));
    assert(!lmb_secure_handshake(raw->fd, 1, raw->sk, raw->pk, &raw->channel));
    assert(raw->channel.have_peer_id && !memcmp(raw->channel.peer_id, g_sec_pk, 32));
}
static int raw_send(Raw *raw, uint32_t op, const void *body, uint32_t length) {
    return lmb_secure_send(&raw->channel, raw->fd, op, body, length, NULL, 0);
}
static int raw_recv(Raw *raw, LmbMsg *m) { return lmb_secure_recv(&raw->channel, raw->fd, m); }
static void raw_close(Raw *raw) { lmb_socket_close(raw->fd); raw->fd = LMB_INVALID_SOCKET; }
static int is_error(const LmbMsg *m, const char *text) {
    size_t n = strlen(text);
    return m->op == LMB_ERR && m->body_len == 2 + n && !memcmp(m->body + 2, text, n);
}
/* One request on a fresh connection, optionally after AUTH; the reply's ERR text. */
static void raw_refused(Raw *raw, int authenticate, uint32_t op, const void *body, uint32_t length,
                        const char *text) {
    LmbMsg m = {0};
    raw_open(raw, tracker_port);
    if (authenticate) {
        uint8_t auth[2 + sizeof household_key - 1];
        auth[0] = (uint8_t)(sizeof household_key - 1); auth[1] = 0;
        memcpy(auth + 2, household_key, sizeof household_key - 1);
        assert(!raw_send(raw, LMB_AUTH, auth, sizeof auth) && !raw_recv(raw, &m) && m.op == LMB_OK);
        lmb_msg_free(&m);
    }
    assert(!raw_send(raw, op, body, length) && !raw_recv(raw, &m));
    assert(is_error(&m, text));
    lmb_msg_free(&m);
    assert(raw_recv(raw, &m));                         /* and the connection is closed */
    raw_close(raw);
}

enum { NORMAL, TAMPER_PUBLIC, LOW_ORDER_PUBLIC, SHORT_CONFIRM, LONG_CONFIRM, OTHER_OP, HANG_UP, STALE_CONFIRM };
typedef struct {
    int result;                 /* 1: key delivered and verified; 0: "pairing failed"; -1: closed */
    char key[LMB_TOKEN_MAX + 1];
    uint8_t confirm[LMB_PAIR_CONFIRM_LEN];
    uint64_t answered_after_ms;
} Attempt;

/* One exchange as a joiner. `seen_tracker` / `claimed_joiner` replace the
 * identities the joiner binds (NULL: the true ones); `stale` is sent in
 * place of this session's confirmation. */
static Attempt attempt(Raw *raw, const char *code, int mode, const uint8_t *seen_tracker,
                       const uint8_t *claimed_joiner, const uint8_t *stale) {
    Attempt a; LmbMsg m = {0};
    uint8_t hello[LMB_PAIR_HELLO_LEN], sid[LMB_PAIR_SID_LEN], scalar[32], long_confirm[LMB_PAIR_CONFIRM_LEN + 1] = {0};
    LmbPairExchange exchange; LmbPairKeys keys;
    memset(&a, 0, sizeof a);
    raw_open(raw, tracker_port);
    lmb_put32(hello, LMB_PAIR_VERSION); lmb_random(hello + 4, LMB_PAIR_NONCE_LEN);
    assert(!raw_send(raw, LMB_PAIR_HELLO, hello, sizeof hello) && !raw_recv(raw, &m));
    assert(m.op == LMB_PAIR_HELLO_R && m.body_len == LMB_PAIR_HELLO_R_LEN && lmb_get32(m.body) == LMB_PAIR_VERSION);
    memcpy(sid, m.body + 4, LMB_PAIR_NONCE_LEN); memcpy(sid + LMB_PAIR_NONCE_LEN, hello + 4, LMB_PAIR_NONCE_LEN);
    lmb_random(scalar, sizeof scalar);
    lmb_pair_begin(&exchange, 0, code, seen_tracker ? seen_tracker : g_sec_pk,
                   claimed_joiner ? claimed_joiner : raw->pk, sid, scalar);
    assert(!lmb_pair_finish(&exchange, m.body + 4 + LMB_PAIR_NONCE_LEN, &keys));
    lmb_msg_free(&m);
    memcpy(a.confirm, exchange.own, 32); memcpy(a.confirm + 32, keys.tag_joiner, LMB_PAIR_TAG_LEN);
    if (mode == HANG_UP) { raw_close(raw); a.result = -1; return a; }
    uint64_t sent = lmb_socket_now_ms();
    if (mode == TAMPER_PUBLIC) { uint8_t changed[LMB_PAIR_CONFIRM_LEN]; memcpy(changed, a.confirm, sizeof changed);
        changed[9] ^= 0x20; assert(!raw_send(raw, LMB_PAIR_CONFIRM, changed, sizeof changed)); }
    else if (mode == LOW_ORDER_PUBLIC) { uint8_t changed[LMB_PAIR_CONFIRM_LEN]; memcpy(changed, a.confirm, sizeof changed);
        memset(changed, 0, 32); changed[0] = 1; assert(!raw_send(raw, LMB_PAIR_CONFIRM, changed, sizeof changed)); }
    else if (mode == SHORT_CONFIRM) assert(!raw_send(raw, LMB_PAIR_CONFIRM, a.confirm, sizeof a.confirm - 1));
    else if (mode == LONG_CONFIRM) { memcpy(long_confirm, a.confirm, sizeof a.confirm);
        assert(!raw_send(raw, LMB_PAIR_CONFIRM, long_confirm, sizeof long_confirm)); }
    else if (mode == OTHER_OP) assert(!raw_send(raw, LMB_PAIR_HELLO, hello, sizeof hello));
    else if (mode == STALE_CONFIRM) assert(!raw_send(raw, LMB_PAIR_CONFIRM, stale, LMB_PAIR_CONFIRM_LEN));
    else assert(!raw_send(raw, LMB_PAIR_CONFIRM, a.confirm, sizeof a.confirm));
    if (raw_recv(raw, &m)) { raw_close(raw); a.result = -1; return a; }
    a.answered_after_ms = lmb_socket_now_ms() - sent;
    if (m.op == LMB_PAIR_DONE) {
        uint8_t secret[LMB_PAIR_SECRET_LEN], identity[32];
        assert(m.body_len == LMB_PAIR_DONE_LEN && lmb_pair_tag_equal(m.body, keys.tag_tracker));
        assert(!lmb_pair_open(&keys, m.body + LMB_PAIR_TAG_LEN, LMB_PAIR_SECRET_LEN,
                              m.body + LMB_PAIR_TAG_LEN + LMB_PAIR_SECRET_LEN, secret));
        assert(!lmb_pair_secret_decode(secret, a.key, identity) && !memcmp(identity, g_sec_pk, 32));
        a.result = 1;
    } else {
        /* Every refusal is this one answer, never sooner than the fixed delay. */
        assert(is_error(&m, "pairing failed") && a.answered_after_ms + 5 >= LMB_PAIR_FAIL_DELAY_MS);
    }
    lmb_msg_free(&m);
    assert(raw_recv(raw, &m));                         /* the tracker hangs up either way */
    raw_close(raw);
    return a;
}

/* ---- a scripted tracker, for what the joiner must refuse ------------------- */
enum { F_HONEST, F_ERR_AT_HELLO, F_SHORT_HELLO_R, F_BAD_VERSION, F_LOW_ORDER, F_SHORT_DONE, F_BAD_TAG,
       F_BAD_SEAL, F_OTHER_IDENTITY, F_BAD_SECRET, F_WRONG_CODE, F_ERR_AT_CONFIRM };
static _Atomic int fake_mode;
static char fake_code[LMB_PAIR_CODE_LEN + 1];

static void *fake_tracker(void *arg) {
    LmbSocket listener = (LmbSocket)(uintptr_t)arg;
    for (;;) {
        LmbSocket fd = lmb_socket_accept(listener, NULL, NULL);
        if (fd == LMB_INVALID_SOCKET) break;
        int mode = atomic_load(&fake_mode);
        LmbMsg m = {0}; uint8_t joiner[32], sid[LMB_PAIR_SID_LEN], scalar[32];
        uint8_t reply[LMB_PAIR_HELLO_R_LEN], done[LMB_PAIR_DONE_LEN], secret[LMB_PAIR_SECRET_LEN], other[32];
        LmbPairExchange exchange; LmbPairKeys keys;
        lmb_set_io_timeout(fd, 5000);
        assert(!lmb_secure_server(fd) && !lmb_secure_peer_id(fd, joiner));
        assert(!lmb_recv(fd, &m) && m.op == LMB_PAIR_HELLO && m.body_len == LMB_PAIR_HELLO_LEN);
        lmb_random(sid, LMB_PAIR_NONCE_LEN); memcpy(sid + LMB_PAIR_NONCE_LEN, m.body + 4, LMB_PAIR_NONCE_LEN);
        lmb_msg_free(&m);
        if (mode == F_ERR_AT_HELLO) { send_err(fd, "unknown op"); lmb_close(fd); continue; }
        lmb_random(scalar, sizeof scalar);
        lmb_pair_begin(&exchange, 1, mode == F_WRONG_CODE ? "ZZZZZZZZ" : fake_code, g_sec_pk, joiner, sid, scalar);
        lmb_put32(reply, mode == F_BAD_VERSION ? LMB_PAIR_VERSION + 1 : LMB_PAIR_VERSION);
        memcpy(reply + 4, sid, LMB_PAIR_NONCE_LEN); memcpy(reply + 4 + LMB_PAIR_NONCE_LEN, exchange.own, 32);
        if (mode == F_LOW_ORDER) { memset(reply + 4 + LMB_PAIR_NONCE_LEN, 0, 32); reply[4 + LMB_PAIR_NONCE_LEN] = 1; }
        assert(!lmb_send(fd, LMB_PAIR_HELLO_R, reply, mode == F_SHORT_HELLO_R ? sizeof reply - 1 : sizeof reply, NULL, 0));
        if (mode == F_SHORT_HELLO_R || mode == F_BAD_VERSION || mode == F_LOW_ORDER) {
            assert(lmb_recv(fd, &m));                  /* the joiner sends nothing more */
            lmb_close(fd); continue;
        }
        assert(!lmb_recv(fd, &m) && m.op == LMB_PAIR_CONFIRM && m.body_len == LMB_PAIR_CONFIRM_LEN);
        int low = lmb_pair_finish(&exchange, m.body, &keys);
        int proved = !low && lmb_pair_tag_equal(m.body + 32, keys.tag_joiner);
        lmb_msg_free(&m);
        assert(proved == (mode != F_WRONG_CODE));
        if (mode == F_ERR_AT_CONFIRM) { send_err(fd, "pairing failed"); lmb_close(fd); continue; }
        lmb_random(other, sizeof other);
        assert(!lmb_pair_secret_encode(secret, household_key, mode == F_OTHER_IDENTITY ? other : g_sec_pk));
        if (mode == F_BAD_SECRET) secret[0] = (uint8_t)(LMB_PAIR_VERSION + 1);
        memcpy(done, keys.tag_tracker, LMB_PAIR_TAG_LEN);
        lmb_pair_seal(&keys, secret, sizeof secret, done + LMB_PAIR_TAG_LEN, done + LMB_PAIR_TAG_LEN + LMB_PAIR_SECRET_LEN);
        if (mode == F_BAD_TAG) done[3] ^= 1;
        if (mode == F_BAD_SEAL) done[LMB_PAIR_TAG_LEN + 20] ^= 1;
        assert(!lmb_send(fd, LMB_PAIR_DONE, done, mode == F_SHORT_DONE ? sizeof done - 1 : sizeof done, NULL, 0));
        lmb_close(fd);
    }
    return NULL;
}

static int remove_entry(const char *path, const struct stat *st, int kind, struct FTW *walk) {
    (void)st; (void)walk; return kind == FTW_DP ? rmdir(path) : unlink(path);
}

int main(void) {
    char path[600];
    signal(SIGPIPE, SIG_IGN);
    snprintf(directory, sizeof directory, "/tmp/lumabri-pairing-test.XXXXXX");
    assert(mkdtemp(directory));
    snprintf(path, sizeof path, "%s/peer.key", directory);
    snprintf(known_hosts, sizeof known_hosts, "%s/known_hosts", directory);
    assert(!setenv("HOME", directory, 1) && !setenv("LUMABRI_PEER_KEY", path, 1) &&
           !setenv("LUMABRI_KNOWN_HOSTS", known_hosts, 1) && !setenv("LUMABRI_ENCRYPT", "1", 1) &&
           !setenv("LUMABRI_TOKEN", household_key, 1) && !unsetenv("LUMABRI_PEER_PINS") &&
           !unsetenv("LUMABRI_REQUIRE_PIN"));
    snprintf(g_token, sizeof g_token, "%s", household_key);
    assert(!lmb_secure_init() && lmb_secure_enabled());
    lmb_conn_gate_init(&g_conn_gate);
    int fake_port;
    LmbSocket listener = listen_loopback(&tracker_port), fake_listener = listen_loopback(&fake_port);
    snprintf(tracker_address, sizeof tracker_address, "127.0.0.1:%d", tracker_port);
    snprintf(fake_address, sizeof fake_address, "127.0.0.1:%d", fake_port);
    pthread_t acceptor, fake;
    assert(!pthread_create(&acceptor, NULL, accept_loop, (void *)(uintptr_t)listener));
    assert(!pthread_create(&fake, NULL, fake_tracker, (void *)(uintptr_t)fake_listener));

    Raw stranger, relay; raw_identity(&stranger); raw_identity(&relay);
    LmbPairOpened opened, replaced; LmbPairStatus status; LmbPairJoined joined; Attempt a;
    char typed[32], wrong[LMB_PAIR_CODE_LEN + 1];
    uint8_t hello[LMB_PAIR_HELLO_LEN], handle[LMB_PAIR_HANDLE_LEN], open_body[LMB_PAIR_OPEN_LEN];
    lmb_put32(hello, LMB_PAIR_VERSION); lmb_random(hello + 4, LMB_PAIR_NONCE_LEN);
    lmb_put32(handle, LMB_PAIR_VERSION); memset(handle + 4, 0, LMB_PAIR_ID_LEN);
    lmb_put32(open_body, LMB_PAIR_VERSION); lmb_put32(open_body + 4, 0);

    /* 1. Join with the real client. It pins nothing before the exchange
     *    succeeds and exactly the proven identity afterwards. */
    opened = open_code(0);
    status = status_of(&opened);
    assert(status.state == LMB_PAIR_WAITING && status.remaining_s > 290 && !status.failed);
    snprintf(path, sizeof path, "%s/joiner_known_hosts", directory);
    assert(!setenv("LUMABRI_KNOWN_HOSTS", path, 1));
    { uint8_t other[32]; lmb_random(other, sizeof other);   /* not the computer that was chosen */
      assert(lmb_pair_join(tracker_address, opened.code, other, &joined) == LMB_PAIR_E_IDENTITY); }
    assert(lmb_pair_join(tracker_address, "7K3M-9QX", NULL, &joined) == LMB_PAIR_E_CODE);
    assert(!file_has(path, tracker_address) && all_zero(&joined, sizeof joined));
    assert(!setenv("LUMABRI_KNOWN_HOSTS", known_hosts, 1));
    status = status_of(&opened); assert(status.state == LMB_PAIR_WAITING && !status.failed);   /* neither reached the code */
    assert(!setenv("LUMABRI_KNOWN_HOSTS", path, 1));
    snprintf(typed, sizeof typed, " %c%c%c%c-%c%c%c%c ", opened.code[0] | 0x20, opened.code[1], opened.code[2],
             opened.code[3], opened.code[4], opened.code[5], opened.code[6], opened.code[7] | 0x20);
    assert(lmb_pair_join(tracker_address, typed, g_sec_pk, &joined) == LMB_PAIR_OK);
    assert(!strcmp(joined.household_key, household_key) && !memcmp(joined.tracker_identity, g_sec_pk, 32));
    { char hex[65], line[160]; lmb_hex(hex, g_sec_pk, 32);
      snprintf(line, sizeof line, "%s %s\n", tracker_address, hex); assert(file_has(path, line)); }
    /* A used code cannot be used again, by the same computer or another. */
    assert(lmb_pair_join(tracker_address, opened.code, NULL, &joined) == LMB_PAIR_E_FAILED);
    assert(all_zero(&joined, sizeof joined));
    assert(!setenv("LUMABRI_KNOWN_HOSTS", known_hosts, 1));
    status = status_of(&opened);
    assert(status.state == LMB_PAIR_USED && !memcmp(status.joiner, g_sec_pk, 32) && !strcmp(status.source, "127.0.0.1"));
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    puts("  ok   join: code accepted once, tracker pinned only after proof");

    /* 2. Codes are managed only by the tracker's own identity, after AUTH. */
    raw_refused(&stranger, 0, LMB_PAIR_OPEN, open_body, sizeof open_body, "this swarm needs an invite token");
    raw_refused(&stranger, 0, LMB_PAIR_STATUS, handle, sizeof handle, "this swarm needs an invite token");
    raw_refused(&stranger, 1, LMB_PAIR_OPEN, open_body, sizeof open_body,
                "pairing codes are managed on the household owner's computer");
    memcpy(handle + 4, opened.id, LMB_PAIR_ID_LEN);
    raw_refused(&stranger, 1, LMB_PAIR_STATUS, handle, sizeof handle,
                "pairing codes are managed on the household owner's computer");
    raw_refused(&stranger, 1, LMB_PAIR_CANCEL, handle, sizeof handle,
                "pairing codes are managed on the household owner's computer");
    /* Frames of the exchange are not requests of their own. */
    raw_refused(&stranger, 1, LMB_PAIR_CONFIRM, hello, sizeof hello, "unknown op");
    raw_refused(&stranger, 0, LMB_PAIR_CONFIRM, hello, sizeof hello, "this swarm needs an invite token");
    /* Malformed first frames. */
    raw_refused(&stranger, 0, LMB_PAIR_HELLO, hello, sizeof hello - 1, "unsupported pairing version");
    raw_refused(&stranger, 0, LMB_PAIR_HELLO, hello, 0, "unsupported pairing version");
    lmb_put32(hello, LMB_PAIR_VERSION + 1);
    raw_refused(&stranger, 0, LMB_PAIR_HELLO, hello, sizeof hello, "unsupported pairing version");
    lmb_put32(hello, LMB_PAIR_VERSION);
    puts("  ok   owner operations need the household key and the tracker's identity");

    /* 3. A wrong code fails and is counted; the right one still works after
     *    it, from a computer with another identity. */
    advance(60000);
    opened = open_code(0);
    memcpy(wrong, opened.code, sizeof wrong); wrong[2] = wrong[2] == 'A' ? 'B' : 'A';
    a = attempt(&stranger, wrong, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    status = status_of(&opened);
    assert(status.state == LMB_PAIR_WAITING && status.failed == 1 && !strcmp(status.source, "127.0.0.1"));
    /* 4. Replay: a confirmation that was valid for another session of the
     *    same code and the same identities. */
    a = attempt(&stranger, opened.code, HANG_UP, NULL, NULL, NULL); assert(a.result == -1);
    for (int i = 0; i < 100 && status_of(&opened).failed < 2; i++) (void)poll(NULL, 0, 20);
    assert(status_of(&opened).failed == 2);              /* an abandoned exchange is a failed one */
    { uint8_t recorded[LMB_PAIR_CONFIRM_LEN]; memcpy(recorded, a.confirm, sizeof recorded);
      a = attempt(&stranger, opened.code, STALE_CONFIRM, NULL, NULL, recorded); assert(a.result == 0); }
    assert(status_of(&opened).failed == 3);
    /* 5. A relay: the joiner bound the relay's identity as the tracker's, or
     *    the tracker saw the relay's identity as the joiner's. */
    a = attempt(&relay, opened.code, NORMAL, relay.pk, stranger.pk, NULL); assert(a.result == 0);
    assert(status_of(&opened).failed == 4);
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL);
    assert(a.result == 1 && !strcmp(a.key, household_key));
    status = status_of(&opened);
    assert(status.state == LMB_PAIR_USED && status.failed == 4 && !memcmp(status.joiner, stranger.pk, 32));
    puts("  ok   wrong code, abandoned exchange, replayed confirmation and relay are refused and counted");

    /* 6. Each malformed confirmation is one failed attempt like any other,
     *    and the fifth failure destroys the code. */
    advance(60000);
    opened = open_code(0);
    static const int malformed[] = { TAMPER_PUBLIC, LOW_ORDER_PUBLIC, SHORT_CONFIRM, LONG_CONFIRM, OTHER_OP };
    for (unsigned i = 0; i < 5; i++) {
        assert(status_of(&opened).state == LMB_PAIR_WAITING);
        a = attempt(&stranger, opened.code, malformed[i], NULL, NULL, NULL); assert(a.result == 0);
        assert(status_of(&opened).failed == i + 1);
    }
    status = status_of(&opened); assert(status.state == LMB_PAIR_LOCKED && status.failed == 5);
    advance(60000);                                       /* not a matter of rate */
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    assert(lmb_pair_join(tracker_address, opened.code, NULL, &joined) == LMB_PAIR_E_FAILED);
    status = status_of(&opened); assert(status.state == LMB_PAIR_LOCKED && status.failed == 5);
    puts("  ok   tampered, low-order and truncated frames; the sixth attempt finds the code destroyed");

    /* 7. Rate per source address: over the rate, even the right code meets a
     *    decoy, and the attempt is not charged to the code. */
    advance(60000);
    for (unsigned i = 0; i < LMB_PAIR_RATE_BURST; i++) {
        a = attempt(&stranger, "00000000", NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    }
    opened = open_code(0);
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    status = status_of(&opened); assert(status.state == LMB_PAIR_WAITING && !status.failed);
    advance(LMB_PAIR_RATE_REFILL_MS + 100);
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 1);
    puts("  ok   attempts over the rate never reach the code");

    /* 8. Lifetime, withdrawal, replacement. */
    advance(60000);
    opened = open_code(2);
    assert(opened.lifetime_s == 2 && status_of(&opened).state == LMB_PAIR_WAITING);
    advance(2000);
    assert(status_of(&opened).state == LMB_PAIR_EXPIRED);
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    opened = open_code(100000); assert(opened.lifetime_s == LMB_PAIR_LIFETIME_S);   /* never longer than the default */
    assert(!lmb_pair_owner_cancel(tracker_address, opened.id));
    assert(status_of(&opened).state == LMB_PAIR_CANCELLED);
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    opened = open_code(0); replaced = open_code(0);
    assert(status_of(&opened).state == LMB_PAIR_UNKNOWN && status_of(&replaced).state == LMB_PAIR_WAITING);
    assert(lmb_pair_owner_cancel(tracker_address, opened.id) == LMB_PAIR_E_REFUSED);
    a = attempt(&stranger, opened.code, NORMAL, NULL, NULL, NULL); assert(a.result == 0);
    a = attempt(&stranger, replaced.code, NORMAL, NULL, NULL, NULL); assert(a.result == 1);
    puts("  ok   expired, withdrawn and replaced codes do not work");

    /* 9. An address already pinned to another identity: the exchange is not
     *    allowed to overrule the pin, and nothing is returned. */
    advance(60000);
    opened = open_code(0);
    snprintf(path, sizeof path, "%s/pinned_known_hosts", directory);
    { FILE *f = fopen(path, "w"); char hex[65]; assert(f); lmb_hex(hex, stranger.pk, 32);
      fprintf(f, "%s %s\n", tracker_address, hex); assert(!fclose(f) && !chmod(path, 0600)); }
    assert(!setenv("LUMABRI_KNOWN_HOSTS", path, 1));
    assert(lmb_pair_join(tracker_address, opened.code, NULL, &joined) == LMB_PAIR_E_PINNED);
    assert(all_zero(&joined, sizeof joined));
    assert(!setenv("LUMABRI_KNOWN_HOSTS", known_hosts, 1));
    puts("  ok   an existing pin for the address is not overruled");

    /* 10. The joiner against a scripted tracker. Only the honest script ends
     *     with a key and a pin. */
    static const struct { int mode, want; } scripts[] = {
        { F_ERR_AT_HELLO, LMB_PAIR_E_UNSUPPORTED }, { F_SHORT_HELLO_R, LMB_PAIR_E_PROTOCOL },
        { F_BAD_VERSION, LMB_PAIR_E_PROTOCOL }, { F_LOW_ORDER, LMB_PAIR_E_FAILED },
        { F_SHORT_DONE, LMB_PAIR_E_PROTOCOL }, { F_BAD_TAG, LMB_PAIR_E_FAILED },
        { F_BAD_SEAL, LMB_PAIR_E_FAILED }, { F_OTHER_IDENTITY, LMB_PAIR_E_IDENTITY },
        { F_BAD_SECRET, LMB_PAIR_E_PROTOCOL }, { F_WRONG_CODE, LMB_PAIR_E_FAILED },
        { F_ERR_AT_CONFIRM, LMB_PAIR_E_FAILED }, { F_HONEST, LMB_PAIR_OK },
    };
    snprintf(path, sizeof path, "%s/fake_known_hosts", directory);
    assert(!setenv("LUMABRI_KNOWN_HOSTS", path, 1));
    lmb_pair_code_new(fake_code);
    for (size_t i = 0; i < sizeof scripts / sizeof *scripts; i++) {
        atomic_store(&fake_mode, scripts[i].mode);
        int rc = lmb_pair_join(fake_address, fake_code, NULL, &joined);
        if (rc != scripts[i].want) { printf("script %d: got %d, want %d\n", scripts[i].mode, rc, scripts[i].want); return 1; }
        if (rc) assert(all_zero(&joined, sizeof joined) && !file_has(path, fake_address));
        else assert(!strcmp(joined.household_key, household_key) && file_has(path, fake_address));
    }
    puts("  ok   joiner refuses short, altered, low-order, unproven and misattributed replies");

    /* shutdown, not close, is what wakes a thread blocked in accept. */
    (void)shutdown(listener, SHUT_RDWR); (void)shutdown(fake_listener, SHUT_RDWR);
    assert(!pthread_join(acceptor, NULL) && !pthread_join(fake, NULL));
    lmb_socket_close(listener); lmb_socket_close(fake_listener);
    for (int i = 0; i < 100 && atomic_load(&g_conn_gate.active); i++) (void)poll(NULL, 0, 20);
    assert(!atomic_load(&g_conn_gate.active) && !atomic_load(&lmb_rx_inflight));
    assert(!nftw(directory, remove_entry, 16, FTW_DEPTH | FTW_PHYS));
    puts("pairing tracker: PASS");
    return 0;
}
