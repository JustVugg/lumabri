/* This is an ordering proof, not a fragile elapsed-time benchmark: both
 * remote workers must receive EXEC before local compute starts, and neither
 * may reply until the local callback has started. A serial implementation
 * times out instead of passing because the machine happened to be fast. */
#define LMBE_ENGINE_ID "hybrid-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include "lumabri_client.h"

enum { DIM = 4, EXPERTS = 4 };
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int received, local_started, local_calls, fail_local, bad_reply;
    struct timespec deadline;
} Gate;
typedef struct { Gate *gate; int fd, expert; } Remote;

static float contribution(int eid) { return eid == 3 ? 1.f : 0x1p-24f; }
static int resident_local(void *opaque, int layer, int expert, const float *x, int D, float *out) {
    int *calls = opaque; assert(layer == 0 && x && D == DIM); (*calls)++;
    for (int d = 0; d < D; d++) out[d] = contribution(expert);
    return 0;
}
static void *different_remote(void *opaque) {
    int fd = *(int *)opaque; LmbMsg msg = {0};
    assert(!lmb_recv(fd, &msg) && msg.op == LMB_HOME_EXPERT);
    float out[DIM]; for (int i = 0; i < DIM; i++) out[i] = 9.f;
    assert(!lmb_send(fd, LMB_EXEC_R, NULL, 0, out, sizeof out));
    lmb_msg_free(&msg); close(fd); return NULL;
}

static void *worker(void *opaque) {
    Remote *r = opaque;
    LmbMsg msg = {0};
    assert(!lmb_recv(r->fd, &msg));
    assert(msg.op == LMB_EXEC && msg.body_len == 16);
    assert(lmb_get32(msg.body + 4) == (unsigned)r->expert);
    assert(lmb_get32(msg.body + 8) == DIM && lmb_get32(msg.body + 12) == 1);
    lmb_msg_free(&msg);
    Gate *g = r->gate;
    pthread_mutex_lock(&g->lock);
    g->received++;
    pthread_cond_broadcast(&g->changed);
    while (!g->local_started)
        assert(!pthread_cond_timedwait(&g->changed, &g->lock, &g->deadline));
    pthread_mutex_unlock(&g->lock);
    float out[DIM];
    for (int d = 0; d < DIM; d++) out[d] = contribution(r->expert);
    /* Callback failure is allowed to close every in-flight socket. */
    int rc = lmb_send(r->fd, LMB_EXEC_R, NULL, 0, out,
                      g->bad_reply ? sizeof(float) : sizeof out);
    if (!g->fail_local && !g->bad_reply) assert(!rc);
    close(r->fd);
    return NULL;
}

static int local(void *opaque, int layer, int expert,
                 const float *x, int D, float *out) {
    Gate *g = opaque;
    assert(layer == 0 && D == DIM && x && (expert == 1 || expert == 2));
    pthread_mutex_lock(&g->lock);
    while (g->received != 2)
        assert(!pthread_cond_timedwait(&g->changed, &g->lock, &g->deadline));
    g->local_started = 1;
    g->local_calls++;
    pthread_cond_broadcast(&g->changed);
    pthread_mutex_unlock(&g->lock);
    for (int d = 0; d < D; d++) out[d] = contribution(expert);
    return g->fail_local ? -1 : 0;
}

