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
enum { LMB_ELASTIC_UNKNOWN, LMB_ELASTIC_READY, LMB_ELASTIC_DRAINING, LMB_ELASTIC_UNREACHABLE };
typedef struct {
    uint64_t at_ms, at_wall;
    uint32_t state, failures;
} LmbElasticReplicaHealth;
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
    /* Observations are not ownership receipts. Unreachable allocations
     * count towards both hard limits until retirement confirms release. */
    uint8_t health_observer[32];
    uint32_t health_revision, health_cursor;
    LmbElasticReplicaHealth health[LMB_ROUTE_REPLICAS];
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
    if (p->health_cursor>=LMB_ROUTE_REPLICAS) return 0;
    for (unsigned i=0;i<LMB_ROUTE_REPLICAS;i++) {
        const LmbElasticReplicaHealth *h=&p->health[i];
        if (h->state>LMB_ELASTIC_UNREACHABLE || h->failures>3 ||
            (h->state!=LMB_ELASTIC_UNREACHABLE && h->failures) ||
            (h->state && (!h->at_ms || !h->at_wall))) return 0;
    }
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
            uint32_t at=0;
            while (at<expected.count && memcmp(expected.replicas[at].allocation,p->pending_allocation,32)) at++;
            if (at==expected.count || p->pending_bytes!=p->resident_bytes[at]) return 0;
            for (uint32_t i=at;i+1<expected.count;i++) expected.replicas[i]=expected.replicas[i+1];
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
            uint32_t at=0;
            while (at<p->route.count && memcmp(p->route.replicas[at].allocation,p->pending_allocation,32)) at++;
            if (at==p->route.count) { p->faulted=1; strcpy(p->reason,"retiring_identity_missing"); return -1; }
            for (uint32_t i=at;i+1<p->route.count;i++) p->resident_bytes[i]=p->resident_bytes[i+1];
            p->resident_bytes[current->count]=0; p->route=*current;
            p->phase=LMB_ELASTIC_RETIRING;
        }
        p->last_change=now; strcpy(p->reason,"publication_reconciled"); return 1;
    }
    p->faulted=1; strcpy(p->reason,"route_changed_operator_review_required"); return -1;
}
/* Identity, not array position, owns the pending charge. Removing a failed
 * early replica must not remove or misaccount a healthy newer one. */
