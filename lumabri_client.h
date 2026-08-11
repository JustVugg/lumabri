/* lumabri_client.h — the chatter side of phase 2.
 *
 * Included by an engine compiled with -DLUMABRI_P2P. When LUMABRI_EXPERTS is
 * set (a comma list of host:port), the MoE layer stops loading expert weights
 * and instead sends the activation row to a peer that holds each selected
 * expert, then sums the returned rows with the router's weights.
 *
 * The K experts of one layer are issued BEFORE any reply is read, on K
 * separate sockets, so the peers work concurrently and the chatter pays one
 * round trip per LAYER, not per expert. That is the whole latency argument of
 * the design: 16 layers × one RTT, not 64 × one RTT.
 *
 * Replicas and distance: an expert may be held by several peers. Each peer's
 * round-trip time is measured at init (two PINGs, take the min), and every
 * call goes to the nearest live replica — the chatter owns its distance map,
 * nobody coordinates it. A peer that fails a call is marked dead and the
 * expert is retried on the next replica: slower for that one round, but the
 * generation survives churn instead of dying with it.
 *
 * Two invariants, both inherited from the engine's own rules:
 *   - the accumulation runs k = 0..K-1 in the router's order, exactly as the
 *     local path does, so the float rounding is identical, not merely close;
 *   - an expert with NO live replica is a hard error, never a silent
 *     fallback to local compute — a fallback would quietly turn a broken
 *     network into a passing measurement.
 */
#ifndef LUMABRI_CLIENT_H
#define LUMABRI_CLIENT_H

#include <limits.h>

#include "lumabri_proto.h"
#include "lumabri_sign.h"

#define LUMI_MAX_PEERS 64
#define LUMI_MAX_K     64
#define LUMI_MAX_REP   4      /* replicas remembered per expert */
#define LUMI_WAIT_S    30     /* how long a vanished peer is given to come back */

typedef struct {
    char addr[64];
    int socks[LUMI_MAX_K];
    int nsocks;
    long rtt_us;
    int dead;
} LumiPeer;

static struct {
    int on, initialized, discovery;
    LumiPeer peers[LUMI_MAX_PEERS];
    int npeers;
    int *own;                   /* [gid * LUMI_MAX_REP] → peer index, -1 = free */
    unsigned char *routed;      /* [n_layers] 1 = this slot routes to experts */
    int n_layers, n_experts, hidden;
    int expected, expected_bits;
    char expected_model[64], profile[LMB_BUILD_PROFILE_MAX];
    LmbModelIdentity identity; int have_identity;
    uint8_t pubkey[32]; int have_pubkey;
    double next_discover, discover_period_s;
    int verify_pct;             /* LUMABRI_VERIFY: % of calls double-checked */
    unsigned long long calls, layers_done, failovers, verified;
    double wait_s;
} L = {0};

#ifndef LMBE_EXPECT_BITS
#define LMBE_EXPECT_BITS 8
#endif

static double lumi_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void lumi_die(const char *msg) {
    fprintf(stderr, "[lumabri] %s\n", msg);
    exit(1);
}

/* One socket per in-flight request: a peer executing two experts of the same
 * layer must see them as two concurrent requests, not a queue of one.
 * Returns -1 (and marks the peer dead) when it cannot connect: the caller
 * has replicas to fall back to, so a dead peer must not be fatal here. */
static int lumi_take_sock(LumiPeer *p) {
    if (p->nsocks) return p->socks[--p->nsocks];
    int fd = lmb_connect(p->addr);
    if (fd >= 0 && lmb_auth(fd)) { close(fd); fd = -1; }
    if (fd < 0) {
        fprintf(stderr, "[lumabri] peer %s unreachable — marked dead\n", p->addr);
        p->dead = 1;
    }
    return fd;
}

static void lumi_put_sock(LumiPeer *p, int fd) {
    if (p->nsocks < LUMI_MAX_K) p->socks[p->nsocks++] = fd;
    else close(fd);
}

static void lumi_probe(LumiPeer *p) {
    p->rtt_us = LONG_MAX;
    int fd = lumi_take_sock(p);
    if (fd < 0) return;
    for (int i = 0; i < 2; i++) {       /* the second ping rides warm; take min */
        double a = lumi_now();
        LmbMsg m = {0};
        if (lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) || lmb_recv(fd, &m)) {
            close(fd); p->dead = 1; return;
        }
        lmb_msg_free(&m);
        long us = (long)((lumi_now() - a) * 1e6);
        if (us < p->rtt_us) p->rtt_us = us;
    }
    lumi_put_sock(p, fd);
}

static int lumi_index_ok(uint32_t layer, uint32_t eid) {
    return L.n_layers > 0 && L.n_experts > 0 &&
           layer < (uint32_t)L.n_layers && eid < (uint32_t)L.n_experts;
}

static void lumi_load_pubkey(void) {
    const char *spec = getenv("LUMABRI_PUBKEY");
    if (!spec || !*spec) return;
    char hex[80] = "";
    FILE *fp = fopen(spec, "r");
    if (fp) { if (fscanf(fp, "%78s", hex) != 1) hex[0] = 0; fclose(fp); }
    else snprintf(hex, sizeof hex, "%s", spec);
    if (strlen(hex) == 64 && !lmb_unhex(L.pubkey, hex, 32)) L.have_pubkey = 1;
    else fprintf(stderr, "[lumabri] invalid LUMABRI_PUBKEY: expert execution stays local\n");
}

