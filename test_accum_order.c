/* The order the contributions are summed in belongs to the model, never to
 * the network.
 *
 * Floating point addition is not associative. The same K experts, with the
 * same weights, summed in a different sequence give a different number — not
 * always, but often enough that a generation drifts a few tokens later, and
 * often enough that a speculative decoder starts rejecting its own drafter.
 * The obvious optimisation, "accumulate each reply the moment it lands so
 * the slow peer costs nothing", is exactly the bug: the sequence would then
 * be whatever the network happened to deliver first, and would change from
 * run to run on the same prompt.
 *
 * Both remote MoE paths therefore issue every request first and accumulate
 * afterwards, in the order the engine itself would have used:
 *
 *   - the per-position path sums k = 0..K-1, the router's order;
 *   - the batched path sums the experts in the order the union was built,
 *     first appearance while scanning positions then k — which is what the
 *     local engine does, and is NOT ascending expert id.
 *
 * This test pins both down. The nodes are made to answer in an order that
 * differs from the issue order, and the output must be bit-identical to a
 * run where they answer immediately. To prove the assertion has teeth, the
 * test also folds the same contributions in arrival order and in ascending
 * id, and requires those to differ from what the client produced: with
 * order-insensitive values every claim here would pass vacuously. */
#define LMBE_ENGINE_ID "accum-order-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <math.h>
#include "lumabri_client.h"

#define DIM   4
#define NEXP  4

/* One heavy contribution and three at half an ulp of it. Summed after the
 * heavy one each rounds away and the total stays 1.0; summed before it they
 * add up first and the total is two ulps larger. That is the whole point:
 * these are the values where the order is visible in the bits. */
static float contrib(int eid) { return eid == 3 ? 1.0f : ldexpf(1.0f, -24); }

typedef struct { int port, delay_ms; } Node;

static _Atomic int g_listening;
static _Atomic int g_stop;

static void *node_main(void *arg) {
    Node *n = (Node *)arg;
    int lfd = lmb_listen(n->port);
    if (lfd < 0) return (void *)1;
    atomic_fetch_add(&g_listening, 1);
    int bad = 0;
    while (!atomic_load(&g_stop)) {
        struct pollfd lp = { lfd, POLLIN, 0 };
        if (poll(&lp, 1, 50) <= 0) continue;
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        for (;;) {
            struct pollfd cp = { fd, POLLIN, 0 };
            int pr = poll(&cp, 1, 50);
            if (pr < 0) break;
            if (pr == 0) { if (atomic_load(&g_stop)) break; continue; }
            LmbMsg m = {0};
            if (lmb_recv(fd, &m) || m.op != LMB_EXEC || m.body_len < 16) {
                lmb_msg_free(&m);
                break;                 /* peer closed, or a dialect we refuse */
            }
            uint32_t eid = lmb_get32(m.body + 4);
            uint32_t d   = lmb_get32(m.body + 8);
            uint32_t nr  = lmb_get32(m.body + 12);
            lmb_msg_free(&m);
            if (d != DIM || nr < 1 || nr > 8 || eid >= NEXP) { bad = 1; break; }
            float h[DIM * 8];
            for (uint32_t i = 0; i < nr * d; i++) h[i] = contrib((int)eid);
            if (n->delay_ms) usleep((useconds_t)n->delay_ms * 1000u);
            if (lmb_send(fd, LMB_EXEC_R, NULL, 0, h,
                         (uint32_t)(nr * d * sizeof(float)))) { bad = 1; break; }
        }
        close(fd);
    }
    close(lfd);
    return (void *)(intptr_t)bad;
}

/* fold the contributions of `order` into one value, the way the accumulation
 * loop does: out starts at zero and takes w * h one expert at a time */
static float fold(const int *order, const float *w, int n) {
    float acc = 0.0f;
    for (int i = 0; i < n; i++) acc += w[i] * contrib(order[i]);
    return acc;
}

static int same_bits(const float *a, const float *b, int n) {
    return memcmp(a, b, (size_t)n * sizeof(float)) == 0;
}

