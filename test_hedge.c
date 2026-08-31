#define LMBE_ENGINE_ID "hedge-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <pthread.h>
#include "lumabri_client.h"

typedef struct { int port, delay_ms; } Echo;

static void *echo_server(void *arg) {
    Echo *e = (Echo *)arg;
    int lfd = lmb_listen(e->port);
    if (lfd < 0) return (void *)1;
    int fd = accept(lfd, NULL, NULL);
    LmbMsg m = {0};
    int bad = fd < 0 || lmb_recv(fd, &m) || m.op != LMB_EXEC || m.body_len < 16;
    if (!bad) {
        if (e->delay_ms) usleep((useconds_t)e->delay_ms * 1000u);
        /* The slow replica answers into a socket the hedge already closed.
         * That send failing with EPIPE is the mechanism working — the loser's
         * late frame must never be mistaken for a reply — so only the fast
         * replica's send is required to succeed. */
        int sent = lmb_send(fd, LMB_EXEC_R, NULL, 0, m.pay, m.pay_len);
        bad = sent != 0 && !e->delay_ms;
    }
    lmb_msg_free(&m);
    if (fd >= 0) close(fd);
    close(lfd);
    return (void *)(intptr_t)(bad != 0);
}

int main(void) {
    Echo slow = {7564, 150}, fast = {7565, 0};
    pthread_t a, b;
    pthread_create(&a, NULL, echo_server, &slow);
    pthread_create(&b, NULL, echo_server, &fast);
    usleep(100000);

    L.n_layers = 1; L.n_experts = 1; L.hidden = 4; L.npeers = 2;
    L.hedge_ms = 20;
    L.own = (int *)malloc(LUMI_MAX_REP * sizeof(int));
    for (int i = 0; i < LUMI_MAX_REP; i++) L.own[i] = -1;
    L.own[0] = 0; L.own[1] = 1;
    snprintf(L.peers[0].addr, sizeof L.peers[0].addr, "127.0.0.1:%d", slow.port);
    snprintf(L.peers[1].addr, sizeof L.peers[1].addr, "127.0.0.1:%d", fast.port);
    L.peers[0].rtt_us = 100; L.peers[1].rtt_us = 200;

    float x[12]; for (int i = 0; i < 12; i++) x[i] = (float)i / 7.0f;
    int fd = lumi_take_sock(&L.peers[0]);
    uint32_t tried = 1;
    double started = lumi_now();
    int bad = fd < 0 || lumi_send_exec(fd, 0, 0, x, 4, 3, NULL);
    LumiPeer *from = NULL;
    float *out = bad ? NULL : lumi_finish_exec(0, 0, x, 4, 3, NULL, fd,
                                                &L.peers[0], started,
                                                &tried, &from);
    bad |= !out || memcmp(out, x, sizeof x) || from != &L.peers[1] ||
           L.hedges != 1 || L.hedge_wins != 1;
    free(out);
    void *ra = NULL, *rb = NULL;
    pthread_join(a, &ra); pthread_join(b, &rb);
    bad |= (intptr_t)ra || (intptr_t)rb;

    /* hedge_ms < 0 is off: the slow primary must answer alone. Without a way
     * to say that, a measurement harness cannot tell a chosen replica from a
     * hedge copy, which is exactly what attribution work needs. */
    Echo slow_off = {7566, 150}, fast_off = {7567, 0};
    pthread_t c, d;
    pthread_create(&c, NULL, echo_server, &slow_off);
    pthread_create(&d, NULL, echo_server, &fast_off);
    usleep(100000);
    for (int i = 0; i < 2; i++)
        while (L.peers[i].nsocks > 0) close(L.peers[i].socks[--L.peers[i].nsocks]);
    snprintf(L.peers[0].addr, sizeof L.peers[0].addr,
             "127.0.0.1:%d", slow_off.port);
    snprintf(L.peers[1].addr, sizeof L.peers[1].addr,
             "127.0.0.1:%d", fast_off.port);
    /* Put the primary's predictor where the automatic policy *would* hedge:
     * enough samples and a tail well past 1.5x the mean. Without that the
     * automatic branch returns zero on its own and the case proves nothing. */
    lmb_predict_init(&L.peers[0].latency, 1000);
    for (int i = 0; i < 8; i++) lmb_predict_observe(&L.peers[0].latency, 1000);
    /* One tail spike, then fast replies: the mean falls by eighths while the
     * p95 falls by thirty-seconds, which is precisely the shape the automatic
     * policy is meant to hedge against. */
    lmb_predict_observe(&L.peers[0].latency, 400000);
    for (int i = 0; i < 10; i++) lmb_predict_observe(&L.peers[0].latency, 1000);
    if (lmb_predict_hedge_ms(&L.peers[0].latency) == 0) {
        fputs("hedge fixture did not arm the automatic policy\n", stderr);
        return 1;
    }
    L.hedge_ms = -1;
    unsigned long long hedges_before = L.hedges;
    int off_fd = lumi_take_sock(&L.peers[0]);
    uint32_t off_tried = 1;
    double off_started = lumi_now();
    bad |= off_fd < 0 || lumi_send_exec(off_fd, 0, 0, x, 4, 3, NULL);
    LumiPeer *off_from = NULL;
    float *off_out = bad ? NULL
                         : lumi_finish_exec(0, 0, x, 4, 3, NULL, off_fd,
                                            &L.peers[0], off_started,
                                            &off_tried, &off_from);
    bad |= !off_out || memcmp(off_out, x, sizeof x) ||
           off_from != &L.peers[0] || L.hedges != hedges_before;
    free(off_out);
    /* The fast replica stays listening on purpose: had a hedge fired it would
     * have been counted. Since it must not, its accept is still pending, so
     * release it with one throwaway connection before joining. */
    int wake = lmb_connect(L.peers[1].addr);
    if (wake >= 0) close(wake);
    void *rc = NULL, *rd = NULL;
    pthread_join(c, &rc); pthread_join(d, &rd);
    (void)rd;                       /* the woken replica saw no valid request */
    bad |= (intptr_t)rc;

    free(L.own);
    if (bad) { fputs("hedged request test failed\n", stderr); return 1; }
    puts("HEDGED EXEC: PASS (3-row batch, fast replica won after 20 ms; "
         "hedge_ms<0 leaves the primary alone)");
    return 0;
}
