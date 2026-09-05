#include "lumabri_home.h"
#include <assert.h>

static LmbHomeOffer fixture(void) {
    LmbHomeOffer o = {0};
    o.id[0] = 1; o.requester[0] = 2; o.edge_peer[0] = 3; o.model_root[0] = 4;
    strcpy(o.model, "tiny-olmoe"); strcpy(o.model_type, "olmoe");
    strcpy(o.tracker, "127.0.0.1:7300");
    o.begin = 0; o.end = 2; o.layers = 4; o.context = 64; o.threads = 1; o.max_new = 16;
    o.ram_bytes = 256u << 20; o.disk_bytes = 128u << 20; o.model_bytes = 64u << 20;
    return o;
}

int main(void) {
    LmbHomeOffer o = fixture(), decoded;
    LmbBuf b = {0}; assert(!lmb_home_offer_pack(&b, &o));
    for (size_t n = 0; n < b.len; n++) {
        LmbCur c = {b.p, n, 0}; assert(lmb_home_offer_unpack(&c, &decoded));
    }
    LmbCur c = {b.p, b.len, 0}; assert(!lmb_home_offer_unpack(&c, &decoded));
    assert(!memcmp(&o, &decoded, sizeof o)); free(b.p);
    LmbHomeTransaction t = {0};
    assert(lmb_home_offer_begin(&t, &o, "other-house:7300", o.ram_bytes, o.disk_bytes, 100));
    assert(lmb_home_offer_begin(&t, &o, o.tracker, o.ram_bytes - 1, o.disk_bytes, 100));
    assert(!lmb_home_offer_begin(&t, &o, o.tracker, o.ram_bytes, o.disk_bytes, 100));
    assert(!t.reservation_held);
    assert(lmb_home_commit(&t, o.id)); /* requesting is not authorizing */
    assert(!lmb_home_decide(&t, 0, o.ram_bytes, o.disk_bytes, 101));
    assert(t.phase == LMB_HOME_REJECTED && !t.reservation_held);
    assert(lmb_home_commit(&t, o.id));
    assert(!lmb_home_offer_begin(&t, &o, o.tracker, o.ram_bytes, o.disk_bytes, 200));
    assert(lmb_home_decide(&t, 1, o.ram_bytes - 1, o.disk_bytes, 201));
    assert(!lmb_home_decide(&t, 1, o.ram_bytes, o.disk_bytes, 201));
    assert(t.phase == LMB_HOME_ACCEPTED && t.reservation_held);
    assert(lmb_home_offer_begin(&t, &o, o.tracker, o.ram_bytes, o.disk_bytes, 202));
    uint8_t wrong_id[32] = {9}; assert(lmb_home_commit(&t, wrong_id));
    assert(!lmb_home_commit(&t, o.id));
    assert(lmb_home_commit(&t, o.id)); /* no repeated launch */
    assert(lmb_home_start_host(&t, o.id)); /* no host before segment readiness */
    assert(!lmb_home_mark_segment_ready(&t));
    assert(lmb_home_start_host(&t, o.id)); /* this node is not Edge */
    assert(lmb_home_expired(&t, 200 + LMB_HOME_LEASE_MS));
    lmb_home_released(&t, LMB_HOME_CLOSED, "client disconnected");
    assert(!t.reservation_held && !lmb_home_expired(&t, UINT64_MAX));
    o.runs_edge = 1; o.edge_ram_bytes = 32u << 20;
    assert(!lmb_home_offer_begin(&t, &o, o.tracker, o.ram_bytes, o.disk_bytes, 300));
    assert(!lmb_home_decide(&t, 1, o.ram_bytes, o.disk_bytes, 301));
    assert(!lmb_home_commit(&t, o.id)); assert(!lmb_home_mark_segment_ready(&t));
    assert(!lmb_home_start_host(&t, o.id));
    strcpy(o.model, "../outside"); assert(!lmb_home_offer_valid(&o));
    puts("HOME TRANSACTION: PASS (consent, immutable commit, budgets, lease, ordered readiness)");
    return 0;
}
