/* Shared bounded resident-plan format. A saved plan is a restart hint, never
 * authority to recreate engines; the live host must prove its pinned root. */
#ifndef LMB_RESIDENT_PLAN_CODEC_H
#define LMB_RESIDENT_PLAN_CODEC_H
#include "lumabri_home.h"
#include "src/runtime/lumabri_calibration_codec.h"
#include "src/ui/lumabri_execution_view.h"
typedef struct {
    char tracker[256], host[64], host_key[65], root[65], model[64];
    uint32_t context, max_new, sessions; /* 0 in older restart hints means one */
    LmbExecutionView execution;
    uint8_t allocation[32], peer_keys[LMB_CLUSTER_MAX_NODES][32];
    double preparation_seconds, prepared_at;
    char content_id[65]; /* source content identity, distinct from the named routing root */
    /* Preparation provenance, not a speed or permission to recreate engines.
     * Numeric ABI/class are learned only from the live authenticated Edge. */
    struct {
        char build_id[65], adapter[32];
        uint32_t goal;
        char hardware[LMB_CLUSTER_MAX_NODES][65], runtime[LMB_CLUSTER_MAX_NODES][65];
        uint32_t threads[LMB_CLUSTER_MAX_NODES];
    } observation;
} LmbResidentPlan;

static int home_resident_digest(const char value[65]) {
    uint8_t bytes[32];
    return memchr(value, 0, 65) && strlen(value) == 64 && !lmb_unhex(bytes, value, 32);
}

static int home_resident_plan_valid(const LmbResidentPlan *p) {
    if (!p || p->execution.count > LMB_CLUSTER_MAX_NODES || p->sessions > LMB_HOST_MAX_SESSIONS) return 0;
    if (!isfinite(p->preparation_seconds) || p->preparation_seconds < 0 || p->preparation_seconds > 86400 ||
        !isfinite(p->prepared_at) || p->prepared_at < 0 || (!!p->prepared_at != !!p->preparation_seconds)) return 0;
    if (lmb_home_nonzero(p->allocation, 32))
        for (uint32_t i = 0; i < p->execution.count; i++) if (!lmb_home_nonzero(p->peer_keys[i], 32)) return 0;
    uint8_t bytes[32];
    if (!memchr(p->content_id, 0, sizeof p->content_id) ||
        (p->content_id[0] && (strlen(p->content_id) != 64 || lmb_unhex(bytes, p->content_id, 32)))) return 0;
    if (p->observation.build_id[0]) {
        if (!p->sessions || !p->content_id[0] || !lmb_home_nonzero(p->allocation, 32) ||
            !home_resident_digest(p->observation.build_id) || p->observation.goal > 1 ||
            !lmb_cal_text(p->observation.adapter, sizeof p->observation.adapter)) return 0;
        for (uint32_t i = 0; i < p->execution.count; i++)
            if (!home_resident_digest(p->observation.hardware[i]) || !home_resident_digest(p->observation.runtime[i]) ||
                !p->observation.threads[i] || p->observation.threads[i] > 256) return 0;
    }
    return lmb_cal_text(p->tracker, sizeof p->tracker) &&
        lmb_cal_text(p->host, sizeof p->host) &&
        lmb_cal_text(p->model, sizeof p->model) &&
        lmb_cal_text(p->host_key, sizeof p->host_key) && strlen(p->host_key) == 64 &&
        !lmb_unhex(bytes, p->host_key, 32) &&
        lmb_cal_text(p->root, sizeof p->root) && strlen(p->root) == 64 &&
        !lmb_unhex(bytes, p->root, 32) &&
        p->context && p->context <= (1u << 20) && p->max_new && p->max_new <= (1u << 20) &&
        lmb_execution_valid(&p->execution);
}

static int home_resident_plan_pack(const LmbResidentPlan *p,LmbBuf *out) {
    if (!home_resident_plan_valid(p)) return -1;
    LmbBuf b = {0};
    /* The validated fixed-capacity fields fit this bound. Reserve before
     * packing so allocation failure cannot produce a partial saved plan. */
    if (lmb_buf_reserve(&b, 65536)) return -1;
    int managed = lmb_home_nonzero(p->allocation, 32);
    if (p->sessions && !managed) { free(b.p); return -1; }
    lmb_buf_u32(&b, p->observation.build_id[0] ? 6 : managed ? p->sessions ? 5 : 4 : p->execution.hybrid ? 2 : 1);
    lmb_buf_str(&b, p->tracker); lmb_buf_str(&b, p->host);
    lmb_buf_str(&b, p->host_key); lmb_buf_str(&b, p->root); lmb_buf_str(&b, p->model);
    lmb_buf_u32(&b, p->context); lmb_buf_u32(&b, p->max_new);
    lmb_buf_u32(&b, p->execution.count); lmb_buf_u32(&b, p->execution.layers);
    if (managed || p->execution.hybrid) lmb_buf_u32(&b, p->execution.hybrid);
    for (uint32_t i = 0; i < p->execution.count; i++) {
        const LmbExecutionNode *n = &p->execution.nodes[i];
        lmb_buf_str(&b, n->name); lmb_buf_str(&b, n->address);
        lmb_buf_u32(&b, n->begin); lmb_buf_u32(&b, n->end);
        lmb_buf_u64(&b, n->reserved_bytes); lmb_buf_u32(&b, (uint32_t)n->edge);
    }
    if (managed) {
        lmb_buf_bytes(&b, p->allocation, 32);
        lmb_buf_bytes(&b, p->peer_keys, p->execution.count * 32);
        lmb_cal_put_double(&b, p->preparation_seconds);
        lmb_cal_put_double(&b, p->prepared_at);
        lmb_buf_str(&b, p->content_id);
        if (p->sessions) lmb_buf_u32(&b, p->sessions);
        if (p->observation.build_id[0]) {
            lmb_buf_str(&b, p->observation.build_id); lmb_buf_str(&b, p->observation.adapter);
            lmb_buf_u32(&b, p->observation.goal);
            for (uint32_t i = 0; i < p->execution.count; i++) {
                lmb_buf_str(&b, p->observation.hardware[i]); lmb_buf_str(&b, p->observation.runtime[i]);
                lmb_buf_u32(&b, p->observation.threads[i]);
            }
        }
    }
    *out=b;return 0;
}

