#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lumabri_segment_discovery.h"
#include "lumabri_proto.h"
#include "lumabri_sign.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *engine;
    const char *schema;
    const char *numeric;
    uint32_t dtype;
    uint32_t width;
    uint64_t backend;
} TinyFamily;

static const TinyFamily families[] = {
    { "olmoe", "kv-standard-v1", "olmoe-f32-strict-tiny",
      LMB_SEG_DTYPE_F32, 8, LMB_SEG_CAP_CPU },
    { "glm", "mla-dsa-rope-device-v1", "glm-f16-strict-tiny",
      LMB_SEG_DTYPE_F16, 12, LMB_SEG_CAP_CUDA },
    { "inkling", "kv-global-sliding-conv-v1", "inkling-f32-strict-tiny",
      LMB_SEG_DTYPE_F32, 10, LMB_SEG_CAP_METAL },
    { "kimi_k3", "mla-kda-conv-attnres-v1", "kimi-bf16-strict-tiny",
      LMB_SEG_DTYPE_BF16, 14, LMB_SEG_CAP_VULKAN },
    { "qwen36", "kv-deltanet-conv-ring-v1", "qwen-f16-strict-tiny",
      LMB_SEG_DTYPE_F16, 16, LMB_SEG_CAP_CUDA },
    { "deepseek_v4", "mhc-window-compressor-indexer-v1",
      "dsv4-bf16-strict-tiny", LMB_SEG_DTYPE_BF16, 18, LMB_SEG_CAP_HIP },
};

static void fill_root(uint8_t root[LMB_SEG_ROOT_BYTES], unsigned seed) {
    for (size_t i = 0; i < LMB_SEG_ROOT_BYTES; i++)
        root[i] = (uint8_t)(seed + 19u * (unsigned)i + 1u);
}

static LmbSegAdvert make_advert(size_t family, unsigned peer,
                                uint32_t begin, uint32_t end) {
    const TinyFamily *f = &families[family];
    LmbSegAdvert a;
    memset(&a, 0, sizeof a);
    snprintf(a.peer_name, sizeof a.peer_name, "seg-%s-%u", f->engine, peer);
    snprintf(a.addr, sizeof a.addr, "127.0.0.1:%u", 7800u + peer);
    snprintf(a.model, sizeof a.model, "tiny-%s", f->engine);
    fill_root(a.model_root, (unsigned)family + 10u);
    fill_root(a.tokenizer_root, (unsigned)family + 80u);
    a.layer_begin = begin; a.layer_end = end;
    a.max_context = 4096; a.max_rows = 64;
    a.state_dtype = f->dtype; a.state_width = f->width;
    a.max_sessions = 8; a.active_sessions = 1;
    a.capabilities = LMB_SEG_CAP_RANGE_NATIVE | LMB_SEG_CAP_MULTI_SESSION |
                     LMB_SEG_CAP_SNAPSHOT | LMB_SEG_CAP_CPU | f->backend;
    a.resident_ram_bytes = 64u << 20;
    a.resident_vram_bytes = f->backend == LMB_SEG_CAP_CPU ? 0 : 32u << 20;
    snprintf(a.engine_id, sizeof a.engine_id, "%s", f->engine);
    snprintf(a.state_schema, sizeof a.state_schema, "%s", f->schema);
    snprintf(a.numeric_class, sizeof a.numeric_class, "%s", f->numeric);
    return a;
}

static LmbSegQuery query_for(const LmbSegAdvert *a,
                             uint32_t begin, uint32_t end) {
    LmbSegQuery q;
    memset(&q, 0, sizeof q);
    snprintf(q.model, sizeof q.model, "%s", a->model);
    memcpy(q.model_root, a->model_root, sizeof q.model_root);
    memcpy(q.tokenizer_root, a->tokenizer_root, sizeof q.tokenizer_root);
    q.layer_begin = begin; q.layer_end = end;
    q.context_tokens = 2048; q.rows = 8;
    q.state_dtype = a->state_dtype; q.state_width = a->state_width;
    q.required_capabilities = LMB_SEG_CAP_RANGE_NATIVE |
                              LMB_SEG_CAP_MULTI_SESSION;
    snprintf(q.engine_id, sizeof q.engine_id, "%s", a->engine_id);
    snprintf(q.state_schema, sizeof q.state_schema, "%s", a->state_schema);
    snprintf(q.numeric_class, sizeof q.numeric_class, "%s", a->numeric_class);
    return q;
}

