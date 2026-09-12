#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

typedef struct {
    int fd;
    _Atomic int stop, pulses, expired;
    LmbHomeTransaction transaction;
} MonitorDonor;

typedef struct {
    HomeDonor donor;
    int fd, rc;
    _Atomic int done;
} IdleOffer;

static void *idle_offer_server(void *arg) {
    IdleOffer *offer = arg;
    offer->rc = home_donor_offer(&offer->donor, offer->fd, "127.0.0.1:1", 0, 0);
    atomic_store(&offer->done, 1);
    return NULL;
}

static void test_authenticated_idle_offer(void) {
    /* Real encrypted handshake and AUTH, then no OFFER. An abandoned
     * catalogue preflight must not monopolize the donor's control loop
     * for the bulk-transfer timeout. No files, weights or LAN are used. */
    unsetenv("LUMABRI_IO_TIMEOUT_MS");
    setenv("LUMABRI_TOKEN", "monitor-test-only", 1);
    uint8_t seed[32] = {0x35};
    lmb_sign_keypair(g_sec_pk, g_sec_sk, seed);
    lmb_enc_wrap = lmb_sec_wrap_hook;
    lmb_enc_send = lmb_sec_send_hook;
    lmb_enc_recv = lmb_sec_recv_hook;
    lmb_enc_forget = lmb_sec_forget_hook;
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    IdleOffer offer = {.fd = pair[1], .donor = {.client = -1}};
    pthread_t server;
    assert(!pthread_create(&server, NULL, idle_offer_server, &offer));
    LmbSecure client;
    assert(!lmb_secure_handshake(pair[0], 1, g_sec_sk, g_sec_pk, &client));
    lmb_set_io_timeout(pair[0], 3000);
    LmbBuf auth = {0}; assert(!lmb_buf_str(&auth, "monitor-test-only"));
    assert(!lmb_secure_send(&client, pair[0], LMB_AUTH, auth.p, (uint32_t)auth.len, NULL, 0));
    free(auth.p);
    LmbMsg ack = {0}; assert(!lmb_secure_recv(&client, pair[0], &ack));
    assert(ack.op == LMB_OK); lmb_msg_free(&ack);
    double deadline = nowd() + 4;
    while (!atomic_load(&offer.done) && nowd() < deadline) (void)poll(NULL, 0, 20);
    int bounded = atomic_load(&offer.done);
    if (!bounded) shutdown(pair[0], SHUT_RDWR); /* bounded failing test too */
    pthread_join(server, NULL);
    struct timeval timeout; socklen_t size = sizeof timeout;
    assert(!getsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, &size));
    assert(bounded && offer.rc && offer.donor.client == -1);
    assert(timeout.tv_sec <= 2); /* allow platform timeout rounding */
    lmb_close(pair[0]); lmb_close(pair[1]);
    puts("HOME IDLE OFFER: PASS (encrypted authenticated preflight bounded; no weights)");
}

static void *monitor_donor(void *arg) {
    MonitorDonor *d = arg;
    while (!atomic_load(&d->stop)) {
        struct pollfd p = {d->fd, POLLIN, 0};
        int ready = poll(&p, 1, 100); assert(ready >= 0);
        if (ready) {
            LmbMsg m = {0}; assert(!lmb_recv(d->fd, &m));
            assert(m.op == LMB_HOME_PULSE && m.body_len == 32 &&
                   !memcmp(m.body, d->transaction.offer.id, 32));
            lmb_msg_free(&m);
            d->transaction.last_seen_ms = (uint64_t)(nowd() * 1000);
            atomic_fetch_add(&d->pulses, 1);
            assert(!home_status_send(d->fd, &d->transaction, 0, 0));
        }
        if (lmb_home_expired(&d->transaction, (uint64_t)(nowd() * 1000)))
            atomic_store(&d->expired, 1);
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    HomeSession s = HOME_SESSION_INIT;
    MonitorDonor donors[2] = {0};
    pthread_t donor_threads[2], monitor;
    s.count = 2;
    for (unsigned i = 0; i < s.count; i++) {
        int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
        s.fd[i] = pair[0]; donors[i].fd = pair[1];
        lmb_set_io_timeout(pair[0], 500); lmb_set_io_timeout(pair[1], 500);
        s.offers[i].id[0] = donors[i].transaction.offer.id[0] = (uint8_t)(i + 1);
        snprintf(s.addresses[i], sizeof s.addresses[i], "donor-%u", i);
        donors[i].transaction.phase = LMB_HOME_LOADING;
        donors[i].transaction.reservation_held = 1;
        donors[i].transaction.last_seen_ms = (uint64_t)(nowd() * 1000);
        assert(!pthread_create(&donor_threads[i], NULL, monitor_donor, &donors[i]));
    }
    assert(!pthread_create(&monitor, NULL, home_session_keepalive, &s));
    /* Foreground makes no UI/socket progress for a lease plus margin. */
    double until = nowd() + LMB_HOME_LEASE_MS / 1000.0 + 2;
    while (nowd() < until) (void)poll(NULL, 0, 100);
    assert(!atomic_load(&s.failed));
    LmbHomePhase phases[LMB_CLUSTER_MAX_NODES];
    uint32_t ports[LMB_CLUSTER_MAX_NODES];
    home_session_snapshot(&s, phases, ports);
    atomic_store(&s.stop, 1); pthread_join(monitor, NULL);
    for (unsigned i = 0; i < s.count; i++) {
        atomic_store(&donors[i].stop, 1); pthread_join(donor_threads[i], NULL);
        assert(!atomic_load(&donors[i].expired) && atomic_load(&donors[i].pulses) >= 14);
        assert(phases[i] == LMB_HOME_LOADING);
    }
    lmb_home_released(&donors[0].transaction, LMB_HOME_CLOSED,
                      "Request lease expired before preparation completed.");
    assert(!home_status_send(donors[0].fd, &donors[0].transaction, 0, 0));
    assert(home_session_receive(&s, 0));
    assert(strstr(s.reason[0], "Request lease expired"));
    close(donors[1].fd);
    assert(home_session_receive(&s, 1));
    assert(strstr(s.reason[1], "donor-1") && strstr(s.reason[1], "connection closed"));
    close(donors[0].fd); close(s.fd[0]); close(s.fd[1]);
    pthread_mutex_destroy(&s.status_lock); pthread_mutex_destroy(&s.send_lock);
    puts("HOME MONITOR: PASS (two leases survive a paused UI; terminal reasons preserved)");
    test_authenticated_idle_offer();
    return 0;
}