static int home_resident_plan_unpack(const uint8_t *bytes,size_t length,const char *tracker,LmbResidentPlan *p) {
    memset(p,0,sizeof *p);
    if (!bytes || !tracker || !length || length>65536) return -1;
    LmbCur c = {bytes, length, 0}; uint32_t version;
    if (lmb_cur_u32(&c, &version) || version < 1 || version > 6 ||
        lmb_inventory_string(&c, p->tracker, sizeof p->tracker) || strcmp(tracker, p->tracker) ||
        lmb_inventory_string(&c, p->host, sizeof p->host) ||
        lmb_inventory_string(&c, p->host_key, sizeof p->host_key) ||
        lmb_inventory_string(&c, p->root, sizeof p->root) ||
        lmb_inventory_string(&c, p->model, sizeof p->model) ||
        lmb_cur_u32(&c, &p->context) || lmb_cur_u32(&c, &p->max_new) ||
        lmb_cur_u32(&c, &p->execution.count) || lmb_cur_u32(&c, &p->execution.layers) ||
        p->execution.count > LMB_CLUSTER_MAX_NODES) return -1;
    if (version >= 2 && (lmb_cur_u32(&c, &p->execution.hybrid) || p->execution.hybrid > 1 ||
        (version == 2 && !p->execution.hybrid))) return -1;
    for (uint32_t i = 0; i < p->execution.count; i++) {
        LmbExecutionNode *n = &p->execution.nodes[i]; uint32_t edge;
        if (lmb_inventory_string(&c, n->name, sizeof n->name) ||
            lmb_inventory_string(&c, n->address, sizeof n->address) ||
            lmb_cur_u32(&c, &n->begin) || lmb_cur_u32(&c, &n->end) ||
            lmb_cur_u64(&c, &n->reserved_bytes) || lmb_cur_u32(&c, &edge) || edge > 1) return -1;
        n->edge = (int)edge;
    }
    if (version >= 3) {
        size_t size = 32 + p->execution.count * 32;
        if (c.len - c.off < size || (version == 3 && c.len - c.off != size)) return -1;
        memcpy(p->allocation, c.p + c.off, 32); c.off += 32;
        memcpy(p->peer_keys, c.p + c.off, size - 32); c.off += size - 32;
        if (!lmb_home_nonzero(p->allocation, 32)) return -1;
        for (uint32_t i = 0; i < p->execution.count; i++)
            if (!lmb_home_nonzero(p->peer_keys[i], 32)) return -1;
        if (version >= 4 && (lmb_cal_get_double(&c, &p->preparation_seconds) ||
                            lmb_cal_get_double(&c, &p->prepared_at) ||
                            lmb_inventory_string(&c, p->content_id, sizeof p->content_id))) return -1;
        if (version >= 5 && (lmb_cur_u32(&c, &p->sessions) || !p->sessions)) return -1;
        if (version == 6) {
            if (lmb_inventory_string(&c, p->observation.build_id, sizeof p->observation.build_id) ||
                !p->observation.build_id[0] ||
                lmb_inventory_string(&c, p->observation.adapter, sizeof p->observation.adapter) ||
                lmb_cur_u32(&c, &p->observation.goal)) return -1;
            for (uint32_t i = 0; i < p->execution.count; i++)
                if (lmb_inventory_string(&c, p->observation.hardware[i], sizeof p->observation.hardware[i]) ||
                    lmb_inventory_string(&c, p->observation.runtime[i], sizeof p->observation.runtime[i]) ||
                    lmb_cur_u32(&c, &p->observation.threads[i])) return -1;
        }
    }
    return c.off == c.len && home_resident_plan_valid(p) ? 0 : -1;
}


static int home_resident_observation_key(const LmbResidentPlan *p, LmbCalKey *key) {
    memset(key, 0, sizeof *key);
    if (!home_resident_plan_valid(p) || !p->observation.build_id[0]) return -1;
    snprintf(key->model_root, sizeof key->model_root, "%s", p->content_id);
    snprintf(key->adapter, sizeof key->adapter, "%s", p->observation.adapter);
    snprintf(key->build_id, sizeof key->build_id, "%s", p->observation.build_id);
    snprintf(key->plan_kind, sizeof key->plan_kind, "%s", p->execution.hybrid ? "hybrid" : "segment");
    key->goal = p->observation.goal; key->context = p->context; key->sessions = p->sessions;
    key->nodes = p->execution.count;
    for (uint32_t i = 0; i < key->nodes; i++) {
        lmb_hex(key->node_id[i], p->peer_keys[i], 32);
        snprintf(key->node_hardware_id[i], sizeof key->node_hardware_id[i], "%s", p->observation.hardware[i]);
        snprintf(key->node_build_id[i], sizeof key->node_build_id[i], "%s", p->observation.runtime[i]);
        snprintf(key->node_backend[i], sizeof key->node_backend[i], "cpu");
        key->threads[i] = p->observation.threads[i];
        key->layer_begin[i] = p->execution.nodes[i].begin; key->layer_end[i] = p->execution.nodes[i].end;
        if (p->execution.nodes[i].edge) key->edge_node = i;
    }
    return 0;
}

#endif
