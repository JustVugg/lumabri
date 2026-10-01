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
#include "src/runtime/lumabri_q8_reference.h"

enum { DIM = 4, EXPERTS = 4 };
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int received, local_started, local_calls, fail_local, bad_reply;
    struct timespec deadline;
} Gate;
typedef struct { Gate *gate; int fd, expert; } Remote;
typedef struct { int fd; float value; } NumericReply;

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
static void *numeric_remote(void *opaque) {
    NumericReply *reply = opaque; LmbMsg msg = {0};
    assert(!lmb_recv(reply->fd, &msg) && msg.op == LMB_HOME_EXPERT);
    float out[DIM]; for (int i = 0; i < DIM; i++) out[i] = reply->value;
    assert(!lmb_send(reply->fd, LMB_EXEC_R, NULL, 0, out, sizeof out));
    lmb_msg_free(&msg); close(reply->fd); return NULL;
}

static void numeric_case(float value, int rejected, int late) {
    memset(&L, 0, sizeof L);
    L.home.count = 1; L.n_layers = 1; L.n_experts = EXPERTS; L.hidden = DIM;
    L.hybrid_policy = LMB_HYBRID_FORCE_SPLIT;
    L.hybrid_timing[0].top_k = EXPERTS;
    L.npeers = 1; L.exec_wait_ms = 5000; L.hedge_ms = -1;
    if (late) { L.hybrid_probes[0] = 4; L.hybrid_timing[0].rounds = late == 2 ? 65 : 5; }
    L.own = malloc(EXPERTS * LUMI_MAX_REP * sizeof *L.own); assert(L.own);
    for (int i = 0; i < EXPERTS * LUMI_MAX_REP; i++) L.own[i] = -1;
    L.own[3 * LUMI_MAX_REP] = 0;
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    L.peers[0].socks[0] = pair[0]; L.peers[0].nsocks = 1;
    NumericReply reply = {pair[1], value}; pthread_t thread;
    assert(!pthread_create(&thread, NULL, numeric_remote, &reply));
    int idx[4] = {3, 0, 1, 2}, calls = 0;
    float x[DIM] = {0}, w[4] = {1, 1, 1, 1}, out[DIM] = {0};
    assert(lumi_moe_apply_split(0, idx, w, 4, x, DIM, out, 3, resident_local, &calls));
    assert(!pthread_join(thread, NULL));
    assert(L.calls == 1 && !!L.hybrid_numeric_failed == rejected);
    float expected = rejected ? contribution(3) : value;
    for (int k = 1; k < 4; k++) expected += contribution(idx[k]);
    for (int d = 0; d < DIM; d++) assert(isfinite(out[d]) && out[d] == expected);
    if (!rejected && value != contribution(3)) assert(L.hybrid_rounding_accepts == 1);
    while (L.peers[0].nsocks) close(L.peers[0].socks[--L.peers[0].nsocks]);
    free(L.own); memset(&L, 0, sizeof L);
}

typedef struct { float local, reference; int calls, fail; } ReferenceCase;
static int reference_local(void *opaque, int layer, int expert, const float *x, int D, float *out) {
    ReferenceCase *c = opaque; assert(layer == 0 && x && D == DIM);
    for (int d = 0; d < D; d++) out[d] = expert == 3 ? c->local : contribution(expert);
    return 0;
}
static int reference_precise(void *opaque, int layer, int expert, const float *x, int D, float *out) {
    ReferenceCase *c = opaque; assert(layer == 0 && expert == 3 && x && D == DIM);
    c->calls++;
    if (c->fail) return -1;
    for (int d = 0; d < D; d++) out[d] = c->reference;
    return 0;
}
static void reference_case(float pc, float mac, float precise, int reject, int calls, int fail) {
    memset(&L, 0, sizeof L);
    L.home.count = 1; L.n_layers = 1; L.n_experts = EXPERTS; L.hidden = DIM;
    L.hybrid_policy = LMB_HYBRID_FORCE_SPLIT;
    L.hybrid_timing[0].top_k = EXPERTS;
    L.npeers = 1; L.exec_wait_ms = 1000; L.hedge_ms = -1;
    L.own = malloc(EXPERTS * LUMI_MAX_REP * sizeof *L.own); assert(L.own);
    for (int i = 0; i < EXPERTS * LUMI_MAX_REP; i++) L.own[i] = -1;
    L.own[3 * LUMI_MAX_REP] = 0;
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    L.peers[0].socks[0] = pair[0]; L.peers[0].nsocks = 1;
    NumericReply reply = {pair[1], mac}; pthread_t thread;
    assert(!pthread_create(&thread, NULL, numeric_remote, &reply));
    ReferenceCase c = {pc, precise, 0, fail};
    int idx[] = {3, 0, 1, 2};
    float x[DIM] = {0}, w[] = {1,1,1,1}, out[DIM] = {0};
    assert(lumi_moe_apply_split_checked(0, idx, w, 4, x, DIM, out, 3,
                                       reference_local, reference_precise, &c));
    assert(!pthread_join(thread, NULL));
    assert(c.calls == calls && !!L.hybrid_numeric_failed == reject && L.calls == 1);
    assert(L.hybrid_reference_checks == (unsigned)calls);
    assert(L.hybrid_reference_accepts == (unsigned)(calls && !reject));
    float expected = reject ? pc : mac;
    for (int k = 1; k < 4; k++) expected += contribution(idx[k]);
    for (int d = 0; d < DIM; d++) assert(out[d] == expected);
    assert(L.hybrid_timing[0].completed == 1);
    assert(L.hybrid_timing[0].wall_s >= L.hybrid_timing[0].validation_s);
    while (L.peers[0].nsocks) close(L.peers[0].socks[--L.peers[0].nsocks]);
    free(L.own); memset(&L, 0, sizeof L);
}

