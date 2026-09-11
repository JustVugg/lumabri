#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

typedef struct {
    int fd;
    _Atomic int stop, pulses, expired;
    LmbHomeTransaction transaction;
} MonitorDonor;

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
    return 0;
}
