/* The survival contract: a chatter with a local mirror must never die
 * because the swarm did.
 *
 * 1. Every replica gone → lumi_moe_apply returns 0 (the engine's local path
 *    runs the layer), the layer is demoted for LUMABRI_DEMOTE_S seconds and
 *    the NEXT failing layer demotes immediately instead of waiting again.
 * 2. A spot-check disagreement quarantines both suspects and recomputes —
 *    it no longer kills the engine that was the victim of the lie.
 *
 * Both regressions used to be exit(1) inside the client library, so the
 * strongest assertion this test makes is simply still being alive to print
 * its own verdict. */
#define LMBE_ENGINE_ID "fallback-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <pthread.h>
#include <stdatomic.h>
#include "lumabri_client.h"

/* One EXEC request, echoed back — optionally with the payload corrupted, so
 * a 100% spot-check sees two "replicas" disagree on the same activation. */
typedef struct { int port, corrupt; } Exec;

static _Atomic int g_listening;

static void *exec_server(void *arg) {
    Exec *e = (Exec *)arg;
    int lfd = lmb_listen(e->port);
    if (lfd < 0) return (void *)1;
    atomic_fetch_add(&g_listening, 1);
    int fd = accept(lfd, NULL, NULL);
    LmbMsg m = {0};
    int bad = fd < 0 || lmb_recv(fd, &m) || m.op != LMB_EXEC;
    if (!bad) {
        if (e->corrupt && m.pay_len) ((uint8_t *)m.pay)[0] ^= 0x55;
        bad = lmb_send(fd, LMB_EXEC_R, NULL, 0, m.pay, m.pay_len) != 0;
    }
    lmb_msg_free(&m);
    if (fd >= 0) close(fd);
    close(lfd);
    return (void *)(intptr_t)(bad != 0);
}

/* A "dead" peer that fails FAST on every dialect of dead. On WSL2 (and any
 * firewall that drops instead of refusing) a closed port eats the full
 * connect timeout, which would turn this test into a half-minute stall: so
 * the dead peer LISTENS and slams every connection shut, exactly like a
 * crashed executor whose port is still in the kernel backlog. */
static void *dead_server(void *arg) {
    int lfd = lmb_listen((int)(intptr_t)arg);
    if (lfd < 0) return (void *)1;
    atomic_fetch_add(&g_listening, 1);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd >= 0) close(fd);
    }
    return NULL;
}

static void wire_replicas(int gid, int a, int b) {
    L.own[(size_t)gid * LUMI_MAX_REP + 0] = a;
    L.own[(size_t)gid * LUMI_MAX_REP + 1] = b;
}

static int fail(const char *what) {
    fprintf(stderr, "test_local_fallback: FAIL — %s\n", what);
    return 1;
}

