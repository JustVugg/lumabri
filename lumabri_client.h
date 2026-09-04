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
#include <poll.h>

#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "lumabri_scheduler.h"

#define LUMI_MAX_PEERS 64
#define LUMI_MAX_K     64
#define LUMI_MAX_REP   4      /* replicas remembered per expert */
#define LUMI_WAIT_S    30     /* how long a vanished peer is given to come back */
/* what a tracker tunnel costs beyond the probe that stops at the tracker:
 * the tracker's hop to the peer and back, home-line grade */
#define LUMI_TUNNEL_PENALTY_US 60000u
#define LUMI_RTT_REFRESH_S 30.0
/* How often one peer's residency report is read again. A cache warms, a
 * measured rate replaces the opening estimate, a node is rebalanced, a
 * cache starts missing: all of that moves, and reading it once when the
 * peer joined publishes a dynamic number through a static channel. Slower
 * than the RTT cadence and on its own connection, so the pooled sockets the
 * RTT refresh accounts for are left alone. */
#define LUMI_ERES_REFRESH_S 60.0
#define LUMI_RTT_SAMPLES   2
#ifndef LUMI_RTT_PROBE_TIMEOUT_MS
#define LUMI_RTT_PROBE_TIMEOUT_MS 2000
#endif

typedef struct {
    char addr[64];
    /* Non-empty: this peer cannot be dialed at addr (NAT); every socket for
     * it goes to the TRACKER instead and each EXEC travels as a targeted
     * LMB_TEXEC naming addr. The reply is a plain LMB_EXEC_R, so above the
     * socket layer this peer is indistinguishable from a dialable one:
     * same predictors, same spreading, same hedging, same spot-check. */
    char relay_target[64];
    /* Non-empty: the advertised address is this very machine seen from the
     * outside. A donor node spawned next to the chat is advertised at the
     * public IP the tracker observed, and a NAT rarely lets its own WAN
     * address be dialed from inside (no hairpin) — so without this, a chat
     * would reach the experts sitting in its OWN RAM by a round trip
     * through the tracker. Sockets dial this instead; identity, canonical
     * ordering and telemetry keep using addr. */
    char loopback[64];
    int socks[LUMI_MAX_K];
    int nsocks;
    long rtt_us;
    double rtt_probe_at;     /* last initial or staggered RTT measurement attempt */
    int dead;
    double retry_at;          /* do not redial a relay-only/NAT address per layer */
    LmbLatencyPredictor latency;
    uint32_t inflight;
    uint64_t exec_observations, exec_observations_at_probe;
    /* Where this executor keeps the experts it holds: 2 in VRAM, 1 in RAM,
     * 0 streamed from disk, -1 unknown (older node, or reached only through
     * the tracker tunnel). A prior on the service time, used until this peer
     * has answered enough calls for the measurement to speak. */
    int resident;
    uint32_t hot_permille;      /* calls served without a disk read, per 1000 */
    uint32_t hot_experts;       /* experts this node keeps hot */
    uint32_t held_experts;      /* experts it serves in all */
    double eres_read_at;        /* when the residency report was last read */
    /* the bill: what this peer answered, how long it took, and the bytes
     * that crossed the wire each way — the report divides them by rounds */
    unsigned long long ok_calls, bytes_out, bytes_in;
    double lat_s;
    uint32_t caps;              /* LMB_CAP_* the executor advertised (ERES) */
} LumiPeer;

static struct {
    int on, initialized, discovery;
    LumiPeer peers[LUMI_MAX_PEERS];
    int npeers;
    int *own;                   /* [gid * LUMI_MAX_REP] → peer index, -1 = free */
    unsigned char *relay;       /* tracker tunnel covers this (slot,expert) */
    unsigned char *routed;      /* [n_layers] 1 = this slot routes to experts */
    unsigned char *layer_ok;    /* [n_layers] 1 = every expert of it has a peer */
    int ok_layers, routed_layers, announced_ok;
    int n_layers, n_experts, hidden;
    int expected, expected_bits;
    char expected_model[64], engine_id[64], profile[LMB_BUILD_PROFILE_MAX];
    LmbModelIdentity identity; int have_identity;
    LmbTrustKeys trust;
    double next_discover, discover_period_s;
    double next_rtt_refresh;  /* one healthy pooled peer per slow cadence */
    double next_eres_refresh; /* likewise for the residency report */
    int verify_pct;             /* LUMABRI_VERIFY: % of calls double-checked */
    int allow_codegen_skew;     /* LUMABRI_ALLOW_CODEGEN_SKEW: cc/isa -> warn, not refuse */
    int spread;                 /* LUMABRI_SPREAD: near-band replica spreading, not strict argmin */
    /* Hedging policy, from LUMABRI_HEDGE_MS:
     *   < 0  off — no second replica is ever issued
     *     0  automatic (default): the predictor hedges only once a peer has
     *        enough samples and a genuinely heavy tail
     *   > 0  fixed delay in milliseconds, the original base policy
     * Off matters to anyone who needs deterministic attribution — a
     * measurement harness cannot tell a chosen replica from a hedge copy. */
    int hedge_ms;
    /* How long one EXEC reply may take before the replica is treated as
     * failed. A dead peer resets its socket and fails instantly; a peer that
     * is merely wedged — swapping, saturated, or behind a black hole — never
     * answers and never errors, so only this bound turns it into a failover
     * instead of a stall. LUMABRI_EXEC_WAIT_MS raises it for slow links. */
    int exec_wait_ms;
    /* Survival policy. A chatter with a mirror must never die because the
     * swarm did: when every replica of an expert is gone and the tracker
     * relay cannot serve it either, the layer is DEMOTED — lumi_layer_on()
     * answers 0 for demote_s seconds, the engine's own local path computes
     * the layer from the mirror, and the swarm is retried when the window
     * expires. peer_wait_s is the old patience for a rebooting peer, now
     * tunable; one expired wait marks the whole swarm sick, so the next
     * failing layer demotes immediately instead of stalling the generation
     * for another full wait. */
    int peer_wait_s;            /* LUMABRI_PEER_WAIT_S, seconds */
    int demote_s;               /* LUMABRI_DEMOTE_S, seconds */
    double *demote_until;       /* [n_layers] monotonic deadline; 0 = active */
    double swarm_sick_until;
    int swarm_disabled;         /* model identity changed mid-run: local only */
    unsigned long long demotions, integrity_fails;
    unsigned long long calls, layers_done, failovers, verified, relays;
    double wait_max_s;          /* the slowest layer round so far: the tail a token pays */
    unsigned long long hedges, hedge_wins, batch_calls, batch_rows;
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

static uint64_t lumi_now_ms(void) { return (uint64_t)(lumi_now() * 1000.0); }

static void lumi_peer_failed(LumiPeer *peer) {
    lmb_predict_failure(&peer->latency, lumi_now_ms());
}

static void lumi_peer_observed(LumiPeer *peer, double started) {
    double took = lumi_now() - started;
    uint64_t elapsed = (uint64_t)(took * 1e6);
    lmb_predict_observe(&peer->latency, elapsed ? elapsed : 1);
    peer->exec_observations++;
    peer->ok_calls++;
    peer->lat_s += took;
}

static void lumi_peer_sent(LumiPeer *peer) { peer->inflight++; }
static void lumi_peer_done(LumiPeer *peer) {
    if (peer->inflight) peer->inflight--;
}

/* One socket per in-flight request: a peer executing two experts of the same
 * layer must see them as two concurrent requests, not a queue of one. A
 * transport failure feeds the circuit breaker; it does not erase a valid
 * manifest permanently. */
/* A pooled socket may have been closed by the executor while it sat idle
 * (its I/O timeout is minutes; a slow reply leaves sockets idle longer).
 * Reusing it means a send that succeeds into the kernel and a recv that
 * finds EOF: a "failed" call charged to a healthy peer, a retry, and after
 * a few of those an open circuit and a patience wait. A closed socket is
 * readable (EOF) before we write to it, so ask, and dial fresh instead. */
static int lumi_sock_alive(int fd) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int r = poll(&pfd, 1, 0);
    if (r < 0) return 1;                       /* cannot tell: use it */
    return r == 0;                             /* nothing pending = idle and open */
}

static int lumi_take_sock(LumiPeer *p) {
    while (p->nsocks) {
        int fd = p->socks[--p->nsocks];
        if (lumi_sock_alive(fd)) return fd;
        close(fd);                             /* stale: the peer hung up */
    }
    const char *dial = p->loopback[0] ? p->loopback : p->addr;
    if (p->relay_target[0]) {
        dial = getenv("LUMABRI_TRACKER");
        if (!dial || !dial[0]) { lumi_peer_failed(p); return -1; }
    }
    int fd = lmb_connect(dial);
    if (fd >= 0 && lmb_auth(fd)) { close(fd); fd = -1; }
    if (fd < 0) {
        fprintf(stderr, "[lumabri] peer %s unreachable — circuit failure\n", p->addr);
        lumi_peer_failed(p);
        return fd;
    }
    /* An expert call that has not answered in two minutes is lost, not slow:
     * the general five-minute I/O timeout cost a reply 10 minutes twice in
     * one hour. Long enough for a prefill block on a slow uplink. */
    lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_EXEC_TIMEOUT_MS", 120000, 1000, 3600000));
    return fd;
}

static void lumi_put_sock(LumiPeer *p, int fd) {
    if (p->nsocks < LUMI_MAX_K) p->socks[p->nsocks++] = fd;
    else close(fd);
}

/* Measure steady-state RTT on an already chosen socket. Both initial and
 * maintenance probes use the minimum of two warm PINGs so one delayed reply
 * cannot replace a useful distance estimate. The caller owns fd on failure. */
