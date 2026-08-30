#define LMBE_ENGINE_ID "rtt-refresh-test"
#define LMBE_SOURCE_ID "test"
#define LMBE_EXPECT_BITS 0
#define LUMI_RTT_PROBE_TIMEOUT_MS 100
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>

#include "lumabri_client.h"

typedef struct {
    const char *name;
    int port, ctrl_fd;
    _Atomic int delay_ms, calls, pings, drop_pings, accepts, ready, stop;
    uint8_t sk[64], pk[32], nonce[32];
    pthread_t thread;
} TestNode;

static const char *g_tracker;

static void close_slot(struct pollfd *slot) {
    if (slot->fd >= 0) close(slot->fd);
    slot->fd = -1;
    slot->events = 0;
    slot->revents = 0;
}

static void *node_server(void *arg) {
    TestNode *node = (TestNode *)arg;
    struct pollfd fds[6] = {{0}};
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++) fds[i].fd = -1;
    fds[0].fd = lmb_listen(node->port);
    fds[0].events = POLLIN;
    if (fds[0].fd < 0) { atomic_store(&node->ready, -1); return (void *)1; }
    atomic_store(&node->ready, 1);

    while (!atomic_load(&node->stop)) {
        int pr = poll(fds, sizeof fds / sizeof fds[0], 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & POLLIN) {
            int fd = accept(fds[0].fd, NULL, NULL);
            if (fd >= 0) {
                atomic_fetch_add(&node->accepts, 1);
                for (size_t i = 1; i < sizeof fds / sizeof fds[0]; i++)
                    if (fds[i].fd < 0) { fds[i].fd = fd; fds[i].events = POLLIN; fd = -1; break; }
                if (fd >= 0) close(fd);
            }
        }
        for (size_t i = 1; i < sizeof fds / sizeof fds[0]; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            LmbMsg message = {0};
            if (lmb_recv(fds[i].fd, &message)) { close_slot(&fds[i]); continue; }
            int delay = atomic_load(&node->delay_ms);
            if (delay > 0) usleep((useconds_t)delay * 1000u);
            int rc = -1;
            if (message.op == LMB_PING) {
                atomic_fetch_add(&node->pings, 1);
                rc = atomic_load(&node->drop_pings) ? 0 :
                     lmb_send(fds[i].fd, LMB_OK, NULL, 0, NULL, 0);
            } else if (message.op == LMB_EXEC) {
                atomic_fetch_add(&node->calls, 1);
                rc = lmb_send(fds[i].fd, LMB_EXEC_R, NULL, 0,
                              message.pay, message.pay_len);
            }
            lmb_msg_free(&message);
            if (rc) close_slot(&fds[i]);
        }
    }
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++) close_slot(&fds[i]);
    return NULL;
}

