/* lumibri_client.h — the chatter side of phase 2.
 *
 * Included by an engine compiled with -DLUMIBRI_P2P. When LUMIBRI_EXPERTS is
 * set (a comma list of host:port), the MoE layer stops loading expert weights
 * and instead sends the activation row to the peer that holds each selected
 * expert, then sums the returned rows with the router's weights.
 *
 * The K experts of one layer are issued BEFORE any reply is read, on K
 * separate sockets, so the peers work concurrently and the chatter pays one
 * round trip per LAYER, not per expert. That is the whole latency argument of
 * the design: 16 layers × one RTT, not 64 × one RTT.
 *
 * Two invariants, both inherited from the engine's own rules:
 *   - the accumulation runs k = 0..K-1 in the router's order, exactly as the
 *     local path does, so the float rounding is identical, not merely close;
 *   - a peer that cannot serve an expert is a hard error, never a silent
 *     fallback to local compute — a fallback would quietly turn a broken
 *     network into a passing measurement.
 */
#ifndef LUMIBRI_CLIENT_H
#define LUMIBRI_CLIENT_H

#include "lumibri_proto.h"

#define LUMI_MAX_PEERS 32
#define LUMI_MAX_K     64

typedef struct {
    char addr[64];
    int socks[LUMI_MAX_K];
    int nsocks;
} LumiPeer;

static struct {
    int on;
    LumiPeer peers[LUMI_MAX_PEERS];
    int npeers;
    int *owner;                 /* [n_layers*n_experts] → peer index, -1 = nobody */
    int n_layers, n_experts, hidden;
    unsigned long long calls, layers_done;
    double wait_s;
} L = {0};

static double lumi_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void lumi_die(const char *msg) {
    fprintf(stderr, "[lumibri] %s\n", msg);
    exit(1);
}

/* One socket per in-flight request: a peer executing two experts of the same
 * layer must see them as two concurrent requests, not a queue of one. */
static int lumi_take_sock(LumiPeer *p) {
    if (p->nsocks) return p->socks[--p->nsocks];
    int fd = lmb_connect(p->addr);
    if (fd < 0) {
        fprintf(stderr, "[lumibri] peer %s unreachable\n", p->addr);
        exit(1);
    }
    return fd;
}

static void lumi_put_sock(LumiPeer *p, int fd) {
    if (p->nsocks < LUMI_MAX_K) p->socks[p->nsocks++] = fd;
    else close(fd);
}

static void lumi_init(int n_layers, int n_experts, int hidden) {
    const char *spec = getenv("LUMIBRI_EXPERTS");
    if (!spec || !*spec) return;

    L.n_layers = n_layers; L.n_experts = n_experts; L.hidden = hidden;
    L.owner = (int *)malloc((size_t)n_layers * n_experts * sizeof(int));
    for (int i = 0; i < n_layers * n_experts; i++) L.owner[i] = -1;

    char list[1024];
    snprintf(list, sizeof list, "%s", spec);
    for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if (L.npeers == LUMI_MAX_PEERS) lumi_die("too many peers");
        LumiPeer *p = &L.peers[L.npeers];
        snprintf(p->addr, sizeof p->addr, "%s", tok);
        p->nsocks = 0;

        LmbMsg m = {0};
        if (lmb_request(p->addr, LMB_EMANIFEST, NULL, 0, &m) || m.op != LMB_EMANIFEST_R) {
            fprintf(stderr, "[lumibri] no expert manifest from %s\n", p->addr);
            exit(1);
        }
        LmbCur c = { m.body, m.body_len, 0 };
        uint32_t n = 0, peer_hidden = 0;
        if (lmb_cur_u32(&c, &n)) lumi_die("bad expert manifest");
        int claimed = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t l, e;
            if (lmb_cur_u32(&c, &l) || lmb_cur_u32(&c, &e)) lumi_die("bad manifest entry");
            if ((int)l >= n_layers || (int)e >= n_experts) lumi_die("manifest out of range");
            int gid = (int)l * n_experts + (int)e;
            if (L.owner[gid] < 0) { L.owner[gid] = L.npeers; claimed++; }
        }
        if (lmb_cur_u32(&c, &peer_hidden) || (int)peer_hidden != hidden)
            lumi_die("peer serves a model with a different hidden size");
        lmb_msg_free(&m);
        fprintf(stderr, "[lumibri] peer %s: %u experts (%d new)\n", p->addr, n, claimed);
        L.npeers++;
    }

    int missing = 0;
    for (int i = 0; i < n_layers * n_experts; i++) if (L.owner[i] < 0) missing++;
    if (missing) {
        fprintf(stderr, "[lumibri] %d of %d experts have no peer — refusing to run "
                        "(a partial network would silently change the model)\n",
                missing, n_layers * n_experts);
        exit(1);
    }
    L.on = 1;
    fprintf(stderr, "[lumibri] phase 2 active: every expert runs on a peer, "
                    "%d peer(s), hidden=%d\n", L.npeers, hidden);
}

/* Run the K selected experts of one layer on their peers and accumulate into
 * `out` with the router weights. Returns nothing: any failure is fatal. */
static void lumi_moe_apply(int layer, const int *idx, const float *val, int K,
                           const float *x, int D, float *out) {
    if (K > LUMI_MAX_K) lumi_die("top-k larger than the client supports");
    int fds[LUMI_MAX_K];
    LumiPeer *ps[LUMI_MAX_K];
    float *res[LUMI_MAX_K];
    double t0 = lumi_now();

    /* issue all K first — this is what buys one RTT per layer */
    for (int k = 0; k < K; k++) {
        int pi = L.owner[layer * L.n_experts + idx[k]];
        ps[k] = &L.peers[pi];
        fds[k] = lumi_take_sock(ps[k]);
        LmbBuf b = {0};
        lmb_buf_u32(&b, (uint32_t)layer);
        lmb_buf_u32(&b, (uint32_t)idx[k]);
        lmb_buf_u32(&b, (uint32_t)D);
        int rc = lmb_send(fds[k], LMB_EXEC, b.p, (uint32_t)b.len,
                          x, (uint32_t)(D * sizeof(float)));
        free(b.p);
        if (rc) lumi_die("send failed");
    }
    /* then collect */
    for (int k = 0; k < K; k++) {
        LmbMsg m = {0};
        if (lmb_recv(fds[k], &m) || m.op != LMB_EXEC_R ||
            m.pay_len != (uint32_t)(D * sizeof(float))) {
            fprintf(stderr, "[lumibri] peer %s failed on layer %d expert %d\n",
                    ps[k]->addr, layer, idx[k]);
            exit(1);
        }
        res[k] = (float *)m.pay;        /* stolen; freed below */
        m.pay = NULL;
        lmb_msg_free(&m);
        lumi_put_sock(ps[k], fds[k]);
    }
    /* accumulate in the router's order, exactly as the local path does */
    for (int k = 0; k < K; k++) {
        float w = val[k];
        const float *h = res[k];
        for (int d = 0; d < D; d++) out[d] += w * h[d];
        free(res[k]);
    }
    L.wait_s += lumi_now() - t0;
    L.calls += (unsigned long long)K;
    L.layers_done++;
}

static void lumi_report(void) {
    if (!L.on) return;
    fprintf(stderr, "[lumibri] %llu remote expert calls in %llu layer rounds · "
                    "%.2fs waiting on peers (%.2f ms per layer round)\n",
            L.calls, L.layers_done, L.wait_s,
            L.layers_done ? 1000.0 * L.wait_s / (double)L.layers_done : 0.0);
}

#endif /* LUMIBRI_CLIENT_H */
