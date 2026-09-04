/* What a node says about its residency, and what the chatter does with it.
 *
 * The flag is binary and most donors are not: a box that keeps a third of a
 * model hot in RAM and streams the rest is neither "RAM" nor "disk". So the
 * node also reports how many experts it keeps hot, how many it serves, and
 * how often a call is answered without a disk read, and the chatter blends
 * the RAM and disk costs by that rate.
 *
 * The case that matters most here is the node that says NOTHING. An
 * executor built before those fields answers ERES with three words and
 * stops. Reading that silence as "hits every time" hands a disk node the
 * RAM price and quietly routes work to the slowest replica in the swarm —
 * which is exactly what the first version of this feature did, because
 * nothing exercised it. Hence this file. */
#define LMBE_ENGINE_ID "residency-report-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include "lumabri_client.h"

typedef struct {
    int port;
    uint32_t flags;
    int with_counts;              /* 0 = an executor that predates the fields */
    uint32_t hot, held, permille;
} Node;

static _Atomic int g_listening;
static _Atomic int g_stop;

static void *node_main(void *arg) {
    Node *n = (Node *)arg;
    int lfd = lmb_listen(n->port);
    if (lfd < 0) return (void *)1;
    atomic_fetch_add(&g_listening, 1);
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
            if (lmb_recv(fd, &m)) { lmb_msg_free(&m); break; }
            int op = m.op;
            lmb_msg_free(&m);
            if (op != LMB_ERES) break;
            LmbBuf b = {0};
            lmb_buf_u32(&b, n->flags);
            lmb_buf_u32(&b, LMB_EXPERT_STATE_ACTIVE);
            lmb_buf_u32(&b, LMB_CAP_EXEC2);
            if (n->with_counts) {
                lmb_buf_u32(&b, n->hot);
                lmb_buf_u32(&b, n->held);
                lmb_buf_u32(&b, n->permille);
            }
            int bad = lmb_send(fd, LMB_ERES_R, b.p, (uint32_t)b.len, NULL, 0);
            free(b.p);
            if (bad) break;
        }
        close(fd);
    }
    close(lfd);
    return NULL;
}

/* Ask one node, the way lumi_add_peer does, and score the answer. */
static uint64_t offset_of(Node *n, LumiPeer *out) {
    LumiPeer *p = out;
    memset(p, 0, sizeof *p);
    snprintf(p->addr, sizeof p->addr, "127.0.0.1:%d", n->port);
    p->resident = -1;
    p->hot_permille = 0;
    lumi_read_residency(p);
    return lumi_blend_offset(p);
}

static int check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) return 0;
    fprintf(stderr, "%s: offset %llu us, expected %llu\n", what,
            (unsigned long long)got, (unsigned long long)want);
    return 1;
}

int main(void) {
    /* an executor from before the counts: three words and nothing more */
    Node old_disk = { 7650, LMB_EXPERT_DISK_FALLBACK, 0, 0, 0, 0 };
    Node old_ram  = { 7651, LMB_EXPERT_RESIDENT_RAM,  0, 0, 0, 0 };
    /* and executors that report them */
    Node cache_cold = { 7652, LMB_EXPERT_DISK_FALLBACK, 1, 1800, 11008, 163 };
    Node cache_warm = { 7653, LMB_EXPERT_DISK_FALLBACK, 1, 3000, 11008, 450 };
    Node cache_hot  = { 7654, LMB_EXPERT_DISK_FALLBACK, 1, 3000, 11008, 1000 };
    Node resident   = { 7655, LMB_EXPERT_RESIDENT_RAM,  1, 9000,  9000, 1000 };
    Node vram       = { 7656, LMB_EXPERT_RESIDENT_VRAM, 1, 9000,  9000, 1000 };
    Node *all[] = { &old_disk, &old_ram, &cache_cold, &cache_warm,
                    &cache_hot, &resident, &vram };
    int n = (int)(sizeof all / sizeof *all);
    pthread_t th[7];
    for (int i = 0; i < n; i++)
        pthread_create(&th[i], NULL, node_main, all[i]);
    for (int spins = 0; atomic_load(&g_listening) < n && spins < 500; spins++)
        usleep(10000);

    LumiPeer p;
    int bad = 0;

    /* THE regression: silence must keep the flag's own price, not the RAM
     * price. A node that cannot tell us how often it hits is a node we have
     * no reason to believe hits at all. */
    bad |= check("old node, disk", offset_of(&old_disk, &p), LUMI_TIER_DISK_US);
    if (p.held_experts) {
        fprintf(stderr, "old node reported counts it never sent\n");
        bad = 1;
    }
    bad |= check("old node, RAM", offset_of(&old_ram, &p), LUMI_TIER_RAM_US);

    /* a cache in front of a disk: the two prices, blended by the rate */
    uint64_t want_cold = (LUMI_TIER_RAM_US * 163 +
                          LUMI_TIER_DISK_US * (1000 - 163)) / 1000;
    bad |= check("cache at 16%", offset_of(&cache_cold, &p), want_cold);
    if (p.hot_experts != 1800 || p.held_experts != 11008) {
        fprintf(stderr, "counts not carried: %u of %u\n",
                p.hot_experts, p.held_experts);
        bad = 1;
    }
    uint64_t want_warm = (LUMI_TIER_RAM_US * 450 +
                          LUMI_TIER_DISK_US * (1000 - 450)) / 1000;
    bad |= check("cache at 45%", offset_of(&cache_warm, &p), want_warm);
    if (!(want_warm < want_cold)) {
        fprintf(stderr, "a warmer cache did not score better\n");
        bad = 1;
    }

    /* a cache that never misses IS RAM, and everything resident is unchanged */
    bad |= check("cache at 100%", offset_of(&cache_hot, &p), LUMI_TIER_RAM_US);
    bad |= check("resident RAM", offset_of(&resident, &p), LUMI_TIER_RAM_US);
    bad |= check("resident VRAM", offset_of(&vram, &p), LUMI_TIER_VRAM_US);

    /* a peer reached only through the tracker tunnel is never asked */
    memset(&p, 0, sizeof p);
    snprintf(p.addr, sizeof p.addr, "127.0.0.1:%d", old_disk.port);
    snprintf(p.relay_target, sizeof p.relay_target, "somewhere");
    p.resident = -1; p.hot_permille = 0;
    lumi_read_residency(&p);
    if (p.resident != -1 || p.hot_permille != 1000) {
        fprintf(stderr, "a tunnel peer was probed: resident %d, hot %u\n",
                p.resident, p.hot_permille);
        bad = 1;
    }
    bad |= check("tunnel peer", lumi_blend_offset(&p), LUMI_TIER_RAM_US);

    atomic_store(&g_stop, 1);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    printf("RESIDENCY REPORT: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