static int node_send_ereg(TestNode *node, int with_stats) {
    LmbBuf body = {0};
    char addr[64], profile[LMB_BUILD_PROFILE_MAX];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", node->port);
    lmb_build_profile(profile, sizeof profile);
    uint8_t held = 1;
    lmb_buf_str(&body, node->name);
    lmb_buf_str(&body, addr);
    lmb_buf_str(&body, "rtt-refresh-model");
    lmb_buf_u32(&body, 1);
    lmb_buf_u32(&body, 1);
    lmb_buf_bytes(&body, &held, 1);
    lmb_buf_u32(&body, LMB_EXPERT_MANIFEST_MAGIC);
    lmb_buf_str(&body, LMBE_ENGINE_ID);
    lmb_buf_str(&body, profile);
    lmb_buf_u32(&body, 0);
    lmb_buf_u32(&body, 4);
    lmb_buf_u32(&body, 1);
    lmb_buf_u32(&body, 1);
    if (with_stats) {
        lmb_buf_u32(&body, LMB_EREG_STATS_MAGIC);
        lmb_buf_u32(&body, LMB_EREG_STATS_VERSION);
        lmb_buf_u32(&body, LMB_EREG_STATS_LENGTH);
        lmb_buf_u64(&body, (uint64_t)atomic_load(&node->calls));
        lmb_buf_u32(&body, 0);
        /* v2 appends residency after the v1 call counters. This executor is a
         * protocol stand-in with no expert weights, so it reports none — but
         * it must still send the fields, or the tracker rejects the whole
         * extension and the named counters read as zero. */
        lmb_buf_u32(&body, 0);
        lmb_buf_u32(&body, 0);
        lmb_buf_u32(&body, 0);
        lmb_buf_u64(&body, 0);
        lmb_buf_u64(&body, 0);
    }
    uint8_t signed_body[512], signature[64];
    size_t signed_len = lmb_peer_auth_msg(node->nonce, node->name,
                                          "rtt-refresh-model", addr,
                                          signed_body, sizeof signed_body);
    lmb_sign(signature, signed_body, signed_len, node->sk);
    lmb_buf_peer_auth(&body, node->pk, signature);
    int bad = lmb_send(node->ctrl_fd, LMB_EREG, body.p, (uint32_t)body.len,
                       NULL, 0);
    free(body.p);
    LmbMsg reply = {0};
    if (!bad) bad = lmb_recv(node->ctrl_fd, &reply);
    if (!bad)
        bad = reply.op != LMB_OK || reply.body_len != 12 ||
              lmb_get32(reply.body) != LMB_EREG_CAP_MAGIC ||
              lmb_get32(reply.body + 4) != LMB_EREG_CAP_VERSION ||
              !(lmb_get32(reply.body + 8) & LMB_EREG_CAP_STATS);
    lmb_msg_free(&reply);
    return bad ? -1 : 0;
}

static int node_register(TestNode *node) {
    uint8_t seed[32];
    lmb_random(seed, sizeof seed);
    lmb_sign_keypair(node->pk, node->sk, seed);
    memset(seed, 0, sizeof seed);
    node->ctrl_fd = lmb_connect(g_tracker);
    if (node->ctrl_fd < 0 || lmb_request_challenge(node->ctrl_fd, node->nonce))
        return -1;
    return node_send_ereg(node, 0);
}

static int read_named_calls(uint64_t *calls_a, uint64_t *calls_b) {
    LmbMsg message = {0};
    if (lmb_request(g_tracker, LMB_SWARM_DETAIL, NULL, 0, &message) ||
        message.op != LMB_SWARM_DETAIL_R || message.pay_len) return -1;
    LmbCur cursor = { message.body, message.body_len, 0 };
    uint32_t version = 0, count = 0;
    int bad = lmb_cur_u32(&cursor, &version) || lmb_cur_u32(&cursor, &count) ||
              version != LMB_SWARM_DETAIL_VERSION || count > 64;
    int found_a = 0, found_b = 0;
    for (uint32_t i = 0; !bad && i < count; i++) {
        char name[64], model[64];
        uint32_t roles, age, files, experts, have_stats, inflight;
        /* SWARM_DETAIL v2 inserted residency between the expert counters and
         * the Segment block; read it so the fields after it stay aligned. */
        uint32_t state, residency_flags, resident_count;
        uint32_t begin, end, active, maximum, queued, segment_inflight, flags;
        uint64_t held, served, reads, calls, resident_ram, resident_vram;
        bad = lmb_cur_str(&cursor, name, sizeof name) ||
              lmb_cur_str(&cursor, model, sizeof model) ||
              lmb_cur_u32(&cursor, &roles) || lmb_cur_u32(&cursor, &age) ||
              lmb_cur_u64(&cursor, &held) || lmb_cur_u64(&cursor, &served) ||
              lmb_cur_u64(&cursor, &reads) || lmb_cur_u32(&cursor, &files) ||
              lmb_cur_u32(&cursor, &experts) || lmb_cur_u32(&cursor, &have_stats) ||
              lmb_cur_u64(&cursor, &calls) || lmb_cur_u32(&cursor, &inflight) ||
              lmb_cur_u32(&cursor, &state) ||
              lmb_cur_u32(&cursor, &residency_flags) ||
              lmb_cur_u32(&cursor, &resident_count) ||
              lmb_cur_u64(&cursor, &resident_ram) ||
              lmb_cur_u64(&cursor, &resident_vram) ||
              lmb_cur_u32(&cursor, &begin) || lmb_cur_u32(&cursor, &end) ||
              lmb_cur_u32(&cursor, &active) || lmb_cur_u32(&cursor, &maximum) ||
              lmb_cur_u32(&cursor, &queued) || lmb_cur_u32(&cursor, &segment_inflight) ||
              lmb_cur_u32(&cursor, &flags);
        (void)age; (void)held; (void)served; (void)reads; (void)files;
        (void)experts; (void)inflight; (void)begin; (void)end; (void)active;
        (void)state; (void)residency_flags; (void)resident_count;
        (void)resident_ram; (void)resident_vram;
        (void)maximum; (void)queued; (void)segment_inflight; (void)flags;
        if (!bad && !strcmp(model, "rtt-refresh-model") &&
            (roles & LMB_SWARM_ROLE_EXPERT) && have_stats) {
            if (!strcmp(name, "rtt-node-a")) { *calls_a = calls; found_a = 1; }
            if (!strcmp(name, "rtt-node-b")) { *calls_b = calls; found_b = 1; }
        }
    }
    bad |= cursor.off != cursor.len || !found_a || !found_b;
    lmb_msg_free(&message);
    return bad ? -1 : 0;
}

