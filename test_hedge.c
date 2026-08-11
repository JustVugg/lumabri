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
    int bad = fd < 0 || lumi_send_exec(fd, 0, 0, x, 4, 3, NULL);
    LumiPeer *from = NULL;
    float *out = bad ? NULL : lumi_finish_exec(0, 0, x, 4, 3, NULL, fd,
                                                &L.peers[0], &tried, &from);
    bad |= !out || memcmp(out, x, sizeof x) || from != &L.peers[1] ||
           L.hedges != 1 || L.hedge_wins != 1;
    free(out); free(L.own);
    void *ra = NULL, *rb = NULL;
    pthread_join(a, &ra); pthread_join(b, &rb);
    bad |= (intptr_t)ra || (intptr_t)rb;
    if (bad) { fputs("hedged request test failed\n", stderr); return 1; }
    puts("HEDGED EXEC: PASS (3-row batch, fast replica won after 20 ms)");
    return 0;
}