static void reference_math_cases(void) {
    int8_t g[] = {1,-1}, u[] = {2,1}, d[] = {3,-2};
    float x[] = {2,1}, gs[] = {1}, us[] = {.5f}, ds[] = {1,2}, out[2];
    assert(!lmb_q8_expert_reference(x, 2, 1, g, u, d, gs, us, ds, out));
    long double a = 2.5L / (1 + expl(-1));
    assert(out[0] == (float)(3*a) && out[1] == (float)(-4*a));
    x[0] = 0; /* negative gate, stable SiLU branch */
    assert(!lmb_q8_expert_reference(x, 2, 1, g, u, d, gs, us, ds, out));
    a = -.5L / (1 + expl(1));
    assert(out[0] == (float)(3*a) && out[1] == (float)(-4*a));
    x[0] = -10000; /* no overflow or NaN in exp(-gate) */
    assert(!lmb_q8_expert_reference(x, 2, 1, g, u, d, gs, us, ds, out));
    assert(out[0] == 0 && out[1] == 0);
    x[0] = NAN;
    assert(lmb_q8_expert_reference(x, 2, 1, g, u, d, gs, us, ds, out));
    x[0] = 2; gs[0] = INFINITY;
    assert(lmb_q8_expert_reference(x, 2, 1, g, u, d, gs, us, ds, out));
    gs[0] = FLT_MAX;
    assert(lmb_q8_expert_reference(x, 2, 1, g, u, d, gs, us, ds, out));
    assert(lmb_q8_expert_reference(x, 0, 1, g, u, d, gs, us, ds, out));
    assert(lmb_q8_expert_reference(x, 2, 0, g, u, d, gs, us, ds, out));
    assert(lmb_q8_expert_reference(x, 2, 65537, g, u, d, gs, us, ds, out));
    assert(lmb_q8_expert_reference(NULL, 2, 1, g, u, d, gs, us, ds, out));
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

typedef struct { int fd, expert, bad; } CountReply;
static void *count_remote(void *opaque) {
    CountReply *r = opaque; LmbMsg msg = {0};
    assert(!lmb_recv(r->fd, &msg) && msg.op == LMB_HOME_EXPERT);
    assert(msg.body_len == 80 && !lmb_get32(msg.body + 64));
    assert(lmb_get32(msg.body + 68) == (unsigned)r->expert);
    assert(lmb_get32(msg.body + 72) == DIM && lmb_get32(msg.body + 76) == 1);
    float out[DIM]; for (int d = 0; d < DIM; d++) out[d] = contribution(r->expert);
    /* On failure the client may already be closing other outstanding calls. */
    (void)lmb_send(r->fd, LMB_EXEC_R, NULL, 0, out, r->bad ? sizeof(float) : sizeof out);
    lmb_msg_free(&msg); close(r->fd); return NULL;
}

static void home_count_case(int top_k, int remote, int bad) {
    memset(&L, 0, sizeof L);
    L.home.count = 1; L.n_layers = 1; L.n_experts = top_k; L.hidden = DIM;
    L.hybrid_policy = LMB_HYBRID_FORCE_SPLIT; L.hybrid_remote_experts = remote;
    L.hybrid_probes[0] = 4; L.hybrid_timing[0].top_k = top_k;
    L.hybrid_timing[0].rounds = 5; /* no validation recomputation in this count proof */
    L.npeers = 1; L.exec_wait_ms = 1000; L.hedge_ms = -1;
    L.own = malloc(top_k * LUMI_MAX_REP * sizeof *L.own); assert(L.own);
    for (int i = 0; i < top_k * LUMI_MAX_REP; i++) L.own[i] = -1;
    int idx[LMB_HYBRID_MAX_REMOTE + 1], calls = 0;
    float x[DIM] = {0}, w[LMB_HYBRID_MAX_REMOTE + 1], out[DIM] = {0};
    for (int i = 0; i < top_k; i++) { idx[i] = (i + top_k - 1) % top_k; w[i] = 1; }
    CountReply replies[LMB_HYBRID_MAX_REMOTE]; pthread_t threads[LMB_HYBRID_MAX_REMOTE];
    for (int i = 0; i < remote; i++) {
        int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
        L.own[idx[i] * LUMI_MAX_REP] = 0;
        L.peers[0].socks[remote - i - 1] = pair[0];
        replies[i] = (CountReply){pair[1], idx[i], bad && !i};
        assert(!pthread_create(&threads[i], NULL, count_remote, &replies[i]));
    }
    L.peers[0].nsocks = remote;
    int ok = lumi_moe_apply_split(0, idx, w, top_k, x, DIM, out, top_k - 1, resident_local, &calls);
    for (int i = 0; i < remote; i++) assert(!pthread_join(threads[i], NULL));
    assert(calls == top_k - remote);
    if (bad) {
        assert(!ok && !L.calls && L.hybrid_timing[0].retry_after[remote] > 0);
        assert(!L.hybrid_timing[0].completed && L.hybrid_timing[0].failures == 1);
        assert(L.hybrid_timing[0].failed_s > 0);
        for (int d = 0; d < DIM; d++) assert(out[d] == 0); /* no partial publication */
    } else {
        float expected = 0;
        for (int k = 0; k < top_k; k++) expected += contribution(idx[k]);
        assert(ok && L.calls == (unsigned)remote);
        assert(L.hybrid_timing[0].samples[remote] == 1);
        const LmbHybridTiming *t = &L.hybrid_timing[0];
        assert(t->completed == 1 && t->remote_rounds == 1 && !t->failures);
        assert(fabs(t->wall_s - (t->send_s + t->local_s + t->wait_s +
            t->validation_s + t->merge_s)) < 1e-9);
        for (int d = 0; d < DIM; d++) assert(!memcmp(&out[d], &expected, sizeof expected));
    }
    while (L.peers[0].nsocks) close(L.peers[0].socks[--L.peers[0].nsocks]);
    free(L.own); memset(&L, 0, sizeof L);
}

static void policy_cases(void) {
    for (int top_k = 1; top_k <= 64; top_k++) {
        int limit = lmb_hybrid_remote_limit(top_k);
        for (int winner = 0; winner <= limit; winner++) {
            LmbHybridTiming t = {0};
            /* Every count remains discoverable, but exploration is spread
             * across the session instead of front-loaded into 29 rounds. */
            for (int n = 0; n < 4 * (limit + 1) * LMB_HYBRID_PROBE_MAX_INTERVAL; n++) {
                int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, top_k, 1);
                assert(r >= 0 && r <= limit && r < top_k);
                double ms = r == winner ? .002 : .020;
                lmb_hybrid_observe(&t, r, ms, 0, .001, 0);
            }
            assert(lmb_hybrid_best(&t) == winner);
            unsigned chosen = 0;
            for (int n = 0; n < 256; n++) {
                int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, top_k, 1);
                chosen += r == winner;
                lmb_hybrid_observe(&t, r, r == winner ? .002 : .020, 0, .001, 0);
            }
            assert(chosen >= 192); /* exploitation still dominates */
            assert(!lmb_hybrid_choose(&t, LMB_HYBRID_FORCE_LOCAL, top_k, 1));
            assert(lmb_hybrid_choose(&t, LMB_HYBRID_FORCE_SPLIT, top_k, 99) == limit);
            uint64_t rounds = t.rounds;
            lmb_hybrid_observe(&t, 0, NAN, 0, 0, 0);
            lmb_hybrid_observe(&t, limit + 1, .001, 0, 0, 0);
            lmb_hybrid_observe(&t, 0, .001, -1, 0, 0);
            assert(t.rounds == rounds);
            if (!limit) continue;
            lmb_hybrid_failed(&t, 1, .040);
            assert(!t.samples[1]);
            while (t.rounds < t.retry_after[1]) {
                int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, top_k, 1);
                assert(r != 1);
                lmb_hybrid_observe(&t, r, .010, 0, .001, 0);
            }
            int retried = 0;
            for (int n = 0; n < 4 * (limit + 1) * LMB_HYBRID_PROBE_MAX_INTERVAL; n++) {
                int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, top_k, 1);
                retried |= r == 1;
                lmb_hybrid_observe(&t, r, .010, 0, .001, 0);
            }
            assert(retried); /* no permanent blacklist after a timeout */
        }
    }
    LmbHybridTiming t = {.top_k=8, .rounds=100};
    t.samples[0] = t.samples[3] = 2; t.seconds[0] = .010; t.seconds[3] = .0098;
    assert(!lmb_hybrid_best(&t)); /* insufficient gain cannot displace local */
    assert(!lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, 4, 1));
    assert(t.top_k == 4 && !t.rounds && !t.samples[3]); /* different top-k is not the same plan */

    memset(&t, 0, sizeof t);
    unsigned remote_startup = 0;
    double startup_s = 0;
    for (int n = 0; n < 29; n++) {
        int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, 8, 1);
        remote_startup += r > 0;
        double elapsed = r ? .020 : .002;
        startup_s += elapsed;
        lmb_hybrid_observe(&t, r, elapsed, 0, .001, 0);
    }
    assert(remote_startup == 2); /* only the bootstrap, no early costly sweep */
    assert(startup_s < .120); /* deterministic fixture, not a speed claim */

    /* A winner must keep being compared with a fresh local reference. */
    for (int n = 0; n < 20000; n++) {
        int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, 8, 1);
        lmb_hybrid_observe(&t, r, r == 1 ? .001 : .010, 0, .001, 0);
        if (lmb_hybrid_best(&t)) assert(t.rounds - t.local_at <= LMB_HYBRID_LOCAL_REFRESH);
    }
    assert(lmb_hybrid_best(&t) == 1);
    /* Load/topology can change without changing the approved allocation. */
    for (int n = 0; n < 20000; n++) {
        int r = lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, 8, 1);
        lmb_hybrid_observe(&t, r, r == 7 ? .001 : .010, 0, .001, 0);
    }
    assert(lmb_hybrid_best(&t) == 7);
    t.seconds[0] = .050; t.last_local_s = .0005;
    assert(!lmb_hybrid_best(&t)); /* stale slow local EWMA cannot force remote */

    /* Deterministic measured cooldown, including clamp and recovery. */
    memset(&t, 0, sizeof t); t.top_k = 8;
    assert(lmb_hybrid_probe_gap(&t, .100) == LMB_HYBRID_PROBE_INTERVAL);
    lmb_hybrid_observe(&t, 0, .002, 0, .002, 0);
    assert(lmb_hybrid_probe_gap(&t, .001) == LMB_HYBRID_PROBE_INTERVAL);
    assert(lmb_hybrid_probe_gap(&t, .008) == 100);
    assert(lmb_hybrid_probe_gap(&t, .200) == LMB_HYBRID_PROBE_MAX_INTERVAL);
    assert(lmb_hybrid_probe_gap(&t, INFINITY) == LMB_HYBRID_PROBE_MAX_INTERVAL);
    lmb_hybrid_record(&t, 1, .001, .001, .001, 0, 0);
    lmb_hybrid_record(&t, 0, 0, .002, 0, 0, 0);
    lmb_hybrid_record(&t, 1, .001, .001, .001, .020, 0);
    assert(t.next_probe - t.rounds == 350); /* validation is not free */
    assert(fabs(t.seconds[1] - .003) < 1e-12); /* service EWMA stays separate */
    uint64_t due = t.next_probe;
    while (t.rounds < due) {
        assert(!lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, 8, 1));
        lmb_hybrid_observe(&t, 0, .002, 0, .002, 0);
    }
    assert(lmb_hybrid_choose(&t, LMB_HYBRID_ADAPTIVE, 8, 1) == 2);
}