static int lumi_identity_valid(const char *model, const LmbModelIdentity *id) {
    if (strcmp(model, id->model)) return 0;
    if (!L.have_pubkey) return 1;
    if (!id->has_sig) return 0;
    size_t n = 0;
    uint8_t *msg = lmb_model_id_msg(model, id->root, &n);
    int ok = msg && lmb_sign_verify(id->sig, msg, n, L.pubkey) == 0;
    free(msg);
    return ok;
}

static int lumi_refresh_identity(void) {
    const char *tracker = getenv("LUMABRI_TRACKER");
    if (!tracker || !*tracker || !L.expected_model[0]) return 0;
    LmbModelIdentity id;
    if (lmb_model_identity_get(tracker, L.expected_model, &id) ||
        !lumi_identity_valid(L.expected_model, &id)) return 0;
    if (L.have_identity && memcmp(L.identity.root, id.root, 32)) {
        fprintf(stderr, "[lumabri] model identity changed during this process — "
                        "refusing to mix checkpoints\n");
        exit(1);
    }
    L.identity = id; L.have_identity = 1;
    return 1;
}

/* Learn one peer: manifest, replica claims, distance. Returns 0, or -1 on
 * any failure (already-known addresses are a no-op success). */
static int lumi_add_peer(const char *addr) {
    int reprobe = -1;
    for (int i = 0; i < L.npeers; i++)
        if (!strcmp(L.peers[i].addr, addr)) {
            LumiPeer *known = &L.peers[i];
            if (!known->dead) return 0;
            while (known->nsocks) close(known->socks[--known->nsocks]);
            reprobe = i;
            break;
        }
    if (reprobe < 0 && L.npeers == LUMI_MAX_PEERS) return -1;
    int pi = reprobe >= 0 ? reprobe : L.npeers;
    LumiPeer *p = &L.peers[pi];
    if (reprobe < 0) snprintf(p->addr, sizeof p->addr, "%s", addr);
    p->nsocks = 0;

    LmbMsg m = {0};
    if (lmb_request(p->addr, LMB_EMANIFEST, NULL, 0, &m) || m.op != LMB_EMANIFEST_R) {
        fprintf(stderr, "[lumabri] no expert manifest from %s — skipped\n", p->addr);
        lmb_msg_free(&m);
        return -1;
    }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t magic = 0, bits = 0, have_id = 0, has_sig = 0;
    uint32_t n = 0, peer_hidden = 0;
    char engine[64] = "", profile[LMB_BUILD_PROFILE_MAX] = "", peer_model[64] = "";
    LmbModelIdentity peer_id = {0};
    int bad = lmb_cur_u32(&c, &magic) || magic != LMB_EXPERT_MANIFEST_MAGIC ||
              lmb_cur_str(&c, engine, sizeof engine) ||
              lmb_cur_str(&c, profile, sizeof profile) ||
              lmb_cur_str(&c, peer_model, sizeof peer_model) ||
              lmb_cur_u32(&c, &bits) || lmb_cur_u32(&c, &have_id) || have_id > 1;
    if (!bad && have_id) {
        snprintf(peer_id.model, sizeof peer_id.model, "%s", peer_model);
        bad = lmb_cur_bytes(&c, peer_id.root, sizeof peer_id.root) ||
              lmb_cur_u32(&c, &has_sig) || has_sig > 1 ||
              (has_sig && lmb_cur_bytes(&c, peer_id.sig, sizeof peer_id.sig));
        peer_id.has_sig = has_sig != 0;
    }
    if (!bad) bad = lmb_cur_u32(&c, &n) ||
                    n > (uint64_t)L.n_layers * (uint64_t)L.n_experts;
    if (!bad && (strcmp(engine, LMBE_ENGINE_ID) || strcmp(profile, L.profile) ||
                 bits != (uint32_t)L.expected_bits ||
                 (L.expected_model[0] && strcmp(peer_model, L.expected_model)) ||
                 ((L.have_identity || L.have_pubkey) && !have_id) ||
                 (have_id && !lumi_identity_valid(peer_model, &peer_id)) ||
                 (L.have_identity && memcmp(L.identity.root, peer_id.root, 32)))) {
        bad = 1;
    }
    size_t gids = (size_t)L.n_layers * L.n_experts;
    uint8_t *seen = reprobe >= 0 ? (uint8_t *)calloc(gids, 1) : NULL;
    if (reprobe >= 0 && !seen) bad = 1;
    int claimed = 0;
    for (uint32_t i = 0; !bad && i < n; i++) {
        uint32_t l, e;
        if (lmb_cur_u32(&c, &l) || lmb_cur_u32(&c, &e) ||
            !lumi_index_ok(l, e)) { bad = 1; break; }
        size_t gid = (size_t)l * (size_t)L.n_experts + e;
        if (seen) { seen[gid] = 1; continue; }
        int *own = &L.own[((size_t)l * (size_t)L.n_experts + e) * LUMI_MAX_REP];
        for (int r = 0; r < LUMI_MAX_REP; r++) {
            if (own[r] == pi) break;                       /* duplicate entry */
            if (own[r] < 0) { own[r] = pi; claimed += r == 0; break; }
        }
    }
    if (!bad && seen)
        for (size_t gid = 0; gid < gids && !bad; gid++)
            for (int r = 0; r < LUMI_MAX_REP; r++)
                if (L.own[gid * LUMI_MAX_REP + r] == pi && !seen[gid]) {
                    bad = 1;
                    break;
                }
    if (bad || lmb_cur_u32(&c, &peer_hidden) ||
        peer_hidden != (uint32_t)L.hidden || c.off != c.len || m.pay_len != 0) {
        /* roll the claims back: this peer must not own anything */
        if (reprobe < 0)
            for (size_t i = 0; i < gids * LUMI_MAX_REP; i++)
                if (L.own[i] == pi) L.own[i] = -1;
        free(seen);
        lmb_msg_free(&m);
        fprintf(stderr, "[lumabri] peer %s: incompatible manifest "
                        "(model/build/engine/bits/shape) — skipped\n", p->addr);
        return -1;
    }
    if (!L.expected_model[0]) snprintf(L.expected_model, sizeof L.expected_model,
                                       "%s", peer_model);
    if (!L.have_identity && have_id) { L.identity = peer_id; L.have_identity = 1; }
    free(seen);
    lmb_msg_free(&m);
    p->dead = 0;
    lumi_probe(p);
    if (p->rtt_us == LONG_MAX) {
        if (reprobe < 0)
            for (size_t i = 0; i < gids * LUMI_MAX_REP; i++)
                if (L.own[i] == pi) L.own[i] = -1;
        return -1;
    }
    fprintf(stderr, "[lumabri] peer %s: %u experts (%d first-holder) · rtt %.2f ms\n",
            p->addr, n, claimed, (double)p->rtt_us / 1000.0);
    if (reprobe < 0) L.npeers++;
    return 0;
}