int main(void) {
    setenv("LUMABRI_PEER_WAIT_S", "2", 1);
    setenv("LUMABRI_DEMOTE_S", "2", 1);
    unsetenv("LUMABRI_EXPERTS");
    pthread_t dt, dp1, dp2;
    pthread_create(&dt, NULL, dead_server, (void *)(intptr_t)7580);
    pthread_create(&dp1, NULL, dead_server, (void *)(intptr_t)7583);
    pthread_create(&dp2, NULL, dead_server, (void *)(intptr_t)7584);
    pthread_detach(dt); pthread_detach(dp1); pthread_detach(dp2);
    for (int spins = 0; atomic_load(&g_listening) < 3 && spins < 500; spins++)
        usleep(10000);
    if (atomic_load(&g_listening) < 3)
        return fail("the dead-peer listeners never came up");
    /* a dead tracker keeps init on the discovery path (which allocates the
     * maps and reads the knobs) without ever finding a peer to trust */
    setenv("LUMABRI_TRACKER", "127.0.0.1:7580", 1);
    setenv("LUMABRI_DISCOVERY_MS", "600000", 1);
    lumi_init(2, 1, 4);          /* 2 layers, 1 expert each, hidden = 4 */
    if (!L.own) return fail("init did not allocate the expert map");
    if (!L.spread)
        return fail("replica spreading must be the default (LUMABRI_SPREAD=0 opts out)");

    /* --- 1. all replicas dead: demote, do not die ------------------------ */
    L.npeers = 2;
    snprintf(L.peers[0].addr, sizeof L.peers[0].addr, "127.0.0.1:7583");
    snprintf(L.peers[1].addr, sizeof L.peers[1].addr, "127.0.0.1:7584");
    lmb_predict_init(&L.peers[0].latency, 1000);
    lmb_predict_init(&L.peers[1].latency, 1000);
    wire_replicas(0, 0, 1);
    wire_replicas(1, 0, 1);
    L.on = 1;
    if (L.layer_ok) { L.layer_ok[0] = 1; L.layer_ok[1] = 1; }

    float x[4] = { 1, 2, 3, 4 }, out[4] = { 0 };
    int idx[1] = { 0 };
    float val[1] = { 1.0f };

    double t0 = lumi_now();
    if (lumi_moe_apply(0, idx, val, 1, x, 4, out))
        return fail("apply reported success with every replica dead");
    if (lumi_now() - t0 > 15.0)
        return fail("first failure took longer than the patience window");
    if (L.demotions != 1) return fail("layer 0 was not demoted");
    if (lumi_layer_on(0)) return fail("demoted layer 0 still claims the swarm");

    /* the swarm is now known sick: the second layer must give up at once */
    t0 = lumi_now();
    if (lumi_moe_apply(1, idx, val, 1, x, 4, out))
        return fail("second apply reported success with every replica dead");
    if (lumi_now() - t0 > 1.5)
        return fail("swarm-sick fast path did not skip the patience wait");
    if (lumi_layer_on(1)) return fail("demoted layer 1 still claims the swarm");

    sleep(3);                    /* the demotion window expires */
    if (!lumi_layer_on(0))
        return fail("layer 0 did not re-enable after the demotion window");

    /* --- 2. spot-check mismatch: quarantine, do not die ------------------ */
    Exec honest = { 7581, 0 }, liar = { 7582, 1 };
    pthread_t a, b;
    pthread_create(&a, NULL, exec_server, &honest);
    pthread_create(&b, NULL, exec_server, &liar);
    for (int spins = 0; atomic_load(&g_listening) < 5 && spins < 500; spins++)
        usleep(10000);
    if (atomic_load(&g_listening) < 5)
        return fail("the exec servers never came up");
    snprintf(L.peers[0].addr, sizeof L.peers[0].addr, "127.0.0.1:%d", honest.port);
    snprintf(L.peers[1].addr, sizeof L.peers[1].addr, "127.0.0.1:%d", liar.port);
    L.peers[0].dead = L.peers[1].dead = 0;
    L.peers[0].retry_at = L.peers[1].retry_at = 0;
    lmb_predict_init(&L.peers[0].latency, 1000);
    lmb_predict_init(&L.peers[1].latency, 1000);
    L.swarm_sick_until = 0;
    if (L.demote_until) { L.demote_until[0] = 0; L.demote_until[1] = 0; }
    L.verify_pct = 100;

    memset(out, 0, sizeof out);
    int r = lumi_moe_apply(0, idx, val, 1, x, 4, out);
    pthread_join(a, NULL); pthread_join(b, NULL);
    if (L.integrity_fails != 1)
        return fail("the disagreement was not counted as an integrity failure");
    if (!L.peers[0].dead || !L.peers[1].dead)
        return fail("the two disagreeing replicas were not both quarantined");
    if (r)
        return fail("apply trusted an answer after an unresolved disagreement");

    printf("local fallback tests: ok (demote %llus, quarantine, still alive)\n",
           (unsigned long long)L.demote_s);
    return 0;
}