static void cost_cases(void) {
    /* Physical OLMoE PC+Mac counterexample, layer 14/expert 8/index 1855.
     * Both independently rounded outputs fit the existing bound against a
     * higher-precision reference, but the pairwise test rejects. Document
     * the current policy limit; do NOT silently relax it to pass an oracle. */
    float reference = 0.084909170866f, pc = 0.0849096700549f, mac = 0.0849076360464f;
    assert(lmb_hybrid_numeric_check(&reference, &pc, 1, NULL) >= 0);
    assert(lmb_hybrid_numeric_check(&reference, &mac, 1, NULL) >= 0);
    assert(lmb_hybrid_numeric_check(&pc, &mac, 1, NULL) == LMB_HYBRID_NUMERIC_REJECT);
    assert(lmb_hybrid_reference_candidate(&pc, &mac, 1));
    assert(lmb_hybrid_reference_check(&reference, &pc, &mac, 1));
    assert(!lmb_hybrid_reference_check(NULL, &pc, &mac, 1));
    reference_case(pc, mac, reference, 0, 1, 0); /* real cross-platform case */
    reference_case(pc, mac, reference, 1, 1, 1); /* unavailable reference */
    reference_case(pc, mac, pc, 1, 1, 0); /* remote fails independent bound */
    reference_case(pc, mac, mac, 1, 1, 0); /* local fails independent bound */
    reference_case(pc, mac, NAN, 1, 1, 0); /* invalid reference */
    reference_case(pc, mac, INFINITY, 1, 1, 0);
    reference_case(1, nextafterf(1, 2), 0, 0, 0, 0); /* fast path unchanged */
    reference_case(1, 1.01f, 1, 1, 0, 0); /* gross error cannot trigger ref */
    reference_case(1, NAN, 1, 1, 0, 0);
    reference_case(1, INFINITY, 1, 1, 0, 0);
    reference_case(-.9e-6f, .9e-6f, 0, 0, 1, 0); /* cancellation near zero */
    reference_math_cases();
    LmbHybridTiming t = {.top_k=8};
    assert(lmb_hybrid_validation_due(&t, 0));
    assert(!lmb_hybrid_validation_due(&t, 4));
    t.rounds = 65;
    assert(lmb_hybrid_validation_due(&t, 4)); /* no exact-modulo blind spot */
    t.validated_at = t.rounds;
    assert(!lmb_hybrid_validation_due(&t, 4));
    t.rounds = 128;
    assert(!lmb_hybrid_validation_due(&t, 4));
    t.rounds = 129;
    assert(lmb_hybrid_validation_due(&t, 4));
    lmb_hybrid_record(&t, 2, .001, .002, .003, .004, .005);
    assert(t.completed == 1 && t.remote_rounds == 1 && t.samples[2] == 1);
    assert(fabs(t.wall_s - .015) < 1e-12);
    assert(fabs(t.seconds[2] - .011) < 1e-12); /* validation charged only to wall */
    lmb_hybrid_record(&t, 0, 0, .002, 0, 0, .001);
    assert(t.completed == 2 && t.remote_rounds == 1);
    assert(fabs(t.wall_s - .018) < 1e-12);
    assert(fabs(t.wall_s - (t.send_s+t.local_s+t.wait_s+t.validation_s+t.merge_s)) < 1e-12);
    LmbHybridTiming before = t;
    lmb_hybrid_record(&t, 1, .001, .002, .003, NAN, .004);
    lmb_hybrid_record(&t, 1, .001, .002, .003, .004, -1);
    lmb_hybrid_record(&t, 1, 0, 0, 0, .003, 0); /* no service observation */
    assert(!memcmp(&before, &t, sizeof t));
    lmb_hybrid_failed(&t, 2, .007);
    lmb_hybrid_failed(&t, 0, .009);
    assert(t.failures == 2 && fabs(t.failed_s - .016) < 1e-12);
    assert(t.completed == 2 && fabs(t.wall_s - .018) < 1e-12);
}