static long lumi_measure_rtt(int fd) {
    long best = LONG_MAX;
    for (int i = 0; i < LUMI_RTT_SAMPLES; i++) {
        double a = lumi_now();
        LmbMsg m = {0};
        if (lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) || lmb_recv(fd, &m))
            return LONG_MAX;
        int ok = m.op == LMB_OK && m.pay_len == 0;
        lmb_msg_free(&m);
        if (!ok) return LONG_MAX;
        long us = (long)((lumi_now() - a) * 1e6);
        if (us < best) best = us;
    }
    return best;
}

/* Maintenance runs synchronously at a layer boundary, so it uses a tighter
 * timeout than the bounded EXEC wait on the normal bulk path. Temporarily
 * bound both directions, then restore the pooled socket before real work. */
static long lumi_measure_rtt_bounded(int fd) {
    struct timeval old_recv, old_send;
    socklen_t recv_len = sizeof old_recv, send_len = sizeof old_send;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &old_recv, &recv_len) ||
        getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &old_send, &send_len))
        return LONG_MAX;
    struct timeval probe = { LUMI_RTT_PROBE_TIMEOUT_MS / 1000,
                             (LUMI_RTT_PROBE_TIMEOUT_MS % 1000) * 1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &probe, sizeof probe) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &probe, sizeof probe)) {
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &old_recv, recv_len);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &old_send, send_len);
        return LONG_MAX;
    }
    long measured = lumi_measure_rtt(fd);
    int restore_bad = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                                 &old_recv, recv_len);
    restore_bad |= setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                              &old_send, send_len);
    return restore_bad ? LONG_MAX : measured;
}

static void lumi_probe(LumiPeer *p) {
    p->rtt_us = LONG_MAX;
    p->rtt_probe_at = lumi_now();
    int fd = lumi_take_sock(p);
    if (fd < 0) return;
    long measured = lumi_measure_rtt(fd);
    if (measured == LONG_MAX) {
        close(fd); lumi_peer_failed(p); return;
    }
    p->rtt_us = measured;
    /* A peer adopted through the tracker tunnel is probed at the tracker,
     * not at the peer: on the origin host that read 0.13 ms for a donor two
     * countries away, made it the "nearest" replica, and sent 95% of the
     * calls into a tunnel that then hung. The tunnel is at least the
     * tracker's own hop to the peer plus the return: charge it. */
    if (p->relay_target[0] && p->rtt_us != LONG_MAX)
        p->rtt_us += LUMI_TUNNEL_PENALTY_US;
    lumi_put_sock(p, fd);
}

/* Refresh measurement only through an idle pooled connection. If every
 * socket is serving EXEC work, defer instead of opening a new connection and
 * accidentally measuring setup cost. A failed maintenance PING discards that
 * socket but leaves health/failover decisions to the next real EXEC. */
static int lumi_refresh_rtt(LumiPeer *p) {
    if (p->dead || p->nsocks <= 0) return -1;
    int idle_epoch = !p->inflight &&
                     p->exec_observations == p->exec_observations_at_probe;
    p->rtt_probe_at = lumi_now();
    int fd = p->socks[--p->nsocks];
    long measured = lumi_measure_rtt_bounded(fd);
    if (measured == LONG_MAX) { close(fd); return -1; }
    lumi_put_sock(p, fd);
    if (p->rtt_us == LONG_MAX) p->rtt_us = measured;
    else {
        /* EWMA alpha=1/2. The overflow-safe rounded mean damps a transient
         * queue delay while still adapting within one sweep when paths swap. */
        p->rtt_us = p->rtt_us / 2 + measured / 2 +
                    ((p->rtt_us & 1) && (measured & 1));
    }
    if (idle_epoch) {
        /* An active peer's EXEC samples are authoritative. Only an idle peer
         * needs the fresh transport baseline folded into scheduler scoring. */
        uint64_t sample = (uint64_t)measured;
        p->latency.ewma_us = p->latency.ewma_us / 2 + sample / 2 +
                             ((p->latency.ewma_us & 1) && (sample & 1));
    }
    p->exec_observations_at_probe = p->exec_observations;
    return 0;
}

static int lumi_index_ok(uint32_t layer, uint32_t eid) {
    return L.n_layers > 0 && L.n_experts > 0 &&
           layer < (uint32_t)L.n_layers && eid < (uint32_t)L.n_experts;
}

static void lumi_load_pubkey(void) {
    const char *spec = getenv("LUMABRI_PUBKEY");
    if (!spec || !*spec) return;
    if (lmb_trust_load_spec(&L.trust, spec))
        fprintf(stderr, "[lumabri] invalid LUMABRI_PUBKEY/keyring: "
                        "expert execution stays local\n");
}

static int lumi_identity_valid(const char *model, const LmbModelIdentity *id) {
    if (strcmp(model, id->model)) return 0;
    if (!L.trust.n) return 1;
    if (!id->has_sig) return 0;
    size_t n = 0;
    uint8_t *msg = lmb_model_id_msg(model, id->root, &n);
    int ok = msg && lmb_trust_verify(&L.trust, id->sig, msg, n) == 0;
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
        /* Refusing to mix checkpoints is right; killing the engine over it
         * is not. The mirror still holds the checkpoint this process
         * started with, so stop trusting peers and finish the run local. */
        if (!L.swarm_disabled)
            fprintf(stderr, "[lumabri] model identity changed during this "
                            "process — refusing to mix checkpoints: swarm "
                            "off, experts run locally from the mirror for "
                            "the rest of this run\n");
        L.swarm_disabled = 1;
        L.on = 0;
        return 0;
    }
    L.identity = id; L.have_identity = 1;
    return 1;
}

/* Where this executor keeps the experts it holds, and how much of that it
 * really keeps hot.
 *
 * ERES answers three words on every node, and three more on a node new
 * enough to count: experts kept hot, experts served, and calls answered per
 * thousand without a disk read. The silence of an older node is not "it
 * hits every time" — it is "we do not know", and the only safe reading of
 * an unknown disk node is the disk price. `held_experts == 0` is what says
 * the counts never arrived, and it is what the score checks.
 *
 * A tunnel-only peer is never asked: the question would be answered by the
 * tracker's socket, not the executor's. */
static void lumi_read_residency(LumiPeer *p) {
    p->resident = -1;
    p->hot_permille = 1000;
    p->hot_experts = p->held_experts = 0;
    p->eres_read_at = lumi_now();
    if (p->relay_target[0]) return;
    LmbMsg rm = {0};
    const char *dial = p->loopback[0] ? p->loopback : p->addr;
    if (!lmb_request(dial, LMB_ERES, NULL, 0, &rm) && rm.op == LMB_ERES_R) {
        LmbCur rc = { rm.body, rm.body_len, 0 };
        uint32_t flags = 0, state = 0, caps = 0;
        if (!lmb_cur_u32(&rc, &flags))
            p->resident = (flags & LMB_EXPERT_RESIDENT_VRAM) ? 2 :
                          (flags & LMB_EXPERT_DISK_FALLBACK) ? 0 : 1;
        if (!lmb_cur_u32(&rc, &state) && !lmb_cur_u32(&rc, &caps))
            p->caps = caps;               /* absent on older nodes: plain EXEC */
        uint32_t hot = 0, held = 0, permille = 0;
        if (!lmb_cur_u32(&rc, &hot) && !lmb_cur_u32(&rc, &held) &&
            !lmb_cur_u32(&rc, &permille) && held > 0) {
            p->hot_experts = hot;
            p->held_experts = held;
            p->hot_permille = permille > 1000 ? 1000 : permille;
        }
    }
    lmb_msg_free(&rm);
}

/* Learn one peer: manifest, replica claims, distance. Returns 0, or -1 on
 * any failure (already-known addresses are a no-op success). */