/* Ask the tracker who can execute for this model. Returns peers added. */
static int lumi_discover(void) {
    const char *tracker = getenv("LUMABRI_TRACKER");
    if (!tracker || !tracker[0]) return 0;
    const char *model = getenv("LUMABRI_MODEL");
    LmbBuf b = {0};
    if (model && model[0]) lmb_buf_str(&b, model);
    LmbMsg m = {0};
    int rc = lmb_request(tracker, LMB_EPEERS, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_EPEERS_R) { lmb_msg_free(&m); return 0; }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    int added = 0;
    if (!lmb_cur_u32(&c, &n))
        for (uint32_t i = 0; i < n; i++) {
            char addr[64];
            if (lmb_cur_str(&c, addr, sizeof addr)) break;
            int before = L.npeers;
            if (lumi_add_peer(addr) == 0 && L.npeers > before) added++;
        }
    lmb_msg_free(&m);
    return added;
}

static int lumi_missing_experts(void) {
    int missing = 0, expected = 0;
    for (int l = 0; l < L.n_layers; l++) {
        if (L.routed && !L.routed[l]) continue;
        for (int e = 0; e < L.n_experts; e++) {
            expected++;
            if (L.own[((size_t)l * L.n_experts + e) * LUMI_MAX_REP] < 0) missing++;
        }
    }
    L.expected = expected;
    return missing;
}

static void lumi_enable_if_complete(void) {
    if (L.on) return;
    int missing = lumi_missing_experts();
    if (missing) return;
    L.on = 1;
    fprintf(stderr, "[lumabri] phase 2 active: every expert runs on a peer, "
                    "%d peer(s), %d experts, hidden=%d, nearest replica "
                    "preferred%s\n",
            L.npeers, L.expected, L.hidden,
            L.verify_pct ? " · spot-check verification on" : "");
}

static void lumi_maybe_discover(void) {
    if (!L.initialized || !L.discovery) return;
    double now = lumi_now();
    if (now < L.next_discover) return;
    L.next_discover = now + L.discover_period_s;
    if (!L.have_identity) lumi_refresh_identity();
    int added = lumi_discover();
    if (added)
        fprintf(stderr, "[lumabri] discovered %d new expert peer(s) mid-generation\n",
                added);
    lumi_enable_if_complete();
}

/* The bootstrap-and-delegate policy, chatter side. Peer list from
 * LUMABRI_EXPERTS when set (explicit, any gap is fatal); otherwise
 * discovered from the tracker — and if the swarm cannot cover every
 * expert, phase 2 simply stays off and the engine runs the experts
 * itself from the phase-1 mirror. Graceful in, graceful out. */
/* `routed` is the per-layer-slot mask of which slots actually route to
 * experts — NULL means all of them. Every engine but olmoe has dense layers
 * (and colibri an MTP slot at index n_layers), and without the mask their
 * non-existent experts would count as missing and switch phase 2 off on
 * every model that has one. */