static void *feature_reply(void *opaque) {
    int fd = *(int *)opaque; LmbMsg msg = {0};
    assert(!lmb_recv(fd, &msg) && msg.op == LMB_HOME_FEATURES && msg.body_len == 128);
    assert(!lmb_send(fd, LMB_OK, msg.body, msg.body_len, NULL, 0));
    lmb_msg_free(&msg); close(fd); return NULL;
}

static void configuration_cases(void) {
    LmbHybridRoutes routes = {0};
    routes.allocation[0] = 1; routes.root[0] = 2; routes.count = 1;
    routes.peers[0].key[0] = 3; routes.peers[0].end = 1;
    strcpy(routes.peers[0].addr, "127.0.0.1:47302");
    LmbBuf b = {0}; assert(!lmb_hybrid_routes_pack(&b, &routes, 1));
    char encoded[LMB_HOME_HYBRID_ENV];
    for (size_t i = 0; i < b.len; i++) snprintf(encoded + 2*i, 3, "%02x", b.p[i]);
    free(b.p);
    assert(!setenv("LUMABRI_HOME_HYBRID_ROUTES", encoded, 1));
    assert(!setenv("LUMABRI_HOME_HYBRID_POLICY", "split", 1));
    const char *bad[] = {"0", "-1", "9", "2x", "999999999999999999999"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        memset(&L, 0, sizeof L);
        assert(!setenv("LUMABRI_HOME_HYBRID_REMOTE_EXPERTS", bad[i], 1));
        assert(lumi_home_init(1, EXPERTS, DIM, "fixture/v1") == -1);
        assert(!L.on && !L.own && !L.home.count); /* no socket or allocation */
    }
    const char *good[] = {NULL, "", "1", "4", "8"};
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        memset(&L, 0, sizeof L);
        if (good[i]) assert(!setenv("LUMABRI_HOME_HYBRID_REMOTE_EXPERTS", good[i], 1));
        else assert(!unsetenv("LUMABRI_HOME_HYBRID_REMOTE_EXPERTS"));
        int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
        L.peers[0].socks[0] = pair[0]; L.peers[0].nsocks = 1;
        pthread_t thread; assert(!pthread_create(&thread, NULL, feature_reply, &pair[1]));
        assert(!lumi_home_init(1, EXPERTS, DIM, "fixture/v1"));
        assert(!pthread_join(thread, NULL));
        assert(L.on && L.hybrid_policy == LMB_HYBRID_FORCE_SPLIT);
        assert(L.hybrid_remote_experts == (good[i] ? atoi(good[i]) : 0));
        assert(!memcmp(&L.home, &routes, sizeof routes));
        while (L.peers[0].nsocks) close(L.peers[0].socks[--L.peers[0].nsocks]);
        free(L.own); free(L.layer_ok);
    }
    unsetenv("LUMABRI_HOME_HYBRID_ROUTES");
    unsetenv("LUMABRI_HOME_HYBRID_POLICY");
    unsetenv("LUMABRI_HOME_HYBRID_REMOTE_EXPERTS");
    memset(&L, 0, sizeof L);
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