static void run_case(int fail_local, int bad_reply) {
    memset(&L, 0, sizeof L);
    L.n_layers = 1; L.n_experts = EXPERTS; L.hidden = DIM; L.npeers = 2;
    L.hedge_ms = -1; L.exec_wait_ms = 5000;
    L.own = malloc(EXPERTS * LUMI_MAX_REP * sizeof *L.own);
    assert(L.own);
    for (int i = 0; i < EXPERTS * LUMI_MAX_REP; i++) L.own[i] = -1;
    Gate gate = { .lock = PTHREAD_MUTEX_INITIALIZER,
                  .changed = PTHREAD_COND_INITIALIZER,
                  .fail_local = fail_local, .bad_reply = bad_reply };
    assert(!clock_gettime(CLOCK_REALTIME, &gate.deadline));
    gate.deadline.tv_sec += 5;
    int idx[EXPERTS] = {3, 0, 1, 2};
    float weights[EXPERTS] = {1, 1, 1, 1};
    float x[DIM] = {0}, out[DIM] = {0}, before[DIM], expected[DIM];
    if (fail_local || bad_reply)
        for (int d = 0; d < DIM; d++) out[d] = (float)(2 + d);
    memcpy(before, out, sizeof out); memcpy(expected, out, sizeof out);
    for (int k = 0; k < EXPERTS; k++)
        for (int d = 0; d < DIM; d++) expected[d] += weights[k] * contribution(idx[k]);
    pthread_t threads[2]; Remote remote[2];
    for (int i = 0; i < 2; i++) {
        int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
        lmb_set_io_timeout(pair[0], 5000); lmb_set_io_timeout(pair[1], 5000);
        L.peers[i].socks[0] = pair[0]; L.peers[i].nsocks = 1;
        L.peers[i].rtt_us = 100;
        snprintf(L.peers[i].addr, sizeof L.peers[i].addr, "fixture-%d", i);
        L.own[idx[i] * LUMI_MAX_REP] = i;
        remote[i] = (Remote){&gate, pair[1], idx[i]};
        assert(!pthread_create(&threads[i], NULL, worker, &remote[i]));
    }
    int ok = lumi_moe_apply_split(0, idx, weights, EXPERTS, x, DIM, out, 2, local, &gate);
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(gate.local_started && gate.received == 2);
    if (fail_local || bad_reply) {
        assert(!ok && !memcmp(out, before, sizeof out));
        assert(!L.hybrid_rounds && !L.calls);
    } else {
        assert(ok && !memcmp(out, expected, sizeof out));
        float wrong = 0;
        for (int k = 0; k < EXPERTS; k++) wrong += contribution(k);
        assert(memcmp(&wrong, expected, sizeof wrong)); /* order-sensitive fixture */
        assert(gate.local_calls == 2 && L.hybrid_local_calls == 2);
        assert(L.hybrid_rounds == 1 && L.calls == 2);
    }
    for (int i = 0; i < 2; i++) {
        assert(!L.peers[i].inflight);
        while (L.peers[i].nsocks) close(L.peers[i].socks[--L.peers[i].nsocks]);
    }
    free(L.own); L.own = NULL;
    pthread_cond_destroy(&gate.changed); pthread_mutex_destroy(&gate.lock);
}

