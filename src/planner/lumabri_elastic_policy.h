/* Replica lifecycle policy, not cloud provisioning or donor consent. A
 * policy owns exact allocations; a traffic hint never grants authority. */
#ifndef LMB_ELASTIC_POLICY_H
#define LMB_ELASTIC_POLICY_H
#include "src/runtime/lumabri_model_routes.h"
#include "src/runtime/lumabri_preparation_request.h"
#include "src/runtime/lumabri_demand.h"

enum { LMB_ELASTIC_IDLE, LMB_ELASTIC_PREPARING, LMB_ELASTIC_PUBLISH_GROW,
       LMB_ELASTIC_PUBLISH_SHRINK, LMB_ELASTIC_RETIRING };
enum { LMB_ELASTIC_HOLD, LMB_ELASTIC_GROW, LMB_ELASTIC_SHRINK };
typedef struct {
    uint32_t revision, enabled, faulted, min_replicas, max_replicas;
    uint32_t cooldown, idle_seconds, up_rejections, ttft_ms, phase;
    uint64_t max_reserved_bytes, last_change, last_check;
    /* Bytes stay charged through preparation and retirement, including
     * uncertain outcomes. Never infer freed RAM from a missing heartbeat. */
    uint64_t resident_bytes[LMB_ROUTE_REPLICAS], pending_bytes;
    LmbModelRoute route, target;
    LmbPreparationRequest request;
    char source_stamp[65], reason[97];
    uint8_t pending_allocation[32], observer[32];
    uint64_t idle_since_ms;
} LmbElasticPolicy;

static inline int lmb_elastic_settings_valid(const LmbElasticPolicy *p) {
    if (!p || p->enabled>1 || p->faulted>1 || p->min_replicas>p->max_replicas ||
        !p->max_replicas || p->max_replicas>LMB_ROUTE_REPLICAS ||
        p->cooldown<30 || p->cooldown>86400 || p->idle_seconds<60 || p->idle_seconds>604800 ||
        !p->up_rejections || p->up_rejections>1000000 || p->ttft_ms>3600000 ||
        !p->max_reserved_bytes || p->max_reserved_bytes>(UINT64_C(1)<<60)) return 0;
    const LmbPreparationRequest *r=&p->request;
    if (r->model_count!=1 || !r->node_count || r->node_count>LMB_PREPARATION_NODES ||
        !r->context || r->context>131072 || !r->max_new || r->max_new>4096 || r->max_new>r->context ||
        !r->sessions || r->sessions>8 || strcmp(r->action,"start")) return 0;
    size_t len=strnlen(r->models[0],sizeof r->models[0]);
    if (!len || len==sizeof r->models[0] || !strcmp(r->models[0],".") || !strcmp(r->models[0],"..")) return 0;
    for (size_t i=0;i<len;i++) if ((unsigned char)r->models[0][i]<32 || r->models[0][i]==127 ||
        r->models[0][i]=='/' || r->models[0][i]=='\\') return 0;
    for (uint32_t i=0;i<r->node_count;i++) {
        if (!memchr(r->nodes[i],0,65) || !lmb_preparation_hex(r->nodes[i])) return 0;
        for (uint32_t j=0;j<i;j++) if (!strcmp(r->nodes[i],r->nodes[j])) return 0;
    }
    return 1;
}
static inline int lmb_elastic_total(const LmbElasticPolicy *p,uint64_t *total) {
    *total=p->phase==LMB_ELASTIC_PUBLISH_SHRINK ? 0 : p->pending_bytes;
    for (uint32_t i=0;i<p->route.count;i++) {
        if (!p->resident_bytes[i] || UINT64_MAX-*total<p->resident_bytes[i]) return -1;
        *total+=p->resident_bytes[i];
    }
    return *total>p->max_reserved_bytes ? -1 : 0;
}
static inline int lmb_elastic_route_equal(const LmbModelRoute *a,const LmbModelRoute *b) {
    LmbBuf x={0},y={0};
    int equal=!lmb_route_encode(a,&x) && !lmb_route_encode(b,&y) && x.len==y.len && !memcmp(x.p,y.p,x.len);
    free(x.p); free(y.p); return equal;
}
static inline int lmb_elastic_valid(const LmbElasticPolicy *p) {
    uint64_t bytes;
    if (!lmb_elastic_settings_valid(p) || !p->revision || !lmb_route_valid(&p->route) ||
        p->route.count>p->max_replicas || p->phase>LMB_ELASTIC_RETIRING ||
        !memchr(p->source_stamp,0,sizeof p->source_stamp) || !lmb_preparation_hex(p->source_stamp) ||
        !memchr(p->reason,0,sizeof p->reason) || lmb_elastic_total(p,&bytes) ||
        p->request.context<p->route.context || p->request.max_new<p->route.max_new) return 0;
    if (p->phase==LMB_ELASTIC_IDLE) return !p->pending_bytes && !lmb_route_nonzero(p->pending_allocation) &&
        !p->request.operation[0] && !p->request.review[0];
    if (!p->pending_bytes) return 0;
    if (p->phase==LMB_ELASTIC_PREPARING || p->phase==LMB_ELASTIC_PUBLISH_GROW) {
        if (!memchr(p->request.operation,0,65) || !lmb_preparation_hex(p->request.operation) ||
            !memchr(p->request.review,0,65) || !lmb_preparation_hex(p->request.review)) return 0;
        if (p->route.count>=p->max_replicas) return 0;
    }
    if (p->phase>=LMB_ELASTIC_PUBLISH_GROW && !lmb_route_nonzero(p->pending_allocation)) return 0;
    if (p->phase==LMB_ELASTIC_PUBLISH_GROW || p->phase==LMB_ELASTIC_PUBLISH_SHRINK) {
        LmbModelRoute expected=p->route;
        if (expected.revision==UINT32_MAX) return 0;
        expected.revision++;
        if (p->phase==LMB_ELASTIC_PUBLISH_GROW) {
            expected.replicas[expected.count++]=p->target.replicas[p->route.count];
            if (memcmp(expected.replicas[expected.count-1].allocation,p->pending_allocation,32)) return 0;
        } else {
            if (!expected.count || p->pending_bytes!=p->resident_bytes[expected.count-1] ||
                memcmp(expected.replicas[expected.count-1].allocation,p->pending_allocation,32)) return 0;
            expected.count--;
        }
        if (!lmb_elastic_route_equal(&expected,&p->target)) return 0;
    }
    return 1;
}
static inline void lmb_elastic_idle(LmbElasticPolicy *p) {
    p->phase=LMB_ELASTIC_IDLE; p->pending_bytes=0;
    memset(p->pending_allocation,0,32); memset(&p->target,0,sizeof p->target);
    p->request.operation[0]=p->request.review[0]=0; p->idle_since_ms=0;
}
/* Reconcile a crash at publication using the full contract, not just a
 * count/revision. Unrelated operator edits stop automation without erasing
 * pending allocation ownership. Returns 1 when already published. */