static void lumi_init_ex(int n_layers, int n_experts, int hidden,
                         const unsigned char *routed) {
    const char *spec = getenv("LUMABRI_EXPERTS");
    if (getenv("LUMABRI_NO_EXEC")) return;
    const char *tracker = getenv("LUMABRI_TRACKER");
    int discovery = !spec || !*spec;
    if (discovery && (!tracker || !tracker[0])) return;
    if (n_layers <= 0 || n_experts <= 0 || hidden <= 0 ||
        (size_t)n_layers > SIZE_MAX / (size_t)n_experts / LUMI_MAX_REP)
        lumi_die("invalid expert topology");

    L.n_layers = n_layers; L.n_experts = n_experts; L.hidden = hidden;
    L.discovery = discovery;
    L.discover_period_s = (double)lmb_env_int("LUMABRI_DISCOVERY_MS", 5000,
                                              100, 600000) / 1000.0;
    L.next_discover = lumi_now() + L.discover_period_s;
    lmb_build_profile(L.profile, sizeof L.profile);
    L.expected_bits = lmb_env_int("LUMABRI_EXPERT_BITS", LMBE_EXPECT_BITS,
                                  0, 32);
    const char *model = getenv("LUMABRI_MODEL");
    if (model) snprintf(L.expected_model, sizeof L.expected_model, "%s", model);
    lumi_load_pubkey();
    if (getenv("LUMABRI_PUBKEY") && !L.have_pubkey) return;
    if (tracker && tracker[0] && L.expected_model[0]) lumi_refresh_identity();
    size_t cells = (size_t)n_layers * n_experts * LUMI_MAX_REP;
    L.own = (int *)malloc(cells * sizeof(int));
    if (!L.own) lumi_die("out of memory creating expert map");
    for (size_t i = 0; i < cells; i++) L.own[i] = -1;
    L.routed = NULL;
    if (routed) {
        L.routed = (unsigned char *)malloc((size_t)n_layers);
        if (L.routed) memcpy(L.routed, routed, (size_t)n_layers);
    }
    const char *v = getenv("LUMABRI_VERIFY");
    if (v) {
        L.verify_pct = atoi(v);
        if (L.verify_pct < 0) L.verify_pct = 0;
        if (L.verify_pct > 100) L.verify_pct = 100;
    }
    L.initialized = 1;

    if (discovery) {
        int found = lumi_discover();
        if (!found) {
            fprintf(stderr, "[lumabri] no expert peers on the swarm — "
                            "running experts locally\n");
        } else
            fprintf(stderr, "[lumabri] discovered %d expert peer(s) from the tracker\n",
                    found);
    } else {
        char list[1024];
        snprintf(list, sizeof list, "%s", spec);
        for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save))
            if (lumi_add_peer(tok)) lumi_die("configured expert peer failed");
    }

    int missing = lumi_missing_experts();
    if (missing) {
        fprintf(stderr, "[lumabri] %d of %d experts have no peer — ", missing,
                L.expected);
        if (discovery) {
            fprintf(stderr, "running experts locally\n");
            return; /* layer_on keeps discovering and enables once complete */
        }
        fprintf(stderr, "refusing to run "
                "(a partial explicit network would silently change the model)\n");
        exit(1);
    }
    lumi_enable_if_complete();
}

/* olmoe's shape: every layer routes */
static void lumi_init(int n_layers, int n_experts, int hidden) {
    lumi_init_ex(n_layers, n_experts, hidden, NULL);
}

/* Does this (slot, expert) pair belong on the swarm? The engines call it
 * before handing a layer over, so an unrouted slot is never a wire error. */
static int lumi_layer_on(int layer) {
    lumi_maybe_discover();
    if (!L.on || layer < 0 || layer >= L.n_layers) return 0;
    return !L.routed || L.routed[layer];
}

/* nearest live replica of (layer,eid) not yet tried this call, or -1 */
static int lumi_pick(int gid, uint32_t tried) {
    const int *own = &L.own[(size_t)gid * LUMI_MAX_REP];
    int best = -1;
    long bestr = LONG_MAX;
    for (int r = 0; r < LUMI_MAX_REP; r++) {
        int pi = own[r];
        if (pi < 0 || ((tried >> r) & 1) || L.peers[pi].dead) continue;
        if (best < 0 || L.peers[pi].rtt_us < bestr)
            { best = r; bestr = L.peers[pi].rtt_us; }
    }
    return best;
}

/* One request carries every row that this layer routed to this expert.
 *
 * Sending them one at a time would be the obvious thing and it is wrong:
 * an engine that computes an expert over nr rows at once does not produce
 * the same floats as nr separate single-row calls — different accumulation
 * order in the kernels, last-bit differences, and a few tokens later the
 * generation has visibly diverged. Batching keeps remote and local doing
 * literally the same arithmetic, and as a bonus a 12-token prefill costs one
 * round trip per expert instead of twelve. */
/* `w` non-NULL: the router weights travel WITH the activations and the peer
 * applies them. Every engine but DeepSeek V4 leaves them here, because there
 * `w · expert(x)` is the same number either way. V4 folds the weight in
 * before the down projection and rounds the result to bf16, so it is not a
 * scale at all and a chatter-side multiply would quietly differ. The body
 * says which: 16 bytes = no weights, 16 + nr*4 = weights follow. */
static int lumi_send_exec(int fd, int layer, int eid, const float *x, int D, int nr,
                          const float *w) {
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)layer);
    lmb_buf_u32(&b, (uint32_t)eid);
    lmb_buf_u32(&b, (uint32_t)D);
    lmb_buf_u32(&b, (uint32_t)nr);
    if (w) lmb_buf_bytes(&b, w, (size_t)nr * sizeof(float));
    int rc = lmb_send(fd, LMB_EXEC, b.p, (uint32_t)b.len,
                      x, (uint32_t)((size_t)nr * D * sizeof(float)));
    free(b.p);
    return rc;
}

/* the failover path: run one expert synchronously on the next replicas.
 * Costs a full extra round trip — it is the price of a peer dying, paid
 * once, instead of the generation dying with it. */