static int lumi_add_peer(const char *addr) {
    int reprobe = -1;
    for (int i = 0; i < L.npeers; i++)
        if (!strcmp(L.peers[i].addr, addr)) {
            LumiPeer *known = &L.peers[i];
            if (!known->dead) return 0;
            if (lumi_now() < known->retry_at) return -1;
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
    p->relay_target[0] = 0;
    p->loopback[0] = 0;
    if (lmb_request(p->addr, LMB_EMANIFEST, NULL, 0, &m) || m.op != LMB_EMANIFEST_R) {
        /* Not dialable — the normal case for a donor behind a home router.
         * Ask the tracker for the same manifest through the peer's own
         * tunnel; if it answers, this peer joins the table as a replica
         * whose route happens to run through the tracker. */
        lmb_msg_free(&m);
        memset(&m, 0, sizeof m);
        /* Hairpin check first: if the unreachable peer is a node on THIS
         * machine, its port answers on loopback with the same manifest the
         * advertised address refused — adopt it at RAM distance instead of
         * tunnel distance. Bounded to 500 ms because a loopback that exists
         * answers instantly and one that does not must not stall discovery
         * (some hosts drop instead of refusing). */
        const char *port_part = strrchr(p->addr, ':');
        int local = 0;
        if (port_part && strncmp(p->addr, "127.", 4) != 0) {
            char here[64];
            snprintf(here, sizeof here, "127.0.0.1%s", port_part);
            int fd = lmb_connect_ms(here, 500);
            if (fd >= 0 && !lmb_auth(fd) &&
                !lmb_send(fd, LMB_EMANIFEST, NULL, 0, NULL, 0) &&
                !lmb_recv(fd, &m) && m.op == LMB_EMANIFEST_R) {
                snprintf(p->loopback, sizeof p->loopback, "%s", here);
                local = 1;
            }
            if (fd >= 0) close(fd);
            if (!local) { lmb_msg_free(&m); memset(&m, 0, sizeof m); }
        }
        if (local)
            fprintf(stderr, "[lumabri] peer %s is this machine — dialing its "
                            "experts on loopback\n", p->addr);
        const char *tracker = getenv("LUMABRI_TRACKER");
        int relayed = 0;
        if (!local && tracker && tracker[0]) {
            LmbBuf tb = {0};
            lmb_buf_str(&tb, p->addr);
            relayed = !lmb_request(tracker, LMB_TMAN, tb.p, (uint32_t)tb.len,
                                   &m) && m.op == LMB_EMANIFEST_R;
            free(tb.p);
        }
        if (!local && !relayed) {
            fprintf(stderr, "[lumabri] no expert manifest from %s — skipped\n",
                    p->addr);
            lmb_msg_free(&m);
            p->dead = 1; p->retry_at = lumi_now() + 30.0;
            if (reprobe < 0) L.npeers++;   /* remember NAT-only addresses */
            return -1;
        }
        if (!local) {
            snprintf(p->relay_target, sizeof p->relay_target, "%s", p->addr);
            fprintf(stderr, "[lumabri] peer %s is not directly dialable — "
                            "adopting it through the tracker tunnel\n", p->addr);
        }
    }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t magic = 0, bits = 0, have_id = 0, has_sig = 0;
    uint32_t n = 0, peer_hidden = 0;
    char engine[64] = "", profile[LMB_BUILD_PROFILE_MAX] = "", peer_model[64] = "";
    char why[224] = "";   /* which manifest field made this peer incompatible */
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
    if (!bad) bad = lmb_cur_u32(&c, &n);
    if (!bad && n > (uint64_t)L.n_layers * (uint64_t)L.n_experts) {
        snprintf(why, sizeof why, "shape: peer lists %u experts, this model has %llu",
                 n, (unsigned long long)((uint64_t)L.n_layers * L.n_experts));
        bad = 1;
    }
    if (!bad && !why[0]) {
        if (strcmp(engine, L.engine_id))
            snprintf(why, sizeof why, "engine: peer='%s' vs local='%s'",
                     engine, L.engine_id);
        else if (strcmp(profile, L.profile)) {
            /* Field-aware: shape/ABI (abi/engine/f32) always gates; a build
             * difference (src/math/cc/isa) is admitted by default with the
             * spot-check as the net, refused under LUMABRI_STRICT=1; omp
             * never gates (version alone changes no bits). A blanket strcmp
             * used to refuse two boxes for a libgomp version or a gcc point
             * release with identical output. */
            char d[192];
            int r = lmb_profile_compat(profile, L.profile,
                                       L.allow_codegen_skew, d, sizeof d);
            if (r == 1)
                snprintf(why, sizeof why, "build %s", d);
            else if (r == 2)
                fprintf(stderr, "[lumabri] peer %s: build %s — admitted "
                        "(spot-check %s; LUMABRI_STRICT=1 rifiuta)\n",
                        p->addr, d,
                        L.verify_pct ? "on" : "OFF — set LUMABRI_VERIFY=<pct>");
            /* r == 0: differ only in omp — compatible, admit silently */
        } else if (bits != (uint32_t)L.expected_bits)
            snprintf(why, sizeof why, "bits: peer=%u vs local=%d",
                     bits, L.expected_bits);
        else if (L.expected_model[0] && strcmp(peer_model, L.expected_model))
            snprintf(why, sizeof why, "model: peer='%s' vs local='%s'",
                     peer_model, L.expected_model);
        else if ((L.have_identity || L.trust.n) && !have_id)
            snprintf(why, sizeof why, "peer sent no signed model identity");
        else if (have_id && !lumi_identity_valid(peer_model, &peer_id))
            snprintf(why, sizeof why, "peer model identity failed to verify");
        else if (L.have_identity && memcmp(L.identity.root, peer_id.root, 32))
            snprintf(why, sizeof why, "model identity differs from the pinned one");
        if (why[0]) bad = 1;
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
    int tail_bad = lmb_cur_u32(&c, &peer_hidden) || c.off != c.len || m.pay_len != 0;
    if (!bad && !why[0] && !tail_bad && peer_hidden != (uint32_t)L.hidden)
        snprintf(why, sizeof why, "shape: peer hidden=%u vs local hidden=%d",
                 peer_hidden, L.hidden);
    if (bad || tail_bad || peer_hidden != (uint32_t)L.hidden) {
        /* roll the claims back: this peer must not own anything */
        if (reprobe < 0)
            for (size_t i = 0; i < gids * LUMI_MAX_REP; i++)
                if (L.own[i] == pi) L.own[i] = -1;
        free(seen);
        lmb_msg_free(&m);
        fprintf(stderr, "[lumabri] peer %s: incompatible manifest — %s — skipped\n",
                p->addr,
                why[0] ? why : "malformed or model/build/engine/bits/shape mismatch");
        return -1;
    }
    if (!L.expected_model[0]) snprintf(L.expected_model, sizeof L.expected_model,
                                       "%s", peer_model);
    if (!L.have_identity && have_id) { L.identity = peer_id; L.have_identity = 1; }
    free(seen);
    lmb_msg_free(&m);
    p->dead = 0;
    p->retry_at = 0;
    p->inflight = 0;
    if (reprobe < 0) lmb_predict_init(&p->latency, 1000);
    lumi_probe(p);
    if (p->rtt_us == LONG_MAX) {
        if (reprobe < 0)
            for (size_t i = 0; i < gids * LUMI_MAX_REP; i++)
                if (L.own[i] == pi) L.own[i] = -1;
        return -1;
    }
    lmb_predict_observe(&p->latency, (uint64_t)p->rtt_us);
    p->exec_observations_at_probe = p->exec_observations;
    lumi_read_residency(p);
    char tier[96];
    if (p->resident == 0 && p->held_experts)
        snprintf(tier, sizeof tier,
                 "%u of %u experts hot, %u%% of calls served without the disk",
                 p->hot_experts, p->held_experts, p->hot_permille / 10);
    else
        snprintf(tier, sizeof tier, "%s",
                 p->resident == 2 ? "experts in VRAM" :
                 p->resident == 1 ? "experts in RAM" :
                 p->resident == 0 ? "experts streamed from disk (last resort)" :
                                    "residency unknown");
    fprintf(stderr, "[lumabri] peer %s: %u experts (%d first-holder) · rtt %.2f ms · %s\n",
            p->addr, n, claimed, (double)p->rtt_us / 1000.0, tier);
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

/* Relay-only nodes cannot be queried at their advertised address, so the
 * tracker returns the union of holder bitmaps whose engine/build/shape
 * metadata exactly matches this chatter. */
static int lumi_relay_coverage(void) {
    const char *tracker = getenv("LUMABRI_TRACKER");
    if (!tracker || !*tracker || !L.expected_model[0]) return 0;
    LmbBuf b = {0};
    lmb_buf_str(&b, L.expected_model);
    lmb_buf_str(&b, L.engine_id);
    lmb_buf_str(&b, L.profile);
    lmb_buf_u32(&b, (uint32_t)L.expected_bits);
    lmb_buf_u32(&b, (uint32_t)L.hidden);
    lmb_buf_u32(&b, (uint32_t)L.n_layers);
    lmb_buf_u32(&b, (uint32_t)L.n_experts);
    LmbMsg m = {0};
    int rc = lmb_request(tracker, LMB_ECOVER, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_ECOVER_R) { lmb_msg_free(&m); return 0; }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t slots = 0, nexp = 0, nb = 0;
    size_t cells = (size_t)L.n_layers * L.n_experts;
    int bad = lmb_cur_u32(&c, &slots) || lmb_cur_u32(&c, &nexp) ||
              lmb_cur_u32(&c, &nb) || slots != (uint32_t)L.n_layers ||
              nexp != (uint32_t)L.n_experts || nb != (cells + 7) / 8 ||
              c.off + nb != c.len;
    if (!bad) {
        if (!L.relay) L.relay = (unsigned char *)calloc(cells ? cells : 1, 1);
        if (!L.relay) bad = 1;
        else for (size_t gid = 0; gid < cells; gid++)
            L.relay[gid] = (c.p[c.off + (gid >> 3)] >> (gid & 7)) & 1;
    }
    lmb_msg_free(&m);
    return bad ? 0 : 1;
}

/* Coverage per LAYER, not per model. The engines hand a layer to the swarm
 * through lumi_layer_on(), so a layer whose every routed expert has a live
 * holder (or relay) can run remotely even while its neighbours run locally
 * from the mirror. The old policy was all-or-nothing: one uncovered expert
 * anywhere kept phase 2 dark and every donor idle — the single deepest
 * "my expert node does nothing" in the project's history (#52's field
 * report; a 5%-coverage donor was worth exactly zero). */
static int lumi_missing_experts(void) {
    int missing = 0, expected = 0, ok = 0, routed = 0;
    if (!L.layer_ok && L.n_layers > 0)
        L.layer_ok = (unsigned char *)calloc((size_t)L.n_layers, 1);
    for (int l = 0; l < L.n_layers; l++) {
        if (L.routed && !L.routed[l]) continue;
        routed++;
        int gap = 0;
        for (int e = 0; e < L.n_experts; e++) {
            expected++;
            size_t gid = (size_t)l * L.n_experts + e;
            if (L.own[gid * LUMI_MAX_REP] < 0 && (!L.relay || !L.relay[gid]))
                { missing++; gap++; }
        }
        if (L.layer_ok) L.layer_ok[l] = (gap == 0);
        if (!gap) ok++;
    }
    L.expected = expected;
    L.ok_layers = ok;
    L.routed_layers = routed;
    return missing;
}

static void lumi_enable_if_complete(void) {
    if (L.swarm_disabled) { L.on = 0; return; }
    int missing = lumi_missing_experts();
    L.on = L.ok_layers > 0;
    if (!L.on || L.ok_layers == L.announced_ok) return;
    L.announced_ok = L.ok_layers;
    int direct = 0;
    for (int i = 0; i < L.npeers; i++) if (!L.peers[i].dead) direct++;
    if (!missing) {
        /* Tell the byte-mirror in this same process to stop reading ahead:
         * every expert runs on peers, and a readahead past a dense block
         * would pull adjacent expert weights the chatter will never execute.
         * Under PARTIAL coverage the mirror must keep reading ahead — the
         * local layers still stream their experts from it. */
        setenv("LUMABRI_REMOTE_EXPERTS", "1", 1);
        fprintf(stderr, "[lumabri] phase 2 active: every expert runs on a peer, "
                        "%d direct peer(s), %d experts, hidden=%d, %s%s\n",
                direct, L.expected, L.hidden,
                L.spread ? "spreading across near replicas" : "nearest replica preferred",
                L.verify_pct ? " · spot-check verification on" : "");
    } else
        fprintf(stderr, "[lumabri] phase 2 partial: %d of %d routed layers run "
                        "on peers (%d peer(s)); the other %d run locally from "
                        "the mirror%s\n",
                L.ok_layers, L.routed_layers, direct,
                L.routed_layers - L.ok_layers,
                L.verify_pct ? " · spot-check verification on" : "");
}

/* Keep the chatter's distance map alive without turning inference into a
 * synchronous all-peer probe. At most one healthy peer is measured per slow
 * cadence, choosing the oldest previous attempt. */
static void lumi_maybe_refresh_rtt(double now) {
    if (now < L.next_rtt_refresh) return;
    L.next_rtt_refresh = now + LUMI_RTT_REFRESH_S;
    int oldest = -1;
    for (int i = 0; i < L.npeers; i++) {
        LumiPeer *p = &L.peers[i];
        if (p->dead || p->nsocks <= 0 || p->rtt_us == LONG_MAX) continue;
        if (oldest < 0 || p->rtt_probe_at < L.peers[oldest].rtt_probe_at)
            oldest = i;
    }
    if (oldest >= 0) lumi_refresh_rtt(&L.peers[oldest]);
}

/* The same shape for the residency report: one peer per cadence, the one
 * read longest ago. A dead peer is skipped; a tunnel peer answers instantly
 * from lumi_read_residency without dialing anything. */
static void lumi_maybe_refresh_residency(double now) {
    if (now < L.next_eres_refresh) return;
    L.next_eres_refresh = now + LUMI_ERES_REFRESH_S;
    int oldest = -1;
    for (int i = 0; i < L.npeers; i++) {
        LumiPeer *p = &L.peers[i];
        if (p->dead || p->relay_target[0]) continue;
        if (oldest < 0 || p->eres_read_at < L.peers[oldest].eres_read_at)
            oldest = i;
    }
    if (oldest >= 0) lumi_read_residency(&L.peers[oldest]);
}

static void lumi_maybe_discover(void) {
    if (!L.initialized) return;
    double now = lumi_now();
    lumi_maybe_refresh_rtt(now);
    lumi_maybe_refresh_residency(now);
    if (!L.discovery || now < L.next_discover) return;
    L.next_discover = now + L.discover_period_s;
    if (!L.have_identity) lumi_refresh_identity();
    int added = lumi_discover();
    lumi_relay_coverage();
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
    if (lmb_secure_init()) return;
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
    double initialized_at = lumi_now();
    L.next_discover = initialized_at + L.discover_period_s;
    /* Segment engines are selected at runtime from one universal archive,
     * whereas the classic chatter binaries bake this value at compile time.
     * Keep one manifest contract for both: segment_node supplies the exact
     * expert engine family through LUMABRI_ENGINE_ID. */
    const char *runtime_engine = getenv("LUMABRI_ENGINE_ID");
    snprintf(L.engine_id, sizeof L.engine_id, "%s",
             runtime_engine && *runtime_engine ? runtime_engine : LMBE_ENGINE_ID);
    snprintf(L.profile, sizeof L.profile,
             "abi=2;engine=%s;src=%s;cc=%s;isa=%s;omp=%s;math=%s;f32=%zu",
             L.engine_id, LMBE_SOURCE_ID, LMB_PROFILE_CC, LMB_PROFILE_ISA,
             LMB_PROFILE_OMP, LMB_PROFILE_MATH, sizeof(float));
    L.expected_bits = lmb_env_int("LUMABRI_EXPERT_BITS", LMBE_EXPECT_BITS,
                                  0, 32);
    const char *model = getenv("LUMABRI_MODEL");
    if (model) snprintf(L.expected_model, sizeof L.expected_model, "%s", model);
    lumi_load_pubkey();
    if (getenv("LUMABRI_PUBKEY") && !L.trust.n) return;
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
    /* Peers built from a different source, math mode, compiler or ISA are
     * ADMITTED by default and spot-check verified: demanding byte-identical
     * toolchains from every stranger on a swarm was the single biggest
     * reason people bounced off. LUMABRI_STRICT=1 restores the old refusal
     * (bit-identity lab mode); LUMABRI_ALLOW_CODEGEN_SKEW stays accepted
     * for compatibility and is now the default. */
    if (getenv("LUMABRI_STRICT") && atoi(getenv("LUMABRI_STRICT")) != 0) {
        L.allow_codegen_skew = 0;
    } else {
        L.allow_codegen_skew = 1;
        /* A skewed peer may round the low bits differently; the spot-check
         * (rerun on another replica, demand agreement) is what turns that
         * from an unchecked risk into a caught one. Default a small rate on
         * unless the operator set LUMABRI_VERIFY themselves. */
        if (!v && L.verify_pct == 0) L.verify_pct = 5;
    }
    /* Spreading is the default. Strict argmin sent every call of every
     * chatter to the single nearest replica — usually the always-on origin
     * server — so a donor with the same experts resident in RAM received
     * exactly zero traffic until that origin failed. The canonical-order
     * band spread keeps cache locality per expert while letting every
     * near-equal replica carry a share. LUMABRI_SPREAD=0 restores argmin. */
    const char *spread_env = getenv("LUMABRI_SPREAD");
    L.spread = spread_env ? atoi(spread_env) != 0 : 1;
    L.hedge_ms = lmb_env_int("LUMABRI_HEDGE_MS", 0, -1, 60000);
    L.exec_wait_ms = lmb_env_int("LUMABRI_EXEC_WAIT_MS", 30000, 100, 3600000);
    L.peer_wait_s = lmb_env_int("LUMABRI_PEER_WAIT_S", LUMI_WAIT_S, 0, 600);
    L.demote_s = lmb_env_int("LUMABRI_DEMOTE_S", 120, 1, 3600);
    L.demote_until = (double *)calloc((size_t)n_layers, sizeof(double));
    L.initialized = 1;

    if (discovery) {
        int found = lumi_discover();
        int relay = lumi_relay_coverage();
        if (!found) {
            fprintf(stderr, relay ? "[lumabri] no directly reachable expert peers; "
                                    "checking tracker relay coverage\n"
                                  : "[lumabri] no expert peers on the swarm — "
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

    /* Bootstrap probes may themselves be slow; begin the maintenance cadence
     * only after every initial peer has been discovered and measured. */
    L.next_rtt_refresh = lumi_now() + LUMI_RTT_REFRESH_S;
    L.next_eres_refresh = lumi_now() + LUMI_ERES_REFRESH_S;

    int missing = lumi_missing_experts();
    if (missing && !discovery) {
        fprintf(stderr, "[lumabri] %d of %d experts have no peer — refusing to "
                "run (a partial explicit network would silently change the "
                "model)\n", missing, L.expected);
        exit(1);
    }
    if (missing && !L.ok_layers)
        fprintf(stderr, "[lumabri] %d of %d experts have no peer and no layer "
                "is fully covered — running experts locally (discovery "
                "continues)\n", missing, L.expected);
    lumi_enable_if_complete();
}

/* olmoe's shape: every layer routes */
static LMB_MAYBE_UNUSED void lumi_init(int n_layers, int n_experts, int hidden) {
    lumi_init_ex(n_layers, n_experts, hidden, NULL);
}

/* Does this (slot, expert) pair belong on the swarm? The engines call it
 * before handing a layer over, so an unrouted slot is never a wire error. */
static int lumi_layer_covered(int layer) {
    if (!L.on || L.swarm_disabled || layer < 0 || layer >= L.n_layers) return 0;
    if (L.demote_until && L.demote_until[layer] > lumi_now()) return 0;
    return !L.layer_ok || L.layer_ok[layer];
}

static LMB_MAYBE_UNUSED int lumi_layer_on(int layer) {
    lumi_maybe_discover();
    if (layer < 0 || layer >= L.n_layers) return 0;
    if (L.routed && !L.routed[layer]) return 0;
    if (lumi_layer_covered(layer)) return 1;
    /* A swarm-fed chatter (LUMABRI_VROOT) has no local expert to fall back
     * to: "running experts locally" means downloading 12 MB per expert per
     * call over the WAN. If the executors are not visible yet — the origin
     * is restarting, the tracker blinked, discovery raced the boot — the
     * only right thing is to wait for them, saying so, and to stop plainly
     * if they never come. Same patience as a vanished replica mid-run. */
    if (!getenv("LUMABRI_VROOT") || L.swarm_disabled) return 0;
    int patience = lmb_env_int("LUMABRI_SWARM_PATIENCE_S", 600, 2, 86400);
    for (int waited = 0; waited < patience; waited += 2) {
        if (waited % 30 == 0)
            fprintf(stderr, "[lumabri] layer %d: nessun esecutore per i suoi "
                            "esperti — aspetto lo sciame (%d/%d s), non li "
                            "eseguo in locale\n", layer, waited, patience);
        sleep(2);
        if (L.discovery) { lumi_discover(); lumi_relay_coverage(); }
        lumi_enable_if_complete();
        if (lumi_layer_covered(layer)) return 1;
    }
    char why[160];
    snprintf(why, sizeof why, "layer %d: nessun esecutore per %d s; non scarico "
             "gli esperti in locale, la risposta si ferma qui", layer, patience);
    lumi_die(why);
    return 0;
}

/* nearest live replica of (layer,eid) not yet tried this call, or -1 */
static uint32_t lumi_hash_gid(int gid) {   /* fnv1a over the (layer,expert) id */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) { h ^= (uint32_t)((unsigned)gid >> (i * 8)) & 0xffu; h *= 16777619u; }
    return h;
}

/* One layer round is over: the token paid max(...) over its experts, so
 * the worst round is what the network really costs, not the mean. */
static void lumi_round_done(double t0) {
    double dt = lumi_now() - t0;
    L.wait_s += dt;
    if (dt > L.wait_max_s) L.wait_max_s = dt;
    L.layers_done++;
}

/* Which replica of `gid` to send to. Default: the nearest live, untried one
 * (strict argmin — deliberate, and what a single-holder-per-expert swarm wants).
 * With LUMABRI_SPREAD on and this expert held by several near-equal replicas,
 * spread instead: keep the ones within 25% + 2 ms of the best, order them
 * canonically by address so every chatter agrees, and pick by a stable
 * per-expert hash — so different experts land on different replicas (load
 * spreads) while one expert keeps a home (cache locality). Collapses to argmin
 * whenever the band has one member, nothing is near-equal, or the best is still
 * unprobed (LONG_MAX), which also keeps the percentage arithmetic from
 * overflowing. Never returns a dead, tried, or absent replica. */
/* A replica that streams its experts from disk costs a disk read per call
 * that no RTT estimate sees until it has happened, and it is usually the
 * origin: the one machine with the lowest RTT and the least spare RAM. Rank
 * it behind a RAM replica at similar distance — the penalty is one cold
 * NVMe read of an expert, not a sentence: a RAM donor reached through a
 * hanging tunnel must still lose to a healthy disk replica 25 ms away. The
 * offset is in microseconds of score, so an unusable RAM replica (dead,
 * circuit open) always loses to a live disk one. */
/* Where an executor keeps its experts is worth about an order of magnitude
 * per call: measured, one to two milliseconds from VRAM, ten to fifteen
 * from RAM, forty to fifty from a cold NVMe read. The RTT probe is a PING
 * and cannot see any of it, so the tier enters the score as a RELATIVE
 * offset — VRAM is the zero, RAM and disk are what they cost more.
 *
 * Always applied, never faded. A prior that depends on how many calls a
 * peer has answered penalises exactly the replica nobody has tried yet: in
 * CI a freshly refreshed peer took sixteen probes and zero calls while the
 * busy one kept every one of them. Constant, the offset cancels between two
 * replicas of the same tier — so their measurements decide, as before — and
 * survives only where the tiers differ, which is the whole point. */
#define LUMI_TIER_VRAM_US     0u
#define LUMI_TIER_RAM_US  10000u
#define LUMI_TIER_DISK_US 48000u

static uint64_t lumi_tier_offset(int residency) {
    switch (residency) {
    case 2:  return LUMI_TIER_VRAM_US;
    case 1:  return LUMI_TIER_RAM_US;
    case 0:  return LUMI_TIER_DISK_US;
    default: return LUMI_TIER_RAM_US;         /* unknown: assume the middle */
    }
}

/* A node with a RAM cache in front of on-disk experts pays the RAM price on
 * a hit and the disk price on a miss, so its offset is the two blended by
 * the rate it reports. A caching node that genuinely never misses IS a RAM
 * node for our purposes, which is the point of asking.
 *
 * A node that reported no counts is not blended at all. Its silence used to
 * be read as "hits every time", which handed an older disk executor the RAM
 * price — the opposite of the flag it had just sent, and a quiet route to
 * the slowest replica in the swarm. No counts, no blend: the flag alone,
 * exactly as it scored before any of this. */
static uint64_t lumi_blend_offset(const LumiPeer *p) {
    if (!p->held_experts) return lumi_tier_offset(p->resident);
    /* the cache in front of the disk is RAM, whatever the flag says overall */
    uint64_t warm = lumi_tier_offset(p->resident == 0 ? 1 : p->resident);
    uint64_t cold = p->resident == 0 ? LUMI_TIER_DISK_US : warm;
    uint64_t hot = p->hot_permille > 1000 ? 1000 : p->hot_permille;
    return (warm * hot + cold * (1000 - hot)) / 1000;
}

static uint64_t lumi_replica_score(const LumiPeer *p) {
    uint64_t score = lmb_predict_score(&p->latency, p->inflight);
    uint64_t tier = lumi_blend_offset(p);
    if (score == UINT64_MAX || !tier) return score;
    return score < UINT64_MAX - tier ? score + tier : score;
}

static int lumi_pick(int gid, uint32_t tried) {
    const int *own = &L.own[(size_t)gid * LUMI_MAX_REP];
    int best = -1;
    uint64_t best_score = UINT64_MAX;
    uint64_t now = lumi_now_ms();
    for (int r = 0; r < LUMI_MAX_REP; r++) {
        int pi = own[r];
        if (pi < 0 || ((tried >> r) & 1) || L.peers[pi].dead ||
            !lmb_predict_available(&L.peers[pi].latency, now)) continue;
        uint64_t score = lumi_replica_score(&L.peers[pi]);
        if (best < 0 || score < best_score)
            { best = r; best_score = score; }
    }
    if (best < 0 || !L.spread || best_score == UINT64_MAX) return best;

    uint64_t band = best_score + best_score / 4 + 2000;
    int cand[LUMI_MAX_REP], nc = 0;
    for (int r = 0; r < LUMI_MAX_REP; r++) {
        int pi = own[r];
        if (pi < 0 || ((tried >> r) & 1) || L.peers[pi].dead ||
            !lmb_predict_available(&L.peers[pi].latency, now)) continue;
        uint64_t score = lumi_replica_score(&L.peers[pi]);
        if (score <= band) cand[nc++] = r;
    }
    if (nc <= 1) return best;
    for (int i = 1; i < nc; i++)            /* canonical: sort the band by address */
        for (int j = i; j > 0 &&
             strcmp(L.peers[own[cand[j]]].addr, L.peers[own[cand[j-1]]].addr) < 0; j--) {
            int t = cand[j]; cand[j] = cand[j-1]; cand[j-1] = t;
        }
    return cand[lumi_hash_gid(gid) % (uint32_t)nc];
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
static int lumi_send_exec(LumiPeer *p, int fd, int layer, int eid,
                          const float *x, int D, int nr, const float *w) {
    LmbBuf b = {0};
    int targeted = p && p->relay_target[0];
    /* EXEC2 to a node that speaks it, directly: the activation goes as
     * bf16 when every value already is one (DeepSeek rounds its MoE input
     * to bf16, so always there; other engines when it happens), which is
     * half the bytes on the uplink that sets a home chatter's tok/s, with
     * the arithmetic untouched. The tunnel keeps the plain dialect. */
    int exec2 = p && !targeted && (p->caps & LMB_CAP_EXEC2);
    size_t n = (size_t)nr * D;
    int bf16 = exec2 && lmb_bf16_exact(x, n);
    if (targeted) lmb_buf_str(&b, p->relay_target);
    lmb_buf_u32(&b, (uint32_t)layer);
    lmb_buf_u32(&b, (uint32_t)eid);
    lmb_buf_u32(&b, (uint32_t)D);
    lmb_buf_u32(&b, (uint32_t)nr);
    if (exec2) lmb_buf_u32(&b, bf16 ? LMB_ENC_BF16 : LMB_ENC_F32);
    if (w) lmb_buf_bytes(&b, w, (size_t)nr * sizeof(float));
    const void *pay = x;
    uint32_t pay_len = (uint32_t)(n * sizeof(float));
    uint16_t *packed = NULL;
    if (bf16) {
        packed = (uint16_t *)malloc(n * sizeof *packed);
        if (packed) { lmb_bf16_pack(packed, x, n); pay = packed; pay_len = (uint32_t)(n * 2); }
        else { bf16 = 0; b.p[b.len - (w ? nr * 4 : 0) - 4] = LMB_ENC_F32; }
    }
    int rc = lmb_send(fd, targeted ? LMB_TEXEC : exec2 ? LMB_EXEC2 : LMB_EXEC,
                      b.p, (uint32_t)b.len, pay, pay_len);
    if (!rc && p) p->bytes_out += 16 + b.len + pay_len;
    free(packed);
    free(b.p);
    return rc;
}

/* The reply of EXEC or EXEC2 as the floats the engine expects, or NULL
 * when it is not one. A bf16 reply is widened here; it was exact. */
static float *lumi_exec_take(LmbMsg *m, int nr, int D) {
    size_t n = (size_t)nr * D;
    if (m->op == LMB_EXEC_R) {
        if (m->pay_len != n * sizeof(float)) return NULL;
        return (float *)lmb_msg_take_pay(m);
    }
    if (m->op != LMB_EXEC2_R || m->body_len < 4) return NULL;
    uint32_t enc = lmb_get32(m->body);
    if (enc == LMB_ENC_F32 && m->pay_len == n * sizeof(float))
        return (float *)lmb_msg_take_pay(m);
    if (enc == LMB_ENC_BF16 && m->pay_len == n * 2) {
        float *res = (float *)malloc(n * sizeof(float));
        if (!res) return NULL;
        lmb_bf16_unpack(res, (const uint16_t *)m->pay, n);
        return res;
    }
    return NULL;
}

/* Adaptive p95 hedging, with LUMABRI_HEDGE_MS as an explicit fixed override.
 * The losing socket is closed so its late frame can never be mistaken for a
 * future request. */
static float *lumi_finish_exec(int layer, int eid, const float *x, int D, int nr,
                               const float *w, int primary_fd, LumiPeer *primary,
                               double primary_started, uint32_t *tried,
                               LumiPeer **from) {
    if (from) *from = NULL;
    if (primary_fd < 0 || !primary) return NULL;
    int fd[2] = { primary_fd, -1 };
    LumiPeer *p[2] = { primary, NULL };
    double started[2] = { primary_started, 0.0 };
    uint32_t hedge_ms = L.hedge_ms < 0 ? 0u :
                        L.hedge_ms > 0 ? (uint32_t)L.hedge_ms :
                        lmb_predict_hedge_ms(&primary->latency);
    if (hedge_ms > 0) {
        struct pollfd one = { fd[0], POLLIN, 0 };
        int pr;
        do pr = poll(&one, 1, (int)hedge_ms); while (pr < 0 && errno == EINTR);
        if (pr == 0) {
            int gid = layer * L.n_experts + eid;
            int r = lumi_pick(gid, *tried);
            if (r >= 0) {
                *tried |= 1u << r;
                p[1] = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
                fd[1] = lumi_take_sock(p[1]);
                started[1] = lumi_now();
                if (fd[1] >= 0 && lumi_send_exec(p[1], fd[1], layer, eid, x, D, nr, w)) {
                    close(fd[1]); lumi_peer_failed(p[1]); fd[1] = -1;
                }
                if (fd[1] >= 0) { lumi_peer_sent(p[1]); L.hedges++; }
            }
        }
    }
    int alive = 1 + (fd[1] >= 0);
    while (alive) {
        struct pollfd pf[2]; int map[2], np = 0;
        for (int i = 0; i < 2; i++) if (fd[i] >= 0) {
            pf[np].fd = fd[i]; pf[np].events = POLLIN; pf[np].revents = 0;
            map[np++] = i;
        }
        int pr;
        /* Zero means "unset": a caller that fills the table by hand instead of
         * calling lumi_init_ex keeps the previous behaviour rather than
         * silently turning this poll into a non-blocking one. */
        int wait_ms = L.exec_wait_ms > 0 ? L.exec_wait_ms
                                         : LMB_DEFAULT_IO_TIMEOUT_MS;
        do pr = poll(pf, np, wait_ms); while (pr < 0 && errno == EINTR);
        if (pr <= 0) break;   /* timeout falls through to peer_failed + failover */
        for (int q = 0; q < np; q++) if (pf[q].revents) {
            int i = map[q];
            LmbMsg m = {0};
            float *res = lmb_recv(fd[i], &m) == 0 ? lumi_exec_take(&m, nr, D) : NULL;
            if (res) {
                uint32_t got = m.pay_len; lmb_msg_free(&m);
                lumi_peer_done(p[i]);
                lumi_put_sock(p[i], fd[i]); fd[i] = -1;
                if (i == 1) L.hedge_wins++;
                p[i]->bytes_in += 16 + got;
                lumi_peer_observed(p[i], started[i]);
                for (int j = 0; j < 2; j++) if (fd[j] >= 0) {
                    close(fd[j]); lumi_peer_done(p[j]);
                }
                if (from) *from = p[i];
                return res;
            }
            lmb_msg_free(&m);
            close(fd[i]); fd[i] = -1; lumi_peer_done(p[i]);
            lumi_peer_failed(p[i]); alive--;
        }
    }
    for (int i = 0; i < 2; i++) if (fd[i] >= 0) {
        close(fd[i]); lumi_peer_done(p[i]); lumi_peer_failed(p[i]);
    }
    return NULL;
}

static float *lumi_exec_relay(int layer, int eid, const float *x, int D, int nr,
                              const float *w) {
    const char *tracker = getenv("LUMABRI_TRACKER");
    size_t gid = (size_t)layer * L.n_experts + eid;
    if (!tracker || !*tracker || !L.discovery || !L.relay || !L.relay[gid]) return NULL;
    LmbBuf b = {0};
    lmb_buf_str(&b, L.expected_model);
    lmb_buf_u32(&b, (uint32_t)L.n_experts);
    lmb_buf_u32(&b, (uint32_t)layer);
    lmb_buf_u32(&b, (uint32_t)eid);
    lmb_buf_u32(&b, (uint32_t)D);
    lmb_buf_u32(&b, (uint32_t)nr);
    if (w) lmb_buf_bytes(&b, w, (size_t)nr * sizeof(float));
    LmbMsg m = {0};
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[lumabri] relay request layer=%d expert=%d rows=%d\n",
                layer, eid, nr);
    int rc = lmb_request_pay(tracker, LMB_REXEC, b.p, (uint32_t)b.len,
                             x, (uint32_t)((size_t)nr * D * sizeof(float)), &m);
    free(b.p);
    uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
    float *out = NULL;
    if (!rc && m.op == LMB_REXEC_R && m.pay_len == want) {
        out = (float *)lmb_msg_take_pay(&m); L.relays++;
    } else if (!rc && m.op == LMB_ERR) {
        char why[160] = "relay refused the call";
        LmbCur c = { m.body, m.body_len, 0 };
        lmb_cur_str(&c, why, sizeof why);
        fprintf(stderr, "[lumabri] tracker EXEC relay unavailable for layer %d "
                        "expert %d: %s\n", layer, eid, why);
    }
    lmb_msg_free(&m);
    return out;
}

/* Every replica of this layer's expert is gone and the relay cannot serve
 * it. Retire the layer to the engine's own local path for demote_s seconds
 * and mark the swarm sick, so the next failing layer skips the patience
 * wait instead of stalling the generation all over again. */
static void lumi_demote_layer(int layer, int waited) {
    double now = lumi_now();
    if (L.demote_until && layer >= 0 && layer < L.n_layers)
        L.demote_until[layer] = now + (double)(L.demote_s > 0 ? L.demote_s : 120);
    L.swarm_sick_until = now + (double)(L.demote_s > 0 ? L.demote_s : 120);
    L.demotions++;
    fprintf(stderr, "[lumabri] layer %d: nessuna replica viva dopo %d s — "
                    "eseguo questo layer in locale dal mirror per %d s, poi "
                    "ritento lo sciame\n",
            layer, waited, L.demote_s > 0 ? L.demote_s : 120);
}

/* the failover path: run one expert synchronously on the next replicas.
 * Costs a full extra round trip — it is the price of a peer dying, paid
 * once, instead of the generation dying with it.
 *
 * `tried` is in/out and `from` reports which replica actually answered, so
 * the caller can spot-check this answer exactly as it does a fast-path one:
 * a failover answer is still an answer, and an unverified answer is the
 * thing this client exists not to emit. `from` stays NULL for a relayed
 * result, which has no replica to attribute it to. */
static float *lumi_exec_retry(int layer, int eid, const float *x, int D, int nr,
                              const float *w, uint32_t *tried_io, LumiPeer **from) {
    int gid = layer * L.n_experts + eid;
    uint32_t tried = tried_io ? *tried_io : 0;
    int waited = 0;
    if (from) *from = NULL;
    for (;;) {
        int r = lumi_pick(gid, tried);
        if (r < 0) {
            float *relayed = lumi_exec_relay(layer, eid, x, D, nr, w);
            if (relayed) { if (tried_io) *tried_io = tried; return relayed; }
            /* A Segment executor owns a complete local kernel for this
             * layer. Its remote Expert provider is an acceleration, not a
             * correctness dependency: hand control back immediately so the
             * patched call site can discard partial remote output and run
             * the unchanged local path. Ordinary expert-only chat keeps the
             * historical wait/fail-closed policy. */
            if (getenv("LUMABRI_EXEC_FALLBACK_LOCAL")) {
                if (tried_io) *tried_io = tried;
                return NULL;
            }
            /* Every known replica is gone. Giving up here is correct in the
             * sense that inventing a result is never allowed — but it threw
             * away a whole generation for a peer that was rebooting, or a
             * network blip, or an ssh session that took the server with it.
             * A swarm with one holder per expert has no redundancy to fall
             * back on, so the only honest thing left is patience: ask the
             * tracker again, and wait, saying so. */
            /* peer_wait_s == 0 means "never read": a harness that fills L
             * by hand (initialized or not) must keep the environment-driven
             * default rather than silently losing all patience. An explicit
             * LUMABRI_PEER_WAIT_S=0 still yields zero via the re-read. */
            int wait_limit = L.peer_wait_s > 0 ? L.peer_wait_s
                : lmb_env_int("LUMABRI_PEER_WAIT_S", LUMI_WAIT_S, 0, 600);
            /* A swarm-fed chatter (LUMABRI_VROOT: the model is a mirror, not
             * a local copy) must never "demote to local": on a 167 GB model
             * the local path is a 12 MB expert download per call over the
             * WAN, and a network blip turned into a dead reply after that
             * download stalled too. Its only honest options are patience
             * (LUMABRI_SWARM_PATIENCE_S, ten minutes by default) and then a
             * plain failure that says so. */
            int swarm_fed = getenv("LUMABRI_VROOT") != NULL;
            if (swarm_fed)
                wait_limit = lmb_env_int("LUMABRI_SWARM_PATIENCE_S", 600, 2, 86400);
            else if (lumi_now() < L.swarm_sick_until) wait_limit = 0;
            if (waited < wait_limit) {
                if (!waited || (swarm_fed && waited % 30 == 0))
                    fprintf(stderr, "[lumabri] layer %d expert %d: nessuna replica "
                            "viva — aspetto che torni (%d/%d s)\n",
                            layer, eid, waited, wait_limit);
                tried = 0;                    /* a returning peer deserves a retry */
                sleep(2);
                waited += 2;
                if (L.discovery) { lumi_discover(); lumi_relay_coverage(); }
                else for (int i = 0; i < L.npeers; i++)
                    if (L.peers[i].dead) {
                        char addr[64];
                        snprintf(addr, sizeof addr, "%s", L.peers[i].addr);
                        lumi_add_peer(addr);   /* revalidates manifest before reuse */
                    }
                continue;
            }
            if (swarm_fed) {
                char why[200];
                snprintf(why, sizeof why, "layer %d expert %d: nessun esecutore "
                         "raggiungibile per %d s; non scarico gli esperti in "
                         "locale, la risposta si ferma qui", layer, eid, waited);
                lumi_die(why);
            }
            lumi_demote_layer(layer, waited);
            if (tried_io) *tried_io = tried;
            return NULL;
        }
        tried |= 1u << r;
        LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r]];
        int fd = lumi_take_sock(p);
        if (fd < 0) continue;
        double started = lumi_now();
        LmbMsg m = {0};
        int send_bad = lumi_send_exec(p, fd, layer, eid, x, D, nr, w);
        if (!send_bad) lumi_peer_sent(p);
        float *res = (!send_bad && lmb_recv(fd, &m) == 0) ? lumi_exec_take(&m, nr, D) : NULL;
        if (!res) {
            close(fd);
            if (!send_bad) lumi_peer_done(p);
            lumi_peer_failed(p);
            lmb_msg_free(&m);
            fprintf(stderr, "[lumabri] peer %s failed — trying next replica\n", p->addr);
            continue;
        }
        p->bytes_in += 16 + m.pay_len;
        lmb_msg_free(&m);
        lumi_peer_done(p);
        lumi_put_sock(p, fd);
        lumi_peer_observed(p, started);
        L.failovers++;
        if (tried_io) *tried_io = tried;
        if (from) *from = p;
        return res;
    }
}

/* Spot-check: rerun this expert on a DIFFERENT replica and demand the same
 * bytes. Determinism makes lying detectable: two honest peers cannot
 * disagree, so a disagreement IS an attack (or broken hardware) — either
 * way the answer cannot be trusted, and the run stops loudly rather than
 * emit a token nobody can vouch for. */
static int lumi_spot_check(int layer, int eid, const float *x, int D, int nr,
                           const float *w, const float *got, LumiPeer *from,
                           uint32_t tried) {
    int gid = layer * L.n_experts + eid;
    int r2 = lumi_pick(gid, tried);
    if (r2 < 0) return 0;                     /* no second replica to ask */
    LumiPeer *p = &L.peers[L.own[(size_t)gid * LUMI_MAX_REP + r2]];
    int fd = lumi_take_sock(p);
    if (fd < 0) return 0;
    uint32_t want = (uint32_t)((size_t)nr * D * sizeof(float));
    LmbMsg m = {0};
    if (lumi_send_exec(p, fd, layer, eid, x, D, nr, w) || lmb_recv(fd, &m) ||
        m.op != LMB_EXEC_R || m.pay_len != want) {
        close(fd);
        lmb_msg_free(&m);
        return 0;                             /* checker down ≠ answer wrong */
    }
    L.verified++;
    int same = memcmp(got, m.pay, (size_t)want) == 0;
    lmb_msg_free(&m);
    lumi_put_sock(p, fd);
    if (!same) {
        /* Two replicas disagreed and there is no majority: neither can be
         * trusted. Killing the engine punished the victim; quarantine both
         * suspects instead, discard the answer, and let the caller recompute
         * on an untainted replica — or on the local mirror, whose bytes are
         * hash-verified and cannot lie. */
        fprintf(stderr, "[lumabri] INTEGRITY FAILURE on layer %d expert %d: "
                "%s and %s returned different bytes for the same activation. "
                "One of them is lying or broken; quarantining both and "
                "recomputing elsewhere.\n",
                layer, eid, from ? from->addr : "the tracker relay", p->addr);
        double until = lumi_now() + 600.0;
        p->dead = 1; p->retry_at = until;
        if (from) { from->dead = 1; from->retry_at = until; }
        L.integrity_fails++;
        return -1;
    }
    return 0;
}

/* The sampling gate in front of it. Every answer this client is willing to
 * use goes through here — fast path, failover and relay alike — so the
 * percentage an operator asks for is the percentage of USED answers that get
 * checked, not the percentage of one favoured path. A relayed answer has no
 * replica of its own (`from` is NULL); it is still worth offering to the
 * check, which compares it against any replica that is reachable and is a
 * no-op when none is. */
static unsigned lumi_vseed = 0x9e3779b9u;
static int lumi_maybe_spot_check(int layer, int eid, const float *x, int D,
                                 int nr, const float *w, const float *got,
                                 LumiPeer *from, uint32_t tried) {
    if (!L.verify_pct || !got) return 0;
    lumi_vseed = lumi_vseed * 1664525u + 1013904223u;
    if ((int)(lumi_vseed % 100u) < L.verify_pct)
        return lumi_spot_check(layer, eid, x, D, nr, w, got, from, tried);
    return 0;
}

/* Run the K selected experts of one layer on their peers and accumulate into
 * `out` with the router weights. A peer failure costs a retry on the next
 * replica; only a replica-exhausted expert is fatal. */
static LMB_MAYBE_UNUSED int lumi_moe_apply(int layer, const int *idx,
                           const float *val, int K,
                           const float *x, int D, float *out) {
    if (K > LUMI_MAX_K) lumi_die("top-k larger than the client supports");
    int fds[LUMI_MAX_K];
    uint32_t tried[LUMI_MAX_K];
    LumiPeer *ps[LUMI_MAX_K];
    float *res[LUMI_MAX_K];
    double sent[LUMI_MAX_K];
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
            sent[k] = lumi_now();
            if (lumi_send_exec(p, fd, layer, idx[k], x, D, 1, NULL)) {
                close(fd);
                lumi_peer_failed(p);
                continue;
            }
            lumi_peer_sent(p);
            fds[k] = fd; ps[k] = p;
            break;
        }
    }
    /* then collect, in order; a failed reply falls over to the next replica */
    for (int k = 0; k < K; k++) {
        if (fds[k] >= 0) {
            LumiPeer *winner = NULL;
            res[k] = lumi_finish_exec(layer, idx[k], x, D, 1, NULL,
                                      fds[k], ps[k], sent[k], &tried[k], &winner);
            if (res[k] &&
                lumi_maybe_spot_check(layer, idx[k], x, D, 1, NULL, res[k],
                                      winner, tried[k])) {
                free(res[k]); res[k] = NULL;  /* mismatch: recompute elsewhere */
            } else if (!res[k])
                fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                "trying next replica\n", ps[k]->addr, layer, idx[k]);
            if (res[k]) continue;
        }
        LumiPeer *rf = NULL;
        res[k] = lumi_exec_retry(layer, idx[k], x, D, 1, NULL, &tried[k], &rf);
        if (res[k] &&
            lumi_maybe_spot_check(layer, idx[k], x, D, 1, NULL, res[k], rf,
                                  tried[k])) {
            free(res[k]); res[k] = NULL;  /* second disagreement: go local */
        }
        if (!res[k]) {
            for (int j = k + 1; j < K; j++) if (fds[j] >= 0) {
                close(fds[j]); lumi_peer_done(ps[j]);
            }
            for (int j = 0; j < K; j++) free(res[j]);
            return 0;
        }
    }
    /* accumulate in the router's order, exactly as the local path does */
    for (int k = 0; k < K; k++) {
        float w = val[k];
        const float *h = res[k];
        for (int d = 0; d < D; d++) out[d] += w * h[d];
        free(res[k]);
    }
    L.calls += (unsigned long long)K;
    lumi_round_done(t0);
    return 1;
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

static LMB_MAYBE_UNUSED int lumi_moe_apply_batch(int layer, const int *idxs,
                                 const float *ws,
                                 const int *keff, int K, const float *x,
                                 int S, int D, float *out) {
    unsigned char *seen = (unsigned char *)calloc((size_t)L.n_experts, 1);
    int *uniq = (int *)malloc((size_t)L.n_experts * sizeof(int));
    int *rows = (int *)malloc((size_t)S * LUMI_BLOCK * sizeof(int));
    float *rw  = (float *)malloc((size_t)S * LUMI_BLOCK * sizeof(float));
    float *xg  = (float *)malloc((size_t)S * LUMI_BLOCK * (size_t)D * sizeof(float));
    float *saved = (float *)malloc((size_t)S * (size_t)D * sizeof(float));
    if (!seen || !uniq || !rows || !rw || !xg || !saved)
        lumi_die("out of memory batching a layer");
    memcpy(saved, out, (size_t)S * (size_t)D * sizeof(float));
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
    double sent[LUMI_BLOCK];

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
                sent[j] = lumi_now();
                if (lumi_send_exec(p, fd, layer, eid, xj, D, nr, NULL)) {
                    close(fd); lumi_peer_failed(p); continue;
                }
                lumi_peer_sent(p);
                fds[j] = fd; ps[j] = p;
                break;
            }
        }
        for (int j = 0; j < nb; j++) {
            int eid = uniq[base + j], nr = nrs[j];
            float *res = NULL, *xj = xg + (size_t)j * S * D;
            if (fds[j] >= 0) {
                LumiPeer *winner = NULL;
                res = lumi_finish_exec(layer, eid, xj, D, nr, NULL,
                                       fds[j], ps[j], sent[j], &tried[j], &winner);
                fds[j] = -1;
                if (res &&
                    lumi_maybe_spot_check(layer, eid, xj, D, nr, NULL, res,
                                          winner, tried[j])) {
                    free(res); res = NULL;  /* mismatch: recompute elsewhere */
                } else if (!res) {
                    fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                    "trying next replica\n", ps[j]->addr, layer, eid);
                }
            }
            if (!res) {
                LumiPeer *rf = NULL;
                res = lumi_exec_retry(layer, eid, xj, D, nr, NULL, &tried[j], &rf);
                if (res &&
                    lumi_maybe_spot_check(layer, eid, xj, D, nr, NULL, res, rf,
                                          tried[j])) {
                    free(res); res = NULL;  /* second disagreement: go local */
                }
                if (!res) {
                    for (int q = j + 1; q < nb; q++)
                        if (fds[q] >= 0) {
                            close(fds[q]); lumi_peer_done(ps[q]);
                        }
                    memcpy(out, saved, (size_t)S * (size_t)D * sizeof(float));
                    free(seen); free(uniq); free(rows); free(rw); free(xg); free(saved);
                    return 0;
                }
            }
            if (nr > 1) { L.batch_calls++; L.batch_rows += (unsigned long long)nr; }
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
    lumi_round_done(t0);
    free(seen); free(uniq); free(rows); free(rw); free(xg); free(saved);
    return 1;
}

