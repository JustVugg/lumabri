/* A donor behind NAT must be a REPLICA, not a coverage bit of last resort.
 *
 * The peer's advertised address refuses every dial (a dead listener — the
 * honest shape of a home router). The fake tracker serves the peer's
 * manifest through LMB_TMAN and executes targeted LMB_TEXEC calls in its
 * place. The client must:
 *
 *   1. adopt the peer through the tunnel (relay_target set, experts in the
 *      table) instead of skipping it;
 *   2. run an ordinary lumi_moe_apply through it — the TEXEC frame names
 *      exactly that peer, and the reply is used like any direct answer.
 */
#define LMBE_ENGINE_ID "nat-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <pthread.h>
#include <stdatomic.h>
#include "lumabri_client.h"

static _Atomic int g_listening;
static _Atomic int g_texec_seen;
static char g_expect_target[64];

/* the NAT'd peer: every dial dies instantly */
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

/* the tracker: manifest over TMAN, compute over TEXEC */
static void *tracker_server(void *arg) {
    int lfd = lmb_listen((int)(intptr_t)arg);
    if (lfd < 0) return (void *)1;
    atomic_fetch_add(&g_listening, 1);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        LmbMsg m = {0};
        while (lmb_recv(fd, &m) == 0) {
            if (m.op == LMB_TMAN) {
                LmbBuf b = {0};
                lmb_buf_u32(&b, LMB_EXPERT_MANIFEST_MAGIC);
                lmb_buf_str(&b, L.engine_id);
                lmb_buf_str(&b, L.profile);   /* byte-compatible by construction */
                lmb_buf_str(&b, "tiny");
                lmb_buf_u32(&b, 0);           /* bits */
                lmb_buf_u32(&b, 0);           /* no identity */
                lmb_buf_u32(&b, 1);           /* one expert: (0,0) */
                lmb_buf_u32(&b, 0);
                lmb_buf_u32(&b, 0);
                lmb_buf_u32(&b, (uint32_t)L.hidden);
                lmb_send(fd, LMB_EMANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
                free(b.p);
            } else if (m.op == LMB_TEXEC) {
                LmbCur c = { m.body, m.body_len, 0 };
                char target[64] = "";
                lmb_cur_str(&c, target, sizeof target);
                if (!strcmp(target, g_expect_target))
                    atomic_fetch_add(&g_texec_seen, 1);
                /* echo the activations back, like an honest expert */
                lmb_send(fd, LMB_EXEC_R, NULL, 0, m.pay, m.pay_len);
            } else if (m.op == LMB_PING) {
                lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            } else break;
            lmb_msg_free(&m);
        }
        lmb_msg_free(&m);
        close(fd);
    }
    return NULL;
}

static int fail(const char *what) {
    fprintf(stderr, "test_nat_adopt: FAIL — %s\n", what);
    return 1;
}

int main(void) {
    unsetenv("LUMABRI_EXPERTS");
    unsetenv("LUMABRI_MODEL");
    setenv("LUMABRI_TRACKER", "127.0.0.1:7591", 1);
    setenv("LUMABRI_DISCOVERY_MS", "600000", 1);
    snprintf(g_expect_target, sizeof g_expect_target, "127.0.0.1:7592");

    lumi_init(1, 1, 4);
    if (!L.own) return fail("init did not allocate the expert map");

    pthread_t dt, dn;
    pthread_create(&dt, NULL, tracker_server, (void *)(intptr_t)7591);
    pthread_create(&dn, NULL, dead_server, (void *)(intptr_t)7592);
    pthread_detach(dt); pthread_detach(dn);
    for (int spins = 0; atomic_load(&g_listening) < 2 && spins < 500; spins++)
        usleep(10000);
    if (atomic_load(&g_listening) < 2)
        return fail("the fake tracker/peer never came up");

    /* 1. adoption through the tunnel */
    if (lumi_add_peer("127.0.0.1:7592"))
        return fail("the NAT peer was skipped although the tunnel had its manifest");
    LumiPeer *peer = NULL;
    for (int i = 0; i < L.npeers; i++)
        if (!strcmp(L.peers[i].addr, "127.0.0.1:7592")) peer = &L.peers[i];
    if (!peer) return fail("the adopted peer is not in the table");
    if (!peer->relay_target[0])
        return fail("the adopted peer has no relay target");
    if (L.own[0] < 0)
        return fail("the adopted peer's expert never entered the replica table");

    /* 2. an ordinary apply rides the tunnel */
    L.on = 1;
    if (L.layer_ok) L.layer_ok[0] = 1;
    float x[4] = { 1, 2, 3, 4 }, out[4] = { 0 };
    int idx[1] = { 0 };
    float val[1] = { 2.0f };
    if (!lumi_moe_apply(0, idx, val, 1, x, 4, out))
        return fail("apply failed although the tunnel executes for the peer");
    if (!atomic_load(&g_texec_seen))
        return fail("no TEXEC frame named the adopted peer");
    for (int d = 0; d < 4; d++)
        if (out[d] != 2.0f * x[d])
            return fail("the tunnelled answer was not accumulated like a direct one");

    printf("nat adopt tests: ok (tunnel manifest, targeted exec, %d TEXEC frame(s))\n",
           atomic_load(&g_texec_seen));
    return 0;
}