static float *lumi_exec_retry(int layer, int eid, const float *x, int D, int nr,
                              const float *w, uint32_t tried) {
    int gid = layer * L.n_experts + eid;
    uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
    int waited = 0;
    for (;;) {
        int r = lumi_pick(gid, tried);
        if (r < 0) {
            /* Every known replica is gone. Giving up here is correct in the
             * sense that inventing a result is never allowed — but it threw
             * away a whole generation for a peer that was rebooting, or a
             * network blip, or an ssh session that took the server with it.
             * A swarm with one holder per expert has no redundancy to fall
             * back on, so the only honest thing left is patience: ask the
             * tracker again, and wait, saying so. */
            if (waited < LUMI_WAIT_S) {
                if (!waited)
                    fprintf(stderr, "[lumabri] layer %d expert %d: nessuna replica "
                            "viva — aspetto che torni (fino a %d s)\n",
                            layer, eid, LUMI_WAIT_S);
                tried = 0;                    /* a returning peer deserves a retry */
                sleep(2);
                waited += 2;
                if (L.discovery) lumi_discover();
                else for (int i = 0; i < L.npeers; i++)
                    if (L.peers[i].dead) {
                        char addr[64];
                        snprintf(addr, sizeof addr, "%s", L.peers[i].addr);
                        lumi_add_peer(addr);   /* revalidates manifest before reuse */
                    }
                continue;
            }
            fprintf(stderr, "[lumabri] layer %d expert %d: nessuna replica viva "
                    "dopo %d s. Con un solo detentore per esperto qualunque "
                    "interruzione e' definitiva: serve un secondo peer.\n",
                    layer, eid, waited);
            exit(1);
        }
        tried |= 1u << r;
        LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
        int fd = lumi_take_sock(p);
        if (fd < 0) continue;
        LmbMsg m = {0};
        if (lumi_send_exec(fd, layer, eid, x, D, nr, w) || lmb_recv(fd, &m) ||
            m.op != LMB_EXEC_R || m.pay_len != want) {
            close(fd);
            p->dead = 1;
            lmb_msg_free(&m);
            fprintf(stderr, "[lumabri] peer %s failed — trying next replica\n", p->addr);
            continue;
        }
        float *res = (float *)m.pay;
        m.pay = NULL;
        lmb_msg_free(&m);
        lumi_put_sock(p, fd);
        L.failovers++;
        return res;
    }
}

/* Spot-check: rerun this expert on a DIFFERENT replica and demand the same
 * bytes. Determinism makes lying detectable: two honest peers cannot
 * disagree, so a disagreement IS an attack (or broken hardware) — either
 * way the answer cannot be trusted, and the run stops loudly rather than
 * emit a token nobody can vouch for. */
static void lumi_spot_check(int layer, int eid, const float *x, int D, int nr,
                            const float *w, const float *got, LumiPeer *from,
                            uint32_t tried) {
    int gid = layer * L.n_experts + eid;
    int r2 = lumi_pick(gid, tried);
    if (r2 < 0) return;                       /* no second replica to ask */
    LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r2]];
    int fd = lumi_take_sock(p);
    if (fd < 0) return;
    uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
    LmbMsg m = {0};
    if (lumi_send_exec(fd, layer, eid, x, D, nr, w) || lmb_recv(fd, &m) ||
        m.op != LMB_EXEC_R || m.pay_len != want) {
        close(fd);
        lmb_msg_free(&m);
        return;                               /* checker down ≠ answer wrong */
    }
    L.verified++;
    int same = memcmp(got, m.pay, (size_t)want) == 0;
    lmb_msg_free(&m);
    lumi_put_sock(p, fd);
    if (!same) {
        fprintf(stderr, "[lumabri] INTEGRITY FAILURE on layer %d expert %d: "
                "%s and %s returned different bytes for the same activation. "
                "One of them is lying or broken; refusing to continue.\n",
                layer, eid, from->addr, p->addr);
        exit(1);
    }
}

/* Run the K selected experts of one layer on their peers and accumulate into
 * `out` with the router weights. A peer failure costs a retry on the next
 * replica; only a replica-exhausted expert is fatal. */