int main(void) {
    Node nodes[NEXP];
    pthread_t th[NEXP];
    for (int i = 0; i < NEXP; i++) {
        nodes[i].port = 7590 + i;
        nodes[i].delay_ms = 0;
        pthread_create(&th[i], NULL, node_main, &nodes[i]);
    }
    for (int spins = 0; atomic_load(&g_listening) < NEXP && spins < 500; spins++)
        usleep(10000);

    L.n_layers = 1; L.n_experts = NEXP; L.hidden = DIM; L.npeers = NEXP;
    L.hedge_ms = -1;                    /* one replica each: never hedge */
    L.own = (int *)malloc((size_t)NEXP * LUMI_MAX_REP * sizeof(int));
    for (int i = 0; i < NEXP * LUMI_MAX_REP; i++) L.own[i] = -1;
    for (int e = 0; e < NEXP; e++) {
        L.own[(size_t)e * LUMI_MAX_REP] = e;
        snprintf(L.peers[e].addr, sizeof L.peers[e].addr,
                 "127.0.0.1:%d", nodes[e].port);
        L.peers[e].rtt_us = 100;
    }

    float x[DIM]; for (int i = 0; i < DIM; i++) x[i] = (float)i / 3.0f;
    int bad = 0;

    /* ---- the per-position path: the router's order, k ascending --------- */
    int idx[NEXP] = { 3, 0, 1, 2 };     /* deliberately not sorted by id */
    float val[NEXP] = { 1.0f, 1.0f, 1.0f, 1.0f };

    float fast[DIM] = {0};
    bad |= !lumi_moe_apply(0, idx, val, NEXP, x, DIM, fast);

    /* now make the first expert issued the last one to answer, so the wire
     * order is 0,1,2 then 3 while the router's order is still 3,0,1,2 */
    nodes[3].delay_ms = 120;
    float slow[DIM] = {0};
    bad |= !lumi_moe_apply(0, idx, val, NEXP, x, DIM, slow);
    nodes[3].delay_ms = 0;

    if (!same_bits(fast, slow, DIM)) {
        fprintf(stderr, "per-position: a late reply changed the sum "
                        "(%.9g vs %.9g)\n", (double)fast[0], (double)slow[0]);
        bad = 1;
    }

    float want = fold(idx, val, NEXP);
    int arrival[NEXP] = { 0, 1, 2, 3 }; /* also the ascending-id order */
    float wrong = fold(arrival, val, NEXP);
    if (want == wrong) {
        fprintf(stderr, "the test proves nothing: the chosen contributions "
                        "sum the same in both orders\n");
        bad = 1;
    }
    for (int d = 0; d < DIM; d++) if (fast[d] != want) {
        fprintf(stderr, "per-position: summed in the wrong order "
                        "(%.9g, router order gives %.9g, arrival order %.9g)\n",
                (double)fast[d], (double)want, (double)wrong);
        bad = 1;
        break;
    }

    /* ---- the batched path: the union's order, first appearance --------- */
    int idxs[NEXP] = { 3, 0, 1, 2 };    /* one position, four experts */
    float ws[NEXP] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int keff[1] = { NEXP };

    float bfast[DIM] = {0};
    bad |= !lumi_moe_apply_batch(0, idxs, ws, keff, NEXP, x, 1, DIM, bfast);

    nodes[3].delay_ms = 120;
    float bslow[DIM] = {0};
    bad |= !lumi_moe_apply_batch(0, idxs, ws, keff, NEXP, x, 1, DIM, bslow);
    nodes[3].delay_ms = 0;

    if (!same_bits(bfast, bslow, DIM)) {
        fprintf(stderr, "batched: a late reply changed the sum "
                        "(%.9g vs %.9g)\n", (double)bfast[0], (double)bslow[0]);
        bad = 1;
    }
    for (int d = 0; d < DIM; d++) if (bfast[d] != want) {
        fprintf(stderr, "batched: summed in the wrong order "
                        "(%.9g, union order gives %.9g, arrival and "
                        "ascending id give %.9g)\n",
                (double)bfast[d], (double)want, (double)wrong);
        bad = 1;
        break;
    }

    atomic_store(&g_stop, 1);
    for (int i = 0; i < NEXP; i++) {
        void *r = NULL;
        pthread_join(th[i], &r);
        bad |= (intptr_t)r != 0;
    }
    for (int i = 0; i < NEXP; i++)
        while (L.peers[i].nsocks > 0) close(L.peers[i].socks[--L.peers[i].nsocks]);
    free(L.own);

    printf("ACCUMULATION ORDER: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