static inline int lmb_elastic_withdraw(LmbElasticPolicy *p,uint32_t at) {
    if (!p->enabled || p->faulted || p->phase || at>=p->route.count || p->route.revision==UINT32_MAX) return -1;
    p->pending_bytes=p->resident_bytes[at]; memcpy(p->pending_allocation,p->route.replicas[at].allocation,32);
    p->target=p->route; p->target.revision++;
    for (uint32_t i=at;i+1<p->target.count;i++) p->target.replicas[i]=p->target.replicas[i+1];
    p->target.count--; p->phase=LMB_ELASTIC_PUBLISH_SHRINK;
    return lmb_elastic_valid(p) ? 0 : -1;
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
/* Probe one replica per tick, without opening an inference session. Three
 * separated failures are required; a restart, route change, long gap or
 * successful probe resets this evidence rather than inheriting an outage. */
static inline int lmb_elastic_health_next(LmbElasticPolicy *p,const LmbDemand *d,uint64_t now) {
    if (!p->enabled || p->faulted || p->phase || !p->route.count || !now) return -1;
    if (p->health_revision!=p->route.revision || memcmp(p->health_observer,d->instance,32)) {
        memset(p->health,0,sizeof p->health); p->health_cursor=0;
        p->health_revision=p->route.revision; memcpy(p->health_observer,d->instance,32);
    }
    unsigned index=p->health_cursor%p->route.count;
    const LmbElasticReplicaHealth *h=&p->health[index];
    if (h->at_ms && now>=h->at_ms && now-h->at_ms<5000) return -1;
    p->health_cursor=(index+1)%p->route.count;
    return (int)index;
}
static inline void lmb_elastic_health_observe(LmbElasticPolicy *p,unsigned index,unsigned state,
                                             uint64_t now,uint64_t wall) {
    if (index>=p->route.count || state>LMB_ELASTIC_UNREACHABLE || !now || !wall) return;
    LmbElasticReplicaHealth *h=&p->health[index]; unsigned failures=0;
    if (state==LMB_ELASTIC_UNREACHABLE) {
        failures=1;
        if (h->state==state && now>=h->at_ms && now-h->at_ms>=5000 && now-h->at_ms<=120000)
            failures=h->failures<3 ? h->failures+1 : 3;
    }
    *h=(LmbElasticReplicaHealth){now,wall,state,failures};
}
static inline int lmb_elastic_health_choice(LmbElasticPolicy *p,const LmbDemand *d,uint64_t now,uint64_t wall,
                                           int choice,const char **reason) {
    if (!p->enabled || p->faulted || p->phase || wall<p->last_check || wall<p->last_change) return choice;
    unsigned failed=0,ready=0;
    int same=p->health_revision==p->route.revision && !memcmp(p->health_observer,d->instance,32);
    for (uint32_t i=0;i<p->route.count;i++) {
        const LmbElasticReplicaHealth *h=&p->health[i];
        if (!same || !h->at_ms || now<h->at_ms || now-h->at_ms>120000) continue;
        ready+=h->state==LMB_ELASTIC_READY;
        failed+=h->state==LMB_ELASTIC_UNREACHABLE && h->failures==3;
    }
    /* In particular, never remove the newly healthy last replica while
     * counting an unreachable old replica as the resident minimum. */
    if (ready<p->route.count) {
        p->idle_since_ms=0;
        if (choice==LMB_ELASTIC_SHRINK) { choice=LMB_ELASTIC_HOLD; *reason="replica_health_not_ready_for_release"; }
    }
    unsigned wanted=p->min_replicas;
    /* Positive recent demand may restore an on-demand model too. Unlike
     * destructive idle evidence this remains useful across a bounded probe. */
    if (!wanted && d->captured_ms && now>=d->captured_ms && now-d->captured_ms<=30000) {
        for (unsigned i=0;i<LMB_DEMAND_MODELS;i++) if (!memcmp(d->models[i].id,p->route.id,32)) {
            LmbDemandSummary s; lmb_demand_summary(&d->models[i],now,&s);
            wanted=d->models[i].pending || d->models[i].admitted || s.counts[LMB_DEMAND_STARTED] ? 1 : 0;
            break;
        }
    }
    /* Deliberate drain and unobserved replicas are not confirmed failures.
     * Neither a failed probe nor a completed replacement frees old RAM. */
    if (!failed || p->route.count-failed>=wanted) return choice;
    *reason="unavailable_replica_recovery_cooldown";
    if (p->last_change && wall-p->last_change<p->cooldown) return LMB_ELASTIC_HOLD;
    *reason="unavailable_replica_limit_operator_review_required";
    if (p->route.count>=p->max_replicas) return LMB_ELASTIC_HOLD;
    *reason="restore_available_replica_minimum";
    return LMB_ELASTIC_GROW;
}
/* Next confirmed-unreachable replica whose exit may be proved, at most one
 * per tick. The search starts at the persisted probe cursor, so a replica
 * whose exit can never be proved (powered-off host) cannot hide a later
 * failed replica forever. Selection is not evidence of exit. */
static inline int lmb_elastic_failed_next(const LmbElasticPolicy *p,const uint8_t observer[32],
                                         uint64_t now,uint64_t wall) {
    if (!p->enabled || p->faulted || p->phase || wall<p->last_change || wall<p->last_check ||
        (p->last_change && wall-p->last_change<p->cooldown) ||
        p->health_revision!=p->route.revision || memcmp(p->health_observer,observer,32)) return -1;
    for (uint32_t k=0;k<p->route.count;k++) {
        uint32_t i=(p->health_cursor+k)%p->route.count;
        const LmbElasticReplicaHealth *h=&p->health[i];
        if (h->state==LMB_ELASTIC_UNREACHABLE && h->failures==3 && h->at_ms && now>=h->at_ms &&
            now-h->at_ms<=120000) return (int)i;
    }
    return -1;
}
#endif