/* Deterministic wire failures: no allocation, model or physical donor needed.
 * Capture the public diagnostic, including redaction of arbitrary peer text. */
static void rpc_failure_case(int kind, const char *reason) {
    memset(&L, 0, sizeof L);
    L.npeers = 1; L.hedge_ms = -1; L.exec_wait_ms = 20;
    L.home.count = kind == 1 ? 0 : 1; /* Only EOF is eligible to reconnect. */
    LumiPeer *peer = &L.peers[0];
    snprintf(peer->addr, sizeof peer->addr, "test-peer");
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    lmb_set_io_timeout(pair[0], 20);
    float out[DIM] = {0};
    if (kind == 1) assert(!shutdown(pair[1], SHUT_WR));
    if (kind == 2 || kind == 5) {
        const char *body = kind == 2 ? "Hybrid capacity busy" : "SECRET-must-not-be-logged";
        assert(!lmb_send(pair[1], LMB_ERR, body, (uint32_t)strlen(body), NULL, 0));
    }
    if (kind == 3) assert(!lmb_send(pair[1], LMB_EXEC_R, NULL, 0, out, sizeof(float)));
    if (kind == 4 || kind == 6) {
        uint8_t header[16] = {0};
        lmb_put32(header, kind == 6 ? 0 : LMB_MAGIC);
        lmb_put32(header + 4, LMB_EXEC_R);
        lmb_put32(header + 12, sizeof out);
        assert(!lmb_write_full(pair[1], header, sizeof header));
        /* kind 4 withholds payload; kind 6 has invalid magic. */
    }
    FILE *log = tmpfile(); assert(log);
    fflush(stderr); int saved = dup(STDERR_FILENO); assert(saved >= 0);
    assert(dup2(fileno(log), STDERR_FILENO) >= 0);
    uint32_t tried = 0; LumiPeer *winner = NULL;
    lumi_peer_sent(peer);
    errno = EINVAL; /* EOF must not inherit an unrelated error. */
    assert(!lumi_finish_exec(0, 3, out, DIM, 1, NULL, pair[0], peer,
                             lumi_now(), &tried, &winner));
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved);
    rewind(log); char text[2048] = {0};
    assert(fread(text, 1, sizeof text - 1, log) > 0); fclose(log);
    assert(strstr(text, reason));
    assert(strstr(text, "layer=0 expert=3"));
    assert(!strstr(text, "SECRET"));
    if (kind == 2) assert(strstr(text, "remote_error=Hybrid capacity busy"));
    assert(!peer->inflight && !peer->nsocks && !winner && !L.calls);
    close(pair[1]);
}