int main(void) {
    LmbHybridTiming timing = {0};
    for (unsigned i = 0; i < 8; i++) {
        int split = lmb_hybrid_choose(&timing, LMB_HYBRID_ADAPTIVE);
        assert(split == !(i & 1));
        lmb_hybrid_observe(&timing, split, split ? .030 : .004, 0, .003, 0);
    }
    assert(!lmb_hybrid_choose(&timing, LMB_HYBRID_ADAPTIVE));
    assert(lmb_hybrid_choose(&timing, LMB_HYBRID_FORCE_SPLIT));
    assert(!lmb_hybrid_choose(&timing, LMB_HYBRID_FORCE_LOCAL));
    timing.rounds = 64;
    assert(lmb_hybrid_choose(&timing, LMB_HYBRID_ADAPTIVE));
    timing.rounds = 65; timing.seconds[1] = .002;
    assert(lmb_hybrid_choose(&timing, LMB_HYBRID_ADAPTIVE));
    lmb_hybrid_observe(&timing, 1, NAN, 0, 0, 0); assert(timing.rounds == 65);
    signal(SIGPIPE, SIG_IGN);
    setenv("LUMABRI_EXEC_FALLBACK_LOCAL", "1", 1);
    unsetenv("LUMABRI_TRACKER");
    run_case(0, 0); run_case(1, 0); run_case(0, 1);
    /* An approved slower accelerator stays resident while the exact same
     * callback handles all experts locally; no socket is required. */
    memset(&L, 0, sizeof L);
    L.home.count = 1; L.n_layers = 1; L.n_experts = EXPERTS; L.hidden = DIM;
    L.hybrid_policy = LMB_HYBRID_FORCE_LOCAL;
    int all_idx[4] = {3, 0, 1, 2}, calls = 0;
    float all_x[DIM] = {0}, all_w[4] = {1,1,1,1}, all_out[DIM] = {0};
    assert(lumi_moe_apply_split(0, all_idx, all_w, 4, all_x, DIM, all_out, 3, resident_local, &calls));
    float expected = 0; for (int i = 0; i < 4; i++) expected += contribution(all_idx[i]);
    for (int d = 0; d < DIM; d++) assert(!memcmp(&all_out[d], &expected, sizeof expected));
    assert(calls == 4 && L.calls == 0 && L.hybrid_timing[0].samples[0] == 1);
    L.hybrid_policy = LMB_HYBRID_FORCE_SPLIT;
    L.npeers = 1; L.exec_wait_ms = 5000; L.hedge_ms = -1;
    L.own = malloc(EXPERTS * LUMI_MAX_REP * sizeof *L.own); assert(L.own);
    for (int i = 0; i < EXPERTS * LUMI_MAX_REP; i++) L.own[i] = -1;
    L.own[3 * LUMI_MAX_REP] = 0;
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    L.peers[0].socks[0] = pair[0]; L.peers[0].nsocks = 1;
    pthread_t mismatched; assert(!pthread_create(&mismatched, NULL, different_remote, &pair[1]));
    memset(all_out, 0, sizeof all_out);
    assert(lumi_moe_apply_split(0, all_idx, all_w, 4, all_x, DIM, all_out, 3, resident_local, &calls));
    assert(!pthread_join(mismatched, NULL));
    assert(L.hybrid_numeric_failed && L.calls == 1);
    L.on = 1;
    assert(!lumi_layer_on(0)); /* native engine path, without callback overhead */
    for (int d = 0; d < DIM; d++) assert(!memcmp(&all_out[d], &expected, sizeof expected));
    /* Even forced split cannot reuse an observed incompatible path. */
    memset(all_out, 0, sizeof all_out);
    assert(lumi_moe_apply_split(0, all_idx, all_w, 4, all_x, DIM, all_out, 3, resident_local, &calls));
    assert(L.calls == 1);
    for (int d = 0; d < DIM; d++) assert(!memcmp(&all_out[d], &expected, sizeof expected));
    while (L.peers[0].nsocks) close(L.peers[0].socks[--L.peers[0].nsocks]);
    free(L.own);
    memset(&L, 0, sizeof L);
    int idx[4] = {0, 1, 2, 3};
    float x[4] = {0}, w[4] = {1, 1, 1, 1}, out[4] = {0};
    assert(!lumi_moe_apply_split(0, idx, w, 4, x, 4, out, 4, local, NULL));
    assert(!lumi_moe_apply_split(0, idx, w, 4, x, 4, out, -1, local, NULL));
    assert(!lumi_moe_apply_split(0, idx, w, 4, x, 4, out, 2, NULL, NULL));
    assert(!lumi_moe_apply_split(-1, idx, w, 4, x, 4, out, 2, local, NULL));
    idx[0] = 4;
    assert(!lumi_moe_apply_split(0, idx, w, 4, x, 4, out, 2, local, NULL));
    const char *bad[] = {"", "0", "-1", "4", "2x", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        setenv("LUMABRI_HYBRID_LOCAL_EXPERTS", bad[i], 1);
        assert(!lumi_hybrid_local_count(4));
    }
    setenv("LUMABRI_HYBRID_LOCAL_EXPERTS", "2", 1);
    assert(lumi_hybrid_local_count(4) == 2);
    puts("Hybrid parallel: concurrent dispatch/local execution, canonical sum, failure cleanup OK");
    return 0;
}