static int run_exec_calls(int count) {
    float input[4] = {0.25f, -0.5f, 0.75f, 1.0f};
    for (int i = 0; i < count; i++) {
        uint32_t tried = 0;
        LumiPeer *from = NULL;
        float *output = lumi_exec_retry(0, 0, input, 4, 1, NULL, &tried, &from);
        int bad = !output || !from || memcmp(output, input, sizeof input);
        free(output);
        if (bad) return -1;
    }
    return 0;
}

static void test_cleanup(TestNode nodes[2]) {
    for (int i = 0; i < L.npeers; i++)
        while (L.peers[i].nsocks) close(L.peers[i].socks[--L.peers[i].nsocks]);
    free(L.own); L.own = NULL;
    for (int i = 0; i < 2; i++) {
        if (nodes[i].ctrl_fd >= 0) close(nodes[i].ctrl_fd);
        atomic_store(&nodes[i].stop, 1);
    }
    for (int i = 0; i < 2; i++) pthread_join(nodes[i].thread, NULL);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s TRACKER\n", argv[0]);
        return 2;
    }
    g_tracker = argv[1];
    TestNode nodes[2] = {
        { .name = "rtt-node-a", .port = 8094, .ctrl_fd = -1, .delay_ms = 2 },
        { .name = "rtt-node-b", .port = 8095, .ctrl_fd = -1, .delay_ms = 20 }
    };
    for (int i = 0; i < 2; i++) pthread_create(&nodes[i].thread, NULL, node_server, &nodes[i]);
    for (int tries = 0; tries < 100; tries++) {
        if (atomic_load(&nodes[0].ready) && atomic_load(&nodes[1].ready)) break;
        usleep(10000);
    }
    int bad = atomic_load(&nodes[0].ready) != 1 || atomic_load(&nodes[1].ready) != 1;
    for (int i = 0; !bad && i < 2; i++) bad = node_register(&nodes[i]) != 0;
    L.n_layers = 1; L.n_experts = 1; L.hidden = 4; L.npeers = 2;
    L.initialized = 1; L.discovery = 0; L.hedge_ms = 0; L.verify_pct = 0;
    L.own = (int *)malloc(LUMI_MAX_REP * sizeof(int));
    if (!L.own) bad = 1;
    if (!bad) {
        for (int i = 0; i < LUMI_MAX_REP; i++) L.own[i] = -1;
        L.own[0] = 0; L.own[1] = 1;
        for (int i = 0; i < 2; i++) {
            snprintf(L.peers[i].addr, sizeof L.peers[i].addr,
                     "127.0.0.1:%d", nodes[i].port);
            lumi_probe(&L.peers[i]);
        }
        bad = L.peers[0].rtt_us >= L.peers[1].rtt_us ||
              atomic_load(&nodes[0].accepts) != 1 ||
              atomic_load(&nodes[1].accepts) != 1;
    }

    long initial_a = L.peers[0].rtt_us, initial_b = L.peers[1].rtt_us;
    atomic_store(&nodes[0].delay_ms, 40);
    atomic_store(&nodes[1].delay_ms, 1);
    if (!bad) bad = run_exec_calls(8) != 0;
    if (!bad) bad = node_send_ereg(&nodes[0], 1) || node_send_ereg(&nodes[1], 1);
    uint64_t calls_a = 0, calls_b = 0;
    if (!bad) bad = read_named_calls(&calls_a, &calls_b) || calls_a != 8 || calls_b != 0;

    if (!bad) {
        double due = 1000.0;
        long before_b = L.peers[1].rtt_us;
        L.next_rtt_refresh = due;
        lumi_maybe_refresh_rtt(due);      /* oldest measurement: node A */
        bad = atomic_load(&nodes[0].pings) != 4 ||
              atomic_load(&nodes[1].pings) != 2 ||
              L.peers[0].rtt_us <= initial_a || L.peers[1].rtt_us != before_b;
        lumi_maybe_refresh_rtt(due + 1.0); /* cadence gate: no second peer yet */
        bad |= atomic_load(&nodes[0].pings) != 4 ||
               atomic_load(&nodes[1].pings) != 2;
        lumi_maybe_refresh_rtt(due + LUMI_RTT_REFRESH_S); /* then node B */
        bad |= L.peers[1].rtt_us >= L.peers[0].rtt_us ||
               L.peers[1].rtt_us >= initial_b ||
               atomic_load(&nodes[0].pings) != 4 ||
               atomic_load(&nodes[1].pings) != 4 ||
               atomic_load(&nodes[0].accepts) != 1 ||
               atomic_load(&nodes[1].accepts) != 1;
    }
    /* A successful maintenance probe must restore the normal EXEC timeout. */
    if (!bad) {
        atomic_store(&nodes[1].delay_ms, 150);
        bad = run_exec_calls(1) != 0;
        atomic_store(&nodes[1].delay_ms, 1);
    }
    if (!bad) bad = run_exec_calls(7) != 0;
    if (!bad) bad = node_send_ereg(&nodes[0], 1) || node_send_ereg(&nodes[1], 1);
    if (!bad) bad = read_named_calls(&calls_a, &calls_b) || calls_a != 8 || calls_b != 8;

    /* A maintenance PING timeout is bounded and does not change health or the
     * previous estimate; the broken pooled socket is simply discarded. */
    if (!bad) {
        long saved = L.peers[0].rtt_us;
        double started = lumi_now();
        atomic_store(&nodes[0].drop_pings, 1);
        bad = lumi_refresh_rtt(&L.peers[0]) != -1 ||
              lumi_now() - started > 1.0 || L.peers[0].dead ||
              L.peers[0].rtt_us != saved;
    }

    long final_a = L.peers[0].rtt_us, final_b = L.peers[1].rtt_us;
    test_cleanup(nodes);
    if (bad) {
        fprintf(stderr, "staggered RTT refresh regression failed "
                        "(named calls A/B=%llu/%llu, RTT A/B=%ld/%ld us)\n",
                (unsigned long long)calls_a, (unsigned long long)calls_b,
                final_a, final_b);
        return 1;
    }
    printf("STAGGERED RTT REFRESH: PASS "
           "(named calls stale 8/0, refreshed 8/8; two PINGs, pooled accepts 1/1)\n");
    return 0;
}