static void secure_receive_failure_case(int kind, int expected_errno) {
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    LmbSecure sender = {0}, receiver = {0};
    sender.active = receiver.active = 1;
    if (kind == 0) assert(!shutdown(pair[1], SHUT_WR));
    if (kind == 1) {
        sender.tx_key[0] = 1; /* Deliberate authentication failure. */
        assert(!lmb_secure_send(&sender, pair[1], LMB_EXEC_R, NULL, 0, NULL, 0));
    }
    if (kind == 2 || kind == 3) {
        uint8_t header[16] = {0};
        lmb_put32(header, kind == 2 ? 0 : LMB_MAGIC);
        lmb_put32(header + 4, LMB_EXEC_R);
        lmb_put32(header + 12, UINT32_MAX);
        assert(!lmb_write_full(pair[1], header, sizeof header));
    }
    LmbMsg msg = {0}; errno = EINVAL;
    assert(lmb_secure_recv(&receiver, pair[0], &msg) == -1);
    assert(errno == expected_errno);
    lmb_msg_free(&msg); close(pair[0]); close(pair[1]);
}

typedef struct { int listener, close_again, wrong_identity, requests; } Reconnect;
static void *reconnect_server(void *opaque) {
    Reconnect *r = opaque;
    int fd = accept(r->listener, NULL, NULL); assert(fd >= 0);
    lmb_set_io_timeout(fd, 2000);
    assert(!lmb_secure_server(fd));
    LmbMsg msg = {0}; int rc = lmb_recv(fd, &msg);
    if (r->wrong_identity) {
        assert(rc); /* Approval pin failed: not one activation was sent. */
    } else {
        assert(!rc && msg.op == LMB_HOME_EXPERT && msg.body_len == 80);
        assert(!memcmp(msg.body, L.home.allocation, 32));
        assert(lmb_get32(msg.body + 64) == 0 && lmb_get32(msg.body + 68) == 3);
        assert(lmb_get32(msg.body + 72) == DIM && lmb_get32(msg.body + 76) == 1);
        assert(msg.pay_len == DIM * sizeof(float));
        r->requests++;
        if (!r->close_again)
            assert(!lmb_send(fd, LMB_EXEC_R, NULL, 0, msg.pay, msg.pay_len));
    }
    lmb_msg_free(&msg); close(fd); return NULL;
}