static void lumi_moe_apply(int layer, const int *idx, const float *val, int K,
                           const float *x, int D, float *out) {
    if (K > LUMI_MAX_K) lumi_die("top-k larger than the client supports");
    int fds[LUMI_MAX_K];
    uint32_t tried[LUMI_MAX_K];
    LumiPeer *ps[LUMI_MAX_K];
    float *res[LUMI_MAX_K];
    double t0 = lumi_now();

    /* issue all K first — this is what buys one RTT per layer */
    for (int k = 0; k < K; k++) {
        int gid = layer * L.n_experts + idx[k];
        tried[k] = 0; fds[k] = -1; ps[k] = NULL; res[k] = NULL;
        for (;;) {
            int r = lumi_pick(gid, tried[k]);
            if (r < 0) break;                  /* collect phase will retry/die */
            tried[k] |= 1u << r;
            LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
            int fd = lumi_take_sock(p);
            if (fd < 0) continue;
            if (lumi_send_exec(fd, layer, idx[k], x, D, 1, NULL)) {
                close(fd);
                p->dead = 1;
                continue;
            }
            fds[k] = fd; ps[k] = p;
            break;
        }
    }
    /* then collect, in order; a failed reply falls over to the next replica */
    static unsigned vseed = 0x9e3779b9u;
    for (int k = 0; k < K; k++) {
        if (fds[k] >= 0) {
            LmbMsg m = {0};
            if (lmb_recv(fds[k], &m) == 0 && m.op == LMB_EXEC_R &&
                m.pay_len == (uint32_t)(D * sizeof(float))) {
                res[k] = (float *)m.pay;
                m.pay = NULL;
                lmb_msg_free(&m);
                lumi_put_sock(ps[k], fds[k]);
                if (L.verify_pct) {
                    vseed = vseed * 1664525u + 1013904223u;
                    if ((int)(vseed % 100u) < L.verify_pct)
                        lumi_spot_check(layer, idx[k], x, D, 1, NULL, res[k], ps[k], tried[k]);
                }
                continue;
            }
            close(fds[k]);
            ps[k]->dead = 1;
            lmb_msg_free(&m);
            fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                            "trying next replica\n", ps[k]->addr, layer, idx[k]);
        }
        res[k] = lumi_exec_retry(layer, idx[k], x, D, 1, NULL, tried[k]);
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

/* ---- the batched form: one whole layer, every position at once -----------
 *
 * olmoe computes an expert one row at a time, so the per-position call above
 * is already what its local path does. Every other colibri engine gathers
 * ALL the rows a layer routed to an expert and computes them in one call —
 * and nr rows in one call is not the same arithmetic as nr calls of one row.
 * Feeding the peers per-position there produces a generation that matches
 * for three or four tokens and then visibly drifts.
 *
 * So the union, the row gather, and the accumulation order below are copied
 * from the engine deliberately, down to the blocks of 64 and the `break`
 * that takes a position only once even if it routed to the same expert
 * twice. This is where "the network may not change the model" is either
 * true or false.
 *
 * It is also, incidentally, much less network: a 12-token prefill costs one
 * round trip per expert instead of twelve.
 */
#define LUMI_BLOCK 64

static void lumi_moe_apply_batch(int layer, const int *idxs, const float *ws,
                                 const int *keff, int K, const float *x,
                                 int S, int D, float *out) {
    unsigned char *seen = (unsigned char *)calloc((size_t)L.n_experts, 1);
    int *uniq = (int *)malloc((size_t)L.n_experts * sizeof(int));
    int *rows = (int *)malloc((size_t)S * LUMI_BLOCK * sizeof(int));
    float *rw  = (float *)malloc((size_t)S * LUMI_BLOCK * sizeof(float));
    float *xg  = (float *)malloc((size_t)S * LUMI_BLOCK * (size_t)D * sizeof(float));
    if (!seen || !uniq || !rows || !rw || !xg) lumi_die("out of memory batching a layer");
    int nu = 0;
    for (int s = 0; s < S; s++)
        for (int kk = 0; kk < keff[s]; kk++) {
            int e = idxs[(int64_t)s * K + kk];
            if (e >= 0 && e < L.n_experts && !seen[e]) { seen[e] = 1; uniq[nu++] = e; }
        }

    double t0 = lumi_now();
    int fds[LUMI_BLOCK];
    uint32_t tried[LUMI_BLOCK];
    LumiPeer *ps[LUMI_BLOCK];
    int nrs[LUMI_BLOCK];
    static unsigned vseed = 0x9e3779b9u;

    for (int base = 0; base < nu; base += LUMI_BLOCK) {
        int nb = nu - base < LUMI_BLOCK ? nu - base : LUMI_BLOCK;
        /* gather each expert's rows, then issue every request before reading
         * a single reply — one round trip for the block, not nb of them */
        for (int j = 0; j < nb; j++) {
            int eid = uniq[base + j], nr = 0;
            int *rj = rows + (size_t)j * S;
            float *wj = rw + (size_t)j * S;
            for (int s = 0; s < S; s++)
                for (int kk = 0; kk < keff[s]; kk++)
                    if (idxs[(int64_t)s * K + kk] == eid) {
                        rj[nr] = s; wj[nr] = ws[(int64_t)s * K + kk]; nr++; break;
                    }
            nrs[j] = nr;
            float *xj = xg + (size_t)j * S * D;
            for (int r = 0; r < nr; r++)
                memcpy(xj + (size_t)r * D, x + (int64_t)rj[r] * D, (size_t)D * sizeof(float));

            int gid = layer * L.n_experts + eid;
            tried[j] = 0; fds[j] = -1; ps[j] = NULL;
            for (;;) {
                int r = lumi_pick(gid, tried[j]);
                if (r < 0) break;                  /* the collect phase retries */
                tried[j] |= 1u << r;
                LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
                int fd = lumi_take_sock(p);
                if (fd < 0) continue;
                if (lumi_send_exec(fd, layer, eid, xj, D, nr, NULL)) {
                    close(fd); p->dead = 1; continue;
                }
                fds[j] = fd; ps[j] = p;
                break;
            }
        }
        for (int j = 0; j < nb; j++) {
            int eid = uniq[base + j], nr = nrs[j];
            uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
            float *res = NULL, *xj = xg + (size_t)j * S * D;
            if (fds[j] >= 0) {
                LmbMsg m = {0};
                if (lmb_recv(fds[j], &m) == 0 && m.op == LMB_EXEC_R && m.pay_len == want) {
                    res = (float *)m.pay;
                    m.pay = NULL;
                    lmb_msg_free(&m);
                    lumi_put_sock(ps[j], fds[j]);
                    if (L.verify_pct) {
                        vseed = vseed * 1664525u + 1013904223u;
                        if ((int)(vseed % 100u) < L.verify_pct)
                            lumi_spot_check(layer, eid, xj, D, nr, NULL, res, ps[j], tried[j]);
                    }
                } else {
                    close(fds[j]);
                    ps[j]->dead = 1;
                    lmb_msg_free(&m);
                    fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                    "trying next replica\n", ps[j]->addr, layer, eid);
                }
            }
            if (!res) res = lumi_exec_retry(layer, eid, xj, D, nr, NULL, tried[j]);
            int *rj = rows + (size_t)j * S;
            float *wj = rw + (size_t)j * S;
            for (int r = 0; r < nr; r++) {
                float *os = out + (int64_t)rj[r] * D, w = wj[r];
                const float *hr = res + (size_t)r * D;
                for (int d = 0; d < D; d++) os[d] += w * hr[d];
            }
            free(res);
            L.calls++;
        }
    }
    L.wait_s += lumi_now() - t0;
    L.layers_done++;
    free(seen); free(uniq); free(rows); free(rw); free(xg);
}