static inline int lmb_elastic_reconcile(LmbElasticPolicy *p,const LmbModelRoute *current,uint64_t now) {
    if (lmb_elastic_route_equal(&p->route,current)) return 0;
    if ((p->phase==LMB_ELASTIC_PUBLISH_GROW || p->phase==LMB_ELASTIC_PUBLISH_SHRINK) &&
        lmb_elastic_route_equal(&p->target,current)) {
        if (p->phase==LMB_ELASTIC_PUBLISH_GROW) {
            p->resident_bytes[p->route.count]=p->pending_bytes;
            p->route=*current; lmb_elastic_idle(p);
        } else {
            p->resident_bytes[current->count]=0; p->route=*current;
            p->phase=LMB_ELASTIC_RETIRING;
        }
        p->last_change=now; strcpy(p->reason,"publication_reconciled"); return 1;
    }
    p->faulted=1; strcpy(p->reason,"route_changed_operator_review_required"); return -1;
}
/* Positive demand may wake a cold model. Destructive scale-down requires
 * an uninterrupted complete observation window from this gateway instance
 * and is still subject to live host/Segment drain fences in the executor. */
static inline int lmb_elastic_choose(LmbElasticPolicy *p,const LmbDemand *d,uint64_t now_ms,uint64_t wall,
    const char **reason) {
    *reason="disabled";
    if (!p->enabled) return LMB_ELASTIC_HOLD;
    *reason="operator_review_required";
    if (p->faulted) return LMB_ELASTIC_HOLD;
    *reason="operation_in_progress";
    if (p->phase) return LMB_ELASTIC_HOLD;
    *reason="clock_moved_backwards";
    if (wall<p->last_check || wall<p->last_change) return LMB_ELASTIC_HOLD;
    if (memcmp(p->observer,d->instance,32)) { memcpy(p->observer,d->instance,32); p->idle_since_ms=0; }
    int fresh=d->started_ms && now_ms>=d->captured_ms && now_ms-d->captured_ms<=1000;
    int busy=0; LmbDemandSummary s={0};
    for (unsigned i=0;i<LMB_DEMAND_MODELS;i++) {
        const LmbDemandModel *m=&d->models[i];
        int logical=!memcmp(m->id,p->route.id,32), relevant=logical;
        for (uint32_t j=0;j<p->route.count;j++) relevant|=!memcmp(m->id,p->route.replicas[j].allocation,32);
        if (!relevant) continue;
        LmbDemandSummary part; lmb_demand_summary(m,now_ms,&part);
        busy|=m->pending || m->admitted || part.counts[LMB_DEMAND_STARTED]!=0;
        /* Only this logical model drives growth. A direct allocation may
         * be shared by another client, and must not be counted twice. */
        if (logical) s=part;
    }
    if (!fresh || !lmb_demand_window_complete(d,now_ms) || busy) p->idle_since_ms=0;
    else if (!p->idle_since_ms || now_ms<p->idle_since_ms) p->idle_since_ms=now_ms;
    /* Wake is not an optimization switch: waiting a cooldown longer than
     * the demand window would forget the first request to a dormant model.
     * The full idle grace still bounds subsequent release/prepare churn. */
    int cold=!p->route.count && s.counts[LMB_DEMAND_STARTED];
    if (fresh && cold) { *reason="on_demand_wake"; return LMB_ELASTIC_GROW; }
    *reason="cooldown";
    if (p->last_change && wall-p->last_change<p->cooldown) return LMB_ELASTIC_HOLD;
    if (p->route.count<p->min_replicas) { *reason="resident_minimum"; return LMB_ELASTIC_GROW; }
    int pressure=s.counts[LMB_DEMAND_CAPACITY]>=p->up_rejections ||
        (p->ttft_ms && s.ttft_samples>=4 && s.ttft_p95_us>(uint64_t)p->ttft_ms*1000);
    if (fresh && (cold || pressure) && p->route.count<p->max_replicas) {
        *reason=cold ? "on_demand_wake" : "observed_demand"; return LMB_ELASTIC_GROW;
    }
    if (p->route.count>p->min_replicas && p->idle_since_ms && now_ms>=p->idle_since_ms &&
        now_ms-p->idle_since_ms>=(uint64_t)p->idle_seconds*1000) {
        *reason="observed_idle"; return LMB_ELASTIC_SHRINK;
    }
    *reason=!fresh || !lmb_demand_window_complete(d,now_ms) ? "observation_window_incomplete" : "within_limits";
    return LMB_ELASTIC_HOLD;
}
#endif