static void reconnect_case(int close_again, int wrong_identity) {
    memset(&L, 0, sizeof L);
    L.home.count = 1; L.home.allocation[0] = 17;
    L.npeers = 1; L.hedge_ms = -1; L.exec_wait_ms = 2000;
    Reconnect r = {.listener=lmb_listen(0), .close_again=close_again,
                   .wrong_identity=wrong_identity};
    assert(r.listener >= 0);
    struct sockaddr_in addr; socklen_t size = sizeof addr;
    assert(!getsockname(r.listener, (struct sockaddr *)&addr, &size));
    snprintf(L.peers[0].addr, sizeof L.peers[0].addr, "127.0.0.1:%u", ntohs(addr.sin_port));
    uint8_t seed[32] = {42}; lmb_sign_keypair(g_sec_pk, g_sec_sk, seed);
    memcpy(L.home.peers[0].key, g_sec_pk, 32);
    if (wrong_identity) L.home.peers[0].key[0] ^= 1;
    char path[] = "/tmp/lumabri-rpc-pins-XXXXXX", hex[65];
    int pinfd = mkstemp(path); assert(pinfd >= 0);
    FILE *pins = fdopen(pinfd, "w"); assert(pins);
    lmb_hex(hex, g_sec_pk, 32); fprintf(pins, "%s %s\n", L.peers[0].addr, hex);
    assert(!fclose(pins));
    const char *previous = getenv("LUMABRI_PEER_PINS");
    char *saved = previous ? strdup(previous) : NULL;
    assert(!setenv("LUMABRI_PEER_PINS", path, 1));
    lmb_enc_send = lmb_sec_send_hook; lmb_enc_recv = lmb_sec_recv_hook;
    lmb_enc_wrap = lmb_sec_wrap_hook; lmb_enc_forget = lmb_sec_forget_hook;
    pthread_t thread; assert(!pthread_create(&thread, NULL, reconnect_server, &r));
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(!shutdown(pair[1], SHUT_WR)); /* FIN raced with the prior dispatch. */
    LumiPeer *winner = NULL; uint32_t tried = 0; float x[DIM] = {1, 2, 3, 4};
    lumi_peer_sent(&L.peers[0]);
    float *result = lumi_finish_exec(0, 3, x, DIM, 1, NULL, pair[0], &L.peers[0],
                                     lumi_now(), &tried, &winner);
    assert(!pthread_join(thread, NULL));
    if (!close_again && !wrong_identity) {
        assert(result && !memcmp(result, x, sizeof x) && winner == &L.peers[0]);
    } else assert(!result && !winner);
    assert(r.requests == !wrong_identity && !L.peers[0].inflight);
    assert(L.peers[0].connect_attempts == 1);
    assert(L.peers[0].connect_successes == (unsigned)!wrong_identity);
    assert(L.peers[0].connect_s > 0);
    /* Even a second EOF permits no third connection. */
    struct pollfd pending = {r.listener, POLLIN, 0}; assert(poll(&pending, 1, 0) == 0);
    free(result); close(pair[1]); close(r.listener);
    while (L.peers[0].nsocks) close(L.peers[0].socks[--L.peers[0].nsocks]);
    lmb_enc_send = NULL; lmb_enc_recv = NULL; lmb_enc_wrap = NULL; lmb_enc_forget = NULL;
    if (saved) { assert(!setenv("LUMABRI_PEER_PINS", saved, 1)); free(saved); }
    else assert(!unsetenv("LUMABRI_PEER_PINS"));
    assert(!unlink(path));
}