/* The same batching, for an engine that has already built the union itself.
 *
 * kimi_k3 arrives here with the work already grouped: nu distinct experts,
 * and for each one the list of positions that routed to it with their
 * weights. It also runs its experts in the LATENT space, so `D` here is
 * c->latent, not hidden — the peer must agree, which is what the manifest's
 * dimension check enforces. */
static void lumi_moe_apply_union(int layer, int nu, const int *uid,
                                 const int *pfirst, const int *pcnt,
                                 const int *poslist, const float *wlist,
                                 const float *Z, int D, float *U) {
    if (nu <= 0) return;
    int maxrows = 0;
    for (int j = 0; j < nu; j++) if (pcnt[j] > maxrows) maxrows = pcnt[j];
    float *xg = (float *)malloc((size_t)maxrows * LUMI_BLOCK * (size_t)D * sizeof(float));
    if (!xg) lumi_die("out of memory batching a layer");
    double t0 = lumi_now();
    int fds[LUMI_BLOCK];
    uint32_t tried[LUMI_BLOCK];
    LumiPeer *ps[LUMI_BLOCK];
    static unsigned vseed = 0x85ebca6bu;

    for (int base = 0; base < nu; base += LUMI_BLOCK) {
        int nb = nu - base < LUMI_BLOCK ? nu - base : LUMI_BLOCK;
        for (int j = 0; j < nb; j++) {
            int eid = uid[base + j], nr = pcnt[base + j], f = pfirst[base + j];
            float *xj = xg + (size_t)j * maxrows * D;
            for (int r = 0; r < nr; r++)
                memcpy(xj + (size_t)r * D, Z + (int64_t)poslist[f + r] * D,
                       (size_t)D * sizeof(float));
            int gid = layer * L.n_experts + eid;
            tried[j] = 0; fds[j] = -1; ps[j] = NULL;
            for (;;) {
                int r = lumi_pick(gid, tried[j]);
                if (r < 0) break;
                tried[j] |= 1u << r;
                LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
                int fd = lumi_take_sock(p);
                if (fd < 0) continue;
                if (lumi_send_exec(fd, layer, eid, xj, D, nr, NULL)) {
                    close(fd); p->dead = 1; continue;
                }
                fds[j] = fd; ps[j] = p;
                break;
            }
        }
        for (int j = 0; j < nb; j++) {
            int eid = uid[base + j], nr = pcnt[base + j], f = pfirst[base + j];
            uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
            float *res = NULL, *xj = xg + (size_t)j * maxrows * D;
            if (fds[j] >= 0) {
                LmbMsg m = {0};
                if (lmb_recv(fds[j], &m) == 0 && m.op == LMB_EXEC_R && m.pay_len == want) {
                    res = (float *)m.pay;
                    m.pay = NULL;
                    lmb_msg_free(&m);
                    lumi_put_sock(ps[j], fds[j]);
                    if (L.verify_pct) {
                        vseed = vseed * 1664525u + 1013904223u;
                        if ((int)(vseed % 100u) < L.verify_pct)
                            lumi_spot_check(layer, eid, xj, D, nr, NULL, res, ps[j], tried[j]);
                    }
                } else {
                    close(fds[j]);
                    ps[j]->dead = 1;
                    lmb_msg_free(&m);
                    fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                    "trying next replica\n", ps[j]->addr, layer, eid);
                }
            }
            if (!res) res = lumi_exec_retry(layer, eid, xj, D, nr, NULL, tried[j]);
            for (int r = 0; r < nr; r++) {
                float *us = U + (int64_t)poslist[f + r] * D, w = wlist[f + r];
                const float *hr = res + (size_t)r * D;
                for (int d = 0; d < D; d++) us[d] += w * hr[d];
            }
            free(res);
            L.calls++;
        }
    }
    L.wait_s += lumi_now() - t0;
    L.layers_done++;
    free(xg);
}

/* ---- DeepSeek V4 -------------------------------------------------------
 *
 * V4 differs from the other four in two ways that both reach the wire.
 *
 * The router weight is not a scale. coli_v4_expert_forward_ref folds it in
 * BEFORE the down projection and rounds the product to bf16, so the peer has
 * to receive it and apply it — see lumi_send_exec. What comes back is
 * already weighted, and the chatter only adds.
 *
 * And the union is in ASCENDING EXPERT ID, not first-seen order: the engine
 * builds it with `for expert 0..n if used[expert]`, and within an expert it
 * walks the batch in order. Both are copied here, because the order of a
 * float sum is part of the answer.
 */