/* The same batching, for an engine that has already built the union itself.
 *
 * kimi_k3 arrives here with the work already grouped: nu distinct experts,
 * and for each one the list of positions that routed to it with their
 * weights. It also runs its experts in the LATENT space, so `D` here is
 * c->latent, not hidden — the peer must agree, which is what the manifest's
 * dimension check enforces. */
static LMB_MAYBE_UNUSED int lumi_moe_apply_union(int layer, int nu,
                                 const int *uid,
                                 const int *pfirst, const int *pcnt,
                                 const int *poslist, const float *wlist,
                                 const float *Z, int D, float *U) {
    if (nu <= 0) return 1;
    int maxrows = 0;
    int positions = 0;
    for (int j = 0; j < nu; j++) if (pcnt[j] > maxrows) maxrows = pcnt[j];
    for (int j = 0; j < nu; j++)
        for (int r = 0; r < pcnt[j]; r++)
            if (poslist[pfirst[j] + r] + 1 > positions)
                positions = poslist[pfirst[j] + r] + 1;
    float *xg = (float *)malloc((size_t)maxrows * LUMI_BLOCK * (size_t)D * sizeof(float));
    float *saved = (float *)malloc((size_t)positions * (size_t)D * sizeof(float));
    if (!xg || !saved) lumi_die("out of memory batching a layer");
    memcpy(saved, U, (size_t)positions * (size_t)D * sizeof(float));
    double t0 = lumi_now();
    int fds[LUMI_BLOCK];
    uint32_t tried[LUMI_BLOCK];
    LumiPeer *ps[LUMI_BLOCK];
    double sent[LUMI_BLOCK];

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
                sent[j] = lumi_now();
                if (lumi_send_exec(p, fd, layer, eid, xj, D, nr, NULL)) {
                    close(fd); lumi_peer_failed(p); continue;
                }
                lumi_peer_sent(p);
                fds[j] = fd; ps[j] = p;
                break;
            }
        }
        for (int j = 0; j < nb; j++) {
            int eid = uid[base + j], nr = pcnt[base + j], f = pfirst[base + j];
            float *res = NULL, *xj = xg + (size_t)j * maxrows * D;
            if (fds[j] >= 0) {
                LumiPeer *winner = NULL;
                res = lumi_finish_exec(layer, eid, xj, D, nr, NULL,
                                       fds[j], ps[j], sent[j], &tried[j], &winner);
                fds[j] = -1;
                if (res &&
                    lumi_maybe_spot_check(layer, eid, xj, D, nr, NULL, res,
                                          winner, tried[j])) {
                    free(res); res = NULL;  /* mismatch: recompute elsewhere */
                } else if (!res) {
                    fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                    "trying next replica\n", ps[j]->addr, layer, eid);
                }
            }
            if (!res) {
                LumiPeer *rf = NULL;
                res = lumi_exec_retry(layer, eid, xj, D, nr, NULL, &tried[j], &rf);
                if (res &&
                    lumi_maybe_spot_check(layer, eid, xj, D, nr, NULL, res, rf,
                                          tried[j])) {
                    free(res); res = NULL;  /* second disagreement: go local */
                }
                if (!res) {
                    for (int q = j + 1; q < nb; q++)
                        if (fds[q] >= 0) {
                            close(fds[q]); lumi_peer_done(ps[q]);
                        }
                    memcpy(U, saved, (size_t)positions * (size_t)D * sizeof(float));
                    free(xg); free(saved);
                    return 0;
                }
            }
            if (nr > 1) { L.batch_calls++; L.batch_rows += (unsigned long long)nr; }
            for (int r = 0; r < nr; r++) {
                float *us = U + (int64_t)poslist[f + r] * D, w = wlist[f + r];
                const float *hr = res + (size_t)r * D;
                for (int d = 0; d < D; d++) us[d] += w * hr[d];
            }
            free(res);
            L.calls++;
        }
    }
    lumi_round_done(t0);
    free(xg); free(saved);
    return 1;
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
static LMB_MAYBE_UNUSED int lumi_moe_apply_v4(int layer, const int *indices,
                              const float *weights,
                              int topk, const float *x, int batch, int D,
                              float *out) {
    unsigned char *used = (unsigned char *)calloc((size_t)L.n_experts, 1);
    int *uid = (int *)malloc((size_t)L.n_experts * sizeof(int));
    int *rows = (int *)malloc((size_t)batch * LUMI_BLOCK * sizeof(int));
    float *rw = (float *)malloc((size_t)batch * LUMI_BLOCK * sizeof(float));
    float *xg = (float *)malloc((size_t)batch * LUMI_BLOCK * (size_t)D * sizeof(float));
    float *saved = (float *)malloc((size_t)batch * (size_t)D * sizeof(float));
    if (!used || !uid || !rows || !rw || !xg || !saved)
        lumi_die("out of memory batching a layer");
    memcpy(saved, out, (size_t)batch * (size_t)D * sizeof(float));
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
    double sent[LUMI_BLOCK];

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
                sent[j] = lumi_now();
                if (lumi_send_exec(p, fd, layer, eid, xj, D, nr, wj)) {
                    close(fd); lumi_peer_failed(p); continue;
                }
                lumi_peer_sent(p);
                fds[j] = fd; ps[j] = p;
                break;
            }
        }
        for (int j = 0; j < nb; j++) {
            int eid = uid[base + j], nr = nrs[j];
            float *res = NULL, *xj = xg + (size_t)j * batch * D;
            float *wj = rw + (size_t)j * batch;
            int *rj = rows + (size_t)j * batch;
            if (fds[j] >= 0) {
                LumiPeer *winner = NULL;
                res = lumi_finish_exec(layer, eid, xj, D, nr, wj,
                                       fds[j], ps[j], sent[j], &tried[j], &winner);
                fds[j] = -1;
                if (res &&
                    lumi_maybe_spot_check(layer, eid, xj, D, nr, wj, res,
                                          winner, tried[j])) {
                    free(res); res = NULL;  /* mismatch: recompute elsewhere */
                } else if (!res) {
                    fprintf(stderr, "[lumabri] peer %s failed on layer %d expert %d — "
                                    "trying next replica\n", ps[j]->addr, layer, eid);
                }
            }
            if (!res) {
                LumiPeer *rf = NULL;
                res = lumi_exec_retry(layer, eid, xj, D, nr, wj, &tried[j], &rf);
                if (res &&
                    lumi_maybe_spot_check(layer, eid, xj, D, nr, wj, res, rf,
                                          tried[j])) {
                    free(res); res = NULL;  /* second disagreement: go local */
                }
                if (!res) {
                    for (int q = j + 1; q < nb; q++)
                        if (fds[q] >= 0) {
                            close(fds[q]); lumi_peer_done(ps[q]);
                        }
                    memcpy(out, saved, (size_t)batch * (size_t)D * sizeof(float));
                    free(used); free(uid); free(rows); free(rw); free(xg); free(saved);
                    return 0;
                }
            }
            if (nr > 1) { L.batch_calls++; L.batch_rows += (unsigned long long)nr; }
            for (int r = 0; r < nr; r++) {       /* already weighted by the peer */
                float *os = out + (size_t)rj[r] * D;
                const float *hr = res + (size_t)r * D;
                for (int d = 0; d < D; d++) os[d] += hr[d];
            }
            free(res);
            L.calls++;
        }
    }
    lumi_round_done(t0);
    free(used); free(uid); free(rows); free(rw); free(xg); free(saved);
    return 1;
}