static void pool_counter_case(void) {
    LumiPeer peer = {0};
    int live[2], stale[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, live));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, stale));
    close(stale[1]);
    lumi_put_sock(&peer, live[0]);
    lumi_put_sock(&peer, stale[0]);
    int fd = lumi_take_sock(&peer);
    assert(fd == live[0] && !peer.nsocks);
    assert(peer.pool_reuses == 1 && peer.stale_pool_closes == 1);
    assert(!peer.connect_attempts && !peer.connect_successes && peer.connect_s == 0);
    close(fd); close(live[1]);
}

int main(void) {
    pool_counter_case();
    reconnect_case(0, 0);
    reconnect_case(1, 0);
    reconnect_case(0, 1);
    secure_receive_failure_case(0, ECONNRESET);
    secure_receive_failure_case(1, EBADMSG);
    secure_receive_failure_case(2, EPROTO);
    secure_receive_failure_case(3, EMSGSIZE);
    rpc_failure_case(0, "reason=reply-timeout");
    rpc_failure_case(1, "reason=connection-closed");
    rpc_failure_case(2, "reason=remote-error");
    rpc_failure_case(3, "reason=invalid-reply");
    rpc_failure_case(4, "reason=receive-timeout");
    rpc_failure_case(5, "remote_error=unspecified");
    rpc_failure_case(6, "reason=invalid-frame");
    memset(&L, 0, sizeof L);
    float numeric_ref[] = {0, 1, -1, 1e-12f};
    float numeric_got[] = {-0.f, 1, -1, 1e-12f}; double numeric_error = -1;
    assert(lmb_hybrid_numeric_check(numeric_ref, numeric_got, 4, &numeric_error) == LMB_HYBRID_NUMERIC_EXACT);
    assert(numeric_error == 0);
    numeric_got[1] = nextafterf(1.f, 2.f);
    assert(lmb_hybrid_numeric_check(numeric_ref, numeric_got, 4, &numeric_error) == LMB_HYBRID_NUMERIC_ROUNDING);
    assert(numeric_error > 0);
    numeric_got[0] = 1e-5f; /* near-zero errors cannot hide in a relative metric */
    assert(lmb_hybrid_numeric_check(numeric_ref, numeric_got, 4, NULL) == LMB_HYBRID_NUMERIC_REJECT);
    numeric_got[0] = 0; numeric_got[2] = -.99f;
    assert(lmb_hybrid_numeric_check(numeric_ref, numeric_got, 4, NULL) == LMB_HYBRID_NUMERIC_REJECT);
    numeric_got[2] = -1; numeric_got[0] = NAN;
    assert(lmb_hybrid_numeric_check(numeric_ref, numeric_got, 4, NULL) == LMB_HYBRID_NUMERIC_REJECT);
    numeric_ref[0] = numeric_got[0] = INFINITY;
    assert(lmb_hybrid_numeric_check(numeric_ref, numeric_got, 4, NULL) == LMB_HYBRID_NUMERIC_REJECT);
    assert(lmb_hybrid_numeric_check(NULL, numeric_got, 4, NULL) == LMB_HYBRID_NUMERIC_REJECT);
    policy_cases();
    cost_cases();
    configuration_cases();
    signal(SIGPIPE, SIG_IGN);
    setenv("LUMABRI_EXEC_FALLBACK_LOCAL", "1", 1);
    unsetenv("LUMABRI_TRACKER");
    numeric_case(nextafterf(1.f, 2.f), 0, 0);
    numeric_case(1.01f, 1, 0);
    numeric_case(1.01f, 1, 2); /* periodic check after startup */
    numeric_case(1.f, 0, 1);
    numeric_case(NAN, 1, 1);
    numeric_case(INFINITY, 1, 1);
    run_case(0, 0); run_case(1, 0); run_case(0, 1);
    for (int k = 2; k <= LMB_HYBRID_MAX_REMOTE + 1; k++)
        for (int r = 1; r < k; r++) home_count_case(k, r, 0);
    home_count_case(4, 2, 1); home_count_case(4, 3, 1);
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
