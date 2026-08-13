/* Does the spot-check cover the FAILOVER answer, not only the fast-path one?
 *
 * Three replicas hold one expert, in this replica order:
 *
 *   1. refuses every call, so the client fails over
 *   2. answers — honestly or with one byte flipped, per argv[1]
 *   3. answers honestly, and is the checker the spot-check can reach
 *
 * With LUMABRI_VERIFY=100 every answer the client is willing to USE must be
 * double-checked, whichever path produced it. So:
 *
 *   liar    the failover answer disagrees with replica 3 → the client must
 *           print INTEGRITY FAILURE and exit 1, never return the bytes
 *   honest  the failover answer agrees → the run completes, and the check
 *           must have actually run (L.verified > 0), so a spot-check that
 *           silently did nothing cannot pass as a success
 *
 * The honest case is the one that keeps the fix safe: verifying the failover
 * path must not make an ordinary peer failure look like an attack.
 */
#define LMBE_ENGINE_ID "verify-failover-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <pthread.h>
#include "lumabri_client.h"

typedef struct {
    int port;
    int refuse;          /* drop every call: forces the caller to fail over */
    int lie;             /* flip one byte of the answer */
    int served;
} Node;

/* An honest expert here echoes the activation back unchanged: deterministic,
 * so two honest nodes always agree and any disagreement is the lie. */
static void *node_thread(void *arg) {
    Node *n = (Node *)arg;
    int lfd = lmb_listen(n->port);
    if (lfd < 0) return (void *)1;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) break;
        if (n->refuse) { close(fd); continue; }
        for (;;) {
            LmbMsg m = {0};
            if (lmb_recv(fd, &m) || m.op != LMB_EXEC) { lmb_msg_free(&m); break; }
            uint32_t len = m.pay_len;
            uint8_t *out = (uint8_t *)malloc(len ? len : 1);
            memcpy(out, m.pay, len);
            if (n->lie && len) out[0] ^= 0xFF;
            n->served++;
            int rc = lmb_send(fd, LMB_EXEC_R, NULL, 0, out, len);
            free(out);
            lmb_msg_free(&m);
            if (rc) break;
        }
        close(fd);
    }
    close(lfd);
    return NULL;
}

int main(int argc, char **argv) {
    int lie = argc > 1 && !strcmp(argv[1], "liar");
    int base = lie ? 7571 : 7581;
    Node refuse = { base, 1, 0, 0 };
    Node second = { base + 1, 0, lie, 0 };
    Node third  = { base + 2, 0, 0, 0 };
    pthread_t t[3];
    pthread_create(&t[0], NULL, node_thread, &refuse);
    pthread_create(&t[1], NULL, node_thread, &second);
    pthread_create(&t[2], NULL, node_thread, &third);
    usleep(200000);

    L.n_layers = 1; L.n_experts = 1; L.hidden = 4;
    L.npeers = 3;
    L.verify_pct = 100;
    L.own = (int *)malloc(LUMI_MAX_REP * sizeof(int));
    for (int i = 0; i < LUMI_MAX_REP; i++) L.own[i] = -1;
    L.own[0] = 0; L.own[1] = 1; L.own[2] = 2;
    snprintf(L.peers[0].addr, sizeof L.peers[0].addr, "127.0.0.1:%d", refuse.port);
    snprintf(L.peers[1].addr, sizeof L.peers[1].addr, "127.0.0.1:%d", second.port);
    snprintf(L.peers[2].addr, sizeof L.peers[2].addr, "127.0.0.1:%d", third.port);
    L.peers[0].rtt_us = 100; L.peers[1].rtt_us = 200; L.peers[2].rtt_us = 300;

    float x[4]; for (int i = 0; i < 4; i++) x[i] = (float)(i + 1) / 3.0f;
    float out[4] = {0};
    int idx[1] = { 0 };
    float val[1] = { 1.0f };

    /* In the liar case the client detects the disagreement inside this call
     * and exits(1); nothing below runs. */
    lumi_moe_apply(0, idx, val, 1, x, 4, out);

    if (lie) {
        printf("FAIL: the corrupted failover answer was accepted "
               "(checker served %d call(s))\n", third.served);
        return 1;
    }
    if (!L.verified) {
        printf("FAIL: the honest failover answer was never checked "
               "(verified=%llu)\n", L.verified);
        return 1;
    }
    if (memcmp(out, x, sizeof x) != 0) {
        printf("FAIL: honest run returned the wrong bytes\n");
        return 1;
    }
    printf("VERIFIED FAILOVER: PASS (honest failover checked %llu time(s) "
           "against replica 3, no false alarm)\n", L.verified);
    return 0;
}