static LMB_MAYBE_UNUSED void lumi_report(void) {
    if (!L.on && !L.calls) return;
    fprintf(stderr, "[lumabri] %llu remote expert calls in %llu layer rounds · "
                    "%.2fs waiting on peers (%.2f ms per layer round, worst %.1f ms) · "
                    "%llu batched call(s)/%llu rows · %llu hedge(s), %llu won · "
                    "%llu failover(s) · %llu tracker relay call(s) · "
                    "%llu spot-check(s)%s\n",
            L.calls, L.layers_done, L.wait_s,
            L.layers_done ? 1000.0 * L.wait_s / (double)L.layers_done : 0.0,
            1000.0 * L.wait_max_s,
            L.batch_calls, L.batch_rows, L.hedges, L.hedge_wins,
            L.failovers, L.relays, L.verified,
            L.integrity_fails ? "" : ", all agreed");
    if (L.demotions || L.integrity_fails)
        fprintf(stderr, "[lumabri] %llu layer demotion(s) to the local mirror"
                        " · %llu integrity quarantine(s)\n",
                L.demotions, L.integrity_fails);
    /* The bill per executor, and the bytes a layer round costs: the number
     * that decides whether a chatter's uplink, not the peers, sets its tok/s. */
    unsigned long long out = 0, in = 0;
    for (int i = 0; i < L.npeers; i++) {
        LumiPeer *p = &L.peers[i];
        out += p->bytes_out; in += p->bytes_in;
        if (!p->ok_calls && !p->bytes_out) continue;
        fprintf(stderr, "[lumabri] executor %s: %llu call(s) answered · %.1f ms each · "
                        "%.1f MB up · %.1f MB down%s%s\n",
                p->addr, p->ok_calls,
                p->ok_calls ? 1000.0 * p->lat_s / (double)p->ok_calls : 0.0,
                (double)p->bytes_out / 1e6, (double)p->bytes_in / 1e6,
                p->relay_target[0] ? " · via tracker tunnel" : "",
                p->resident == 0 ? " · experts from disk" :
                p->resident == 1 ? " · experts in RAM" : "");
    }
    if (L.layers_done)
        fprintf(stderr, "[lumabri] wire: %.1f MB up · %.1f MB down · per layer round "
                        "%.0f KB up · %.0f KB down\n",
                (double)out / 1e6, (double)in / 1e6,
                (double)out / 1e3 / (double)L.layers_done,
                (double)in / 1e3 / (double)L.layers_done);
}

#endif /* LUMABRI_CLIENT_H */
