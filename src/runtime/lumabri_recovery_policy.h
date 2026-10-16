/* A turn may opt into waiting for capacity prepared by an already enabled
 * operator policy. It never authorizes a new policy, node or allocation.
 * Policy edits revoke this authority, even if subsequently reverted. */
#ifndef LMB_RECOVERY_POLICY_H
#define LMB_RECOVERY_POLICY_H
#include "src/planner/lumabri_elastic_store.h"

static inline int lmb_recovery_policy_key(const LmbElasticPolicy *p,uint8_t key[32]) {
    if (!lmb_elastic_valid(p) || !p->enabled || p->faulted) return -1;
    LmbBuf b={0};
    const uint32_t fields[]={p->revision,p->min_replicas,p->max_replicas,p->cooldown,p->idle_seconds,
        p->up_rejections,p->ttft_ms,p->request.context,p->request.sessions,p->request.max_new,p->request.node_count};
    int bad=lmb_buf_str(&b,"LMB-TURN-RECOVERY1") || lmb_buf_str(&b,p->source_stamp) ||
        lmb_buf_str(&b,p->request.models[0]) || lmb_buf_u64(&b,p->max_reserved_bytes);
    for (unsigned i=0;!bad && i<sizeof fields/sizeof *fields;i++) bad=lmb_buf_u32(&b,fields[i]);
    for (uint32_t i=0;!bad && i<p->request.node_count;i++) bad=lmb_buf_str(&b,p->request.nodes[i]);
    if (!bad) lmb_elastic_digest(b.p,b.len,key);
    free(b.p); return bad ? -1 : 0;
}
static inline int lmb_recovery_policy_route(const LmbElasticPolicy *p,const uint8_t key[32],
    const LmbModelRoute *original,const LmbModelRoute *current) {
    uint8_t actual[32];
    if (lmb_recovery_policy_key(p,actual) || memcmp(key,actual,32)) return 0;
    int settled=lmb_elastic_route_equal(&p->route,current);
    /* Publication is two journalled writes. A visible, authorized target
     * is pending, not a manual edit and not yet usable by this reader. */
    if (!settled && (p->phase!=LMB_ELASTIC_PUBLISH_GROW ||
        !lmb_elastic_route_equal(&p->target,current))) return 0;
    /* Only the controller's replica set and its publication revision may
     * change. Identity, numeric class, limits and routing policy stay fixed. */
    LmbModelRoute normalized=*current;
    normalized.count=original->count; normalized.revision=original->revision;
    memcpy(normalized.replicas,original->replicas,sizeof normalized.replicas);
    return lmb_elastic_route_equal(original,&normalized) ? (settled ? 1 : 2) : 0;
}
#endif