static void lumi_moe_apply_v4(int layer, const int *indices, const float *weights,
                              int topk, const float *x, int batch, int D,
                              float *out) {
    unsigned char *used = (unsigned char *)calloc((size_t)L.n_experts, 1);
    int *uid = (int *)malloc((size_t)L.n_experts * sizeof(int));
    int *rows = (int *)malloc((size_t)batch * LUMI_BLOCK * sizeof(int));
    float *rw = (float *)malloc((size_t)batch * LUMI_BLOCK * sizeof(float));
    float *xg = (float *)malloc((size_t)batch * LUMI_BLOCK * (size_t)D * sizeof(float));
    if (!used || !uid || !rows || !rw || !xg) lumi_die("out of memory batching a layer");
    for (int item = 0; item < batch; item++)
        for (int k = 0; k < topk; k++) {
            int e = indices[(size_t)item * topk + k];
            if (e >= 0 && e < L.n_experts) used[e] = 1;
        }
    int nu = 0;
    for (int e = 0; e < L.n_experts; e++) if (used[e]) uid[nu++] = e;

    double t0 = lumi_now();
    int fds[LUMI_BLOCK];
    uint32_t tried[LUMI_BLOCK];
    LumiPeer *ps[LUMI_BLOCK];
    int nrs[LUMI_BLOCK];
    static unsigned vseed = 0xc2b2ae35u;

    for (int base = 0; base < nu; base += LUMI_BLOCK) {
        int nb = nu - base < LUMI_BLOCK ? nu - base : LUMI_BLOCK;
        for (int j = 0; j < nb; j++) {
            int eid = uid[base + j], nr = 0;
            int *rj = rows + (size_t)j * batch;
            float *wj = rw + (size_t)j * batch;
            for (int item = 0; item < batch; item++) {
                int rank = -1;
                for (int k = 0; k < topk; k++)
                    if (indices[(size_t)item * topk + k] == eid) rank = k;
                if (rank < 0) continue;                    /* engine takes the LAST match */
                rj[nr] = item;
                wj[nr] = weights[(size_t)item * topk + rank];
                nr++;
            }
            nrs[j] = nr;
            float *xj = xg + (size_t)j * batch * D;
            for (int r = 0; r < nr; r++)
                memcpy(xj + (size_t)r * D, x + (size_t)rj[r] * D, (size_t)D * sizeof(float));

            int gid = layer * L.n_experts + eid;
            tried[j] = 0; fds[j] = -1; ps[j] = NULL;
            for (;;) {
                int r = lumi_pick(gid, tried[j]);
                if (r < 0) break;
                tried[j] |= 1u << r;
                LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
                int fd = lumi_take_sock(p);
                if (fd < 0) continue;
                if (lumi_send_exec(fd, layer, eid, xj, D, nr, wj)) {
                    close(fd); p->dead = 1; continue;
                }
                fds[j] = fd; ps[j] = p;
                break;
            }
        }
        for (int j = 0; j < nb; j++) {
            int eid = uid[base + j], nr = nrs[j];
            uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
            float *res = NULL, *xj = xg + (size_t)j * batch * D;
            float *wj = rw + (size_t)j * batch;
            int *rj = rows + (size_t)j * batch;
            if (fds[j] >= 0) {
                LmbMsg m = {0};
                if (lmb_recv(fds[j], &m) == 0 && m.op == LMB_EXEC_R && m.pay_len == want) {
                    res = (float *)m.pay;
                    m.pay = NULL;
                    lmb_msg_free(&m);
                    lumi_put_sock(ps[j], fds[j]);
                    if (L.verify_pct) {
                        vseed = vseed * 1664525u + 1013904223u;
                        if ((int)(vseed % 100u) < L.verify_pct)
                            lumi_spot_check(layer, eid, xj, D, nr, wj, res, ps[j], tried[j]);
                    }
                } else {
                    close(fds[j]);
                    ps[j]->dead = 1;
                    lmb_msg_free(&m);
                    fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                    "trying next replica\n", ps[j]->addr, layer, eid);
                }
            }
            if (!res) res = lumi_exec_retry(layer, eid, xj, D, nr, wj, tried[j]);
            for (int r = 0; r < nr; r++) {       /* already weighted by the peer */
                float *os = out + (size_t)rj[r] * D;
                const float *hr = res + (size_t)r * D;
                for (int d = 0; d < D; d++) os[d] += hr[d];
            }
            free(res);
            L.calls++;
        }
    }
    L.wait_s += lumi_now() - t0;
    L.layers_done++;
    free(used); free(uid); free(rows); free(rw); free(xg);
}

static void lumi_report(void) {
    if (!L.on) return;
    fprintf(stderr, "[lumabri] %llu remote expert calls in %llu layer rounds · "
                    "%.2fs waiting on peers (%.2f ms per layer round) · "
                    "%llu failover(s) · %llu spot-check(s), all agreed\n",
            L.calls, L.layers_done, L.wait_s,
            L.layers_done ? 1000.0 * L.wait_s / (double)L.layers_done : 0.0,
            L.failovers, L.verified);
}

#endif /* LUMABRI_CLIENT_H */