static void test_codecs(void) {
    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        LmbSegAdvert a = make_advert(i, (unsigned)i, 0, 4), decoded;
        LmbSegQuery q = query_for(&a, 0, 4), qdecoded;
        uint8_t *body = NULL; uint32_t len = 0;
        assert(lmb_seg_advert_valid(&a));
        assert(!lmb_seg_advert_encode(&a, &body, &len));
        assert(!lmb_seg_advert_decode(body, len, &decoded));
        assert(!memcmp(&a, &decoded, sizeof a));
        assert(lmb_seg_advert_decode(body, len - 1, &decoded));
        free(body); body = NULL;
        assert(!lmb_seg_query_encode(&q, &body, &len));
        assert(!lmb_seg_query_decode(body, len, &qdecoded));
        assert(!memcmp(&q, &qdecoded, sizeof q));
        assert(lmb_seg_advert_compatible(&a, &q));
        free(body);
    }

    LmbSegAdvert a = make_advert(0, 0, 0, 2);
    LmbSegQuery q = query_for(&a, 0, 4);
    LmbSegRouteSnapshot route = { .route_generation = 7, .count = 2 };
    route.entries[0].advert = a;
    route.entries[1].advert = make_advert(0, 1, 2, 4);
    for (uint32_t i = 0; i < route.count; i++) {
        memset(route.entries[i].owner.lease_id.bytes, (int)i + 1,
               sizeof route.entries[i].owner.lease_id.bytes);
        route.entries[i].owner.fencing_epoch = i + 1;
        route.entries[i].owner.route_generation = route.route_generation;
        route.entries[i].transport = LMB_SEG_TRANSPORT_DIRECT;
    }
    route.complete = (uint32_t)lmb_seg_route_complete(&route, &q);
    assert(route.complete);
    uint8_t *body = NULL; uint32_t len = 0;
    LmbSegRouteSnapshot decoded;
    assert(!lmb_seg_route_encode(&route, &body, &len));
    assert(!lmb_seg_route_decode(body, len, &decoded));
    assert(decoded.complete && decoded.count == 2);
    free(body);
    route.entries[1].advert.layer_begin = 3;
    assert(!lmb_seg_route_complete(&route, &q));
}

typedef struct {
    int fd;
    uint8_t pk[32], sk[64];
    LmbSegOwner owner;
} Registration;

static int register_advert(const char *tracker, const LmbSegAdvert *a,
                           unsigned seed_value, Registration *r) {
    if (r->fd <= 0) {
        uint8_t seed[32];
        for (size_t i = 0; i < sizeof seed; i++)
            seed[i] = (uint8_t)(seed_value + (unsigned)i + 1u);
        lmb_sign_keypair(r->pk, r->sk, seed);
        r->fd = lmb_connect(tracker);
        if (r->fd < 0 || lmb_auth(r->fd)) return -1;
    }
    uint8_t nonce[32];
    if (lmb_request_challenge(r->fd, nonce)) return -1;
    uint8_t *wire = NULL; uint32_t wire_len = 0;
    if (lmb_seg_advert_encode(a, &wire, &wire_len)) return -1;
    LmbBuf body = { wire, wire_len, wire_len };
    uint8_t msg[512], sig[64];
    size_t msg_len = lmb_peer_auth_msg(nonce, a->peer_name, a->model, a->addr,
                                       msg, sizeof msg);
    if (!msg_len) { free(body.p); return -1; }
    lmb_sign(sig, msg, msg_len, r->sk);
    if (lmb_buf_peer_auth(&body, r->pk, sig) ||
        lmb_send(r->fd, LMB_SEG_REGISTER, body.p, (uint32_t)body.len, NULL, 0)) {
        free(body.p); return -1;
    }
    free(body.p);
    LmbMsg reply = {0};
    if (lmb_recv(r->fd, &reply) || reply.op != LMB_SEG_REGISTER_R ||
        lmb_seg_registration_reply_decode(reply.body, reply.body_len, &r->owner)) {
        lmb_msg_free(&reply); return -1;
    }
    lmb_msg_free(&reply);
    return 0;
}

static void test_tracker(const char *tracker) {
    enum { NF = (int)(sizeof families / sizeof families[0]), NR = NF + 2 };
    Registration registrations[NR];
    LmbSegAdvert adverts[NF];
    memset(registrations, 0, sizeof registrations);
    for (int i = 0; i < NF; i++) {
        registrations[i].fd = -1;
        adverts[i] = make_advert((size_t)i, (unsigned)i + 1u, 0, 4);
        assert(!register_advert(tracker, &adverts[i], (unsigned)i + 20u,
                                &registrations[i]));
    }

    /* A Segment-only compute peer is not an Expert peer. This preserves the
     * existing chatter path during rolling upgrades. */
    LmbBuf expert_query = {0}; LmbMsg expert_reply = {0};
    assert(!lmb_buf_str(&expert_query, adverts[0].model));
    assert(!lmb_request(tracker, LMB_EPEERS, expert_query.p,
                        (uint32_t)expert_query.len, &expert_reply));
    free(expert_query.p);
    assert(expert_reply.op == LMB_EPEERS_R);
    LmbCur expert_cur = { expert_reply.body, expert_reply.body_len, 0 };
    uint32_t expert_count = 1;
    assert(!lmb_cur_u32(&expert_cur, &expert_count));
    assert(expert_count == 0 && expert_cur.off == expert_cur.len);
    lmb_msg_free(&expert_reply);

    for (int i = 0; i < NF; i++) {
        LmbSegQuery q = query_for(&adverts[i], 0, 4);
        LmbSegRouteSnapshot s;
        assert(!lmb_seg_routes_fetch(tracker, &q, &s));
        assert(s.complete && s.count == 1);
        assert(s.entries[0].owner.route_generation == s.route_generation);
        assert(s.entries[0].transport == LMB_SEG_TRANSPORT_DIRECT);
    }

    /* Telemetry may refresh at heartbeat frequency without fencing sessions
     * or churning placement generations. */
    LmbSegQuery oq = query_for(&adverts[0], 0, 4);
    LmbSegRouteSnapshot os;
    assert(!lmb_seg_routes_fetch(tracker, &oq, &os));
    uint64_t generation_before = os.route_generation;
    LmbSegOwner before = os.entries[0].owner;
    adverts[0].queue_depth = 3;
    adverts[0].inflight = 2;
    adverts[0].resident_ram_bytes += 4096;
    assert(!register_advert(tracker, &adverts[0], 20, &registrations[0]));
    assert(!lmb_seg_routes_fetch(tracker, &oq, &os));
    assert(os.route_generation == generation_before);
    assert(registrations[0].owner.fencing_epoch == before.fencing_epoch);
    assert(lmb_seg_id_equal(&registrations[0].owner.lease_id, &before.lease_id));

    /* Material range changes receive a new lease and generation. Add a
     * second range and one replica: the returned list preserves replicas and
     * its union forms a complete layer-aligned chain. */
    adverts[0].layer_end = 2;
    assert(!register_advert(tracker, &adverts[0], 20, &registrations[0]));
    assert(registrations[0].owner.route_generation > before.route_generation);
    assert(registrations[0].owner.fencing_epoch > before.fencing_epoch);
    assert(!lmb_seg_id_equal(&registrations[0].owner.lease_id, &before.lease_id));
    LmbSegAdvert upper = make_advert(0, 90, 2, 4);
    LmbSegAdvert replica = make_advert(0, 91, 2, 4);
    registrations[NF].fd = registrations[NF + 1].fd = -1;
    assert(!register_advert(tracker, &upper, 90, &registrations[NF]));
    assert(!register_advert(tracker, &replica, 91, &registrations[NF + 1]));
    assert(!lmb_seg_routes_fetch(tracker, &oq, &os));
    assert(os.complete && os.count == 3);

    /* Draining removes a peer from new placements but does not mutate an old
     * snapshot already held by a running session. One replica remains. */
    LmbSegRouteSnapshot old = os;
    upper.flags = LMB_SEG_ADVERT_DRAINING;
    assert(!register_advert(tracker, &upper, 90, &registrations[NF]));
    assert(!lmb_seg_routes_fetch(tracker, &oq, &os));
    assert(os.complete && os.count == 2);
    assert(old.count == 3);

    /* Exact roots, schema and numeric class are compatibility gates. */
    LmbSegQuery incompatible = oq;
    incompatible.model_root[0] ^= 0x80;
    assert(!lmb_seg_routes_fetch(tracker, &incompatible, &os));
    assert(!os.complete && os.count == 0);
    incompatible = oq;
    snprintf(incompatible.numeric_class, sizeof incompatible.numeric_class,
             "different-numeric-class");
    assert(!lmb_seg_routes_fetch(tracker, &incompatible, &os));
    assert(!os.complete && os.count == 0);

    /* The control worker publishes snapshots asynchronously. The inference
     * side only copies the immutable value and never opens a tracker socket. */
    LmbSegDiscovery *discovery = lmb_seg_discovery_start(tracker, &oq, 50);
    assert(discovery);
    int have = 0;
    for (int i = 0; i < 100 && !have; i++) {
        usleep(20000);
        have = lmb_seg_discovery_snapshot(discovery, &os);
    }
    assert(have == 1 && os.complete && os.count == 2);
    lmb_seg_discovery_stop(discovery);

    for (int i = 0; i < NR; i++) if (registrations[i].fd >= 0) close(registrations[i].fd);
}

int main(int argc, char **argv) {
    test_codecs();
    if (argc == 2) test_tracker(argv[1]);
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [TRACKER]\n", argv[0]);
        return 2;
    }
    puts("SEGMENT DISCOVERY: PASS (six families, compatibility, leases, routes, async snapshot)");
    return 0;
}
