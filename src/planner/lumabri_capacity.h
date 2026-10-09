/* Observed joint workload, not an extrapolated admission guarantee. A probe
 * measures an explicit mix of approved allocations under a bounded burst.
 * All requests (including rejections) remain in the denominator. */
#ifndef LMB_CAPACITY_H
#define LMB_CAPACITY_H
#include "lumabri_calibration_store.h"
#include "lumabri_resource_facts.h"

#define LMB_CAPACITY_MODELS 8u
#define LMB_CAPACITY_CLIENTS 8u
#define LMB_CAPACITY_ROUNDS 16u
#define LMB_CAPACITY_SAMPLES (LMB_CAPACITY_CLIENTS * LMB_CAPACITY_ROUNDS)
#define LMB_CAPACITY_BYTES 16384u
typedef struct {
    uint32_t model, round, status, complete, timing_known, prompt_tokens, generated_tokens;
    double ttft, gap_p95, completion;
} LmbCapacitySample;
typedef struct {
    uint8_t fingerprint[32], allocations[LMB_CAPACITY_MODELS][32], contents[LMB_CAPACITY_MODELS][32];
    uint8_t contracts[LMB_CAPACITY_MODELS][32];
    uint32_t numeric_abi[LMB_CAPACITY_MODELS];
    char numeric_class[LMB_CAPACITY_MODELS][97];
    uint32_t models, clients, rounds, max_new, count, stable;
    uint64_t measured_at;
    LmbCapacitySample samples[LMB_CAPACITY_SAMPLES];
} LmbCapacity;
typedef struct {
    uint32_t requests, completed, rejected, failed, measured;
    double ttft_p95, gap_p95_of_response_p95, completion_p95;
} LmbCapacitySummary;
typedef struct { int known; uint64_t micro_per_hour; char currency[4]; } LmbCapacityCost;
static inline void lmb_capacity_cost_add(LmbCapacityCost *cost,const LmbResourceFacts *f) {
    if (!(f->known&LMB_FACT_PRICE) || !lmb_resource_facts_valid(f)) { cost->known=0; return; }
    if (!cost->currency[0]) memcpy(cost->currency,f->currency,4);
    if (memcmp(cost->currency,f->currency,4) || UINT64_MAX-cost->micro_per_hour<f->price_micro_per_hour) cost->known=0;
    else cost->micro_per_hour+=f->price_micro_per_hour;
}
static inline int lmb_capacity_choose(const LmbCapacityCost *costs,const int *eligible,uint32_t n,int *prices) {
    *prices=1; int best=-1; char currency[4]={0};
    if (!n || n>LMB_CAPACITY_MODELS) return -1;
    for (uint32_t i=0;i<n;i++) if (eligible[i]) {
        if (!costs[i].known || !costs[i].currency[0] || costs[i].currency[3]) *prices=0;
        else {
            if (!currency[0]) memcpy(currency,costs[i].currency,4);
            if (memcmp(currency,costs[i].currency,4)) *prices=0;
        }
    }
    for (uint32_t i=0;i<n;i++) if (eligible[i] && (best<0 ||
        (*prices && costs[i].micro_per_hour<costs[best].micro_per_hour))) best=(int)i;
    return best;
}

/* Canonical fields, never padding or a made-up calibration. This identity
 * also binds the co-resident allocation set on every participating machine. */
static inline int lmb_capacity_key_hash(const LmbCalKey *k, uint8_t out[32]) {
    if (!lmb_cal_key_valid(k)) return -1;
    LmbBuf b={0}; int bad=0;
#define TEXT(f) bad |= lmb_buf_str(&b,k->f)
#define NUM(f) bad |= lmb_buf_u32(&b,k->f)
    bad |= lmb_buf_str(&b,"LMB-CAPACITY-KEY1");
    TEXT(model_root); TEXT(adapter); NUM(adapter_abi); TEXT(numeric_class);
    TEXT(commit_lumabri); TEXT(commit_colibri); TEXT(build_id); TEXT(plan_kind);
    NUM(goal); NUM(nodes); NUM(edge_node); NUM(context); NUM(sessions);
    for (uint32_t i=0;i<k->nodes;i++) {
        TEXT(node_id[i]); TEXT(node_hardware_id[i]); TEXT(node_build_id[i]); TEXT(node_backend[i]);
        NUM(layer_begin[i]); NUM(layer_end[i]); NUM(threads[i]); NUM(from_disk[i]);
        const LmbWorkloadFacts *w=&k->workload[i];
        bad |= lmb_buf_u32(&b,w->known); bad |= lmb_buf_u32(&b,w->allocations);
        bad |= lmb_buf_u32(&b,w->compute_policy); bad |= lmb_buf_u64(&b,w->reserved_bytes);
        bad |= lmb_buf_bytes(&b,w->allocation_set,32);
    }
#undef TEXT
#undef NUM
    if (!bad) { LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,b.p,b.len); lmb_sha_final(&sha,out); }
    free(b.p); return bad ? -1 : 0;
}
static inline int lmb_capacity_valid(const LmbCapacity *r) {
    if (!r || !r->models || r->models>LMB_CAPACITY_MODELS || r->clients<r->models ||
        r->clients>LMB_CAPACITY_CLIENTS || !r->rounds || r->rounds>LMB_CAPACITY_ROUNDS ||
        r->max_new<2 || r->max_new>512 || r->count!=r->clients*r->rounds || r->stable>1 || !r->measured_at) return 0;
    uint8_t zero[32]={0};
    if (!memcmp(r->fingerprint,zero,32)) return 0;
    for (uint32_t i=0;i<r->models;i++) {
        if (!memcmp(r->allocations[i],zero,32) || !memcmp(r->contents[i],zero,32) || !memcmp(r->contracts[i],zero,32) ||
            !r->numeric_abi[i] || !lmb_cal_text(r->numeric_class[i],sizeof r->numeric_class[i])) return 0;
        for (uint32_t j=0;j<i;j++) if (!memcmp(r->allocations[i],r->allocations[j],32)) return 0;
    }
    for (uint32_t i=0;i<r->count;i++) {
        const LmbCapacitySample *s=&r->samples[i];
        if (s->model!=(i%r->clients)%r->models || s->round!=i/r->clients || s->complete>1 || s->timing_known>1 ||
            s->status>599 || (s->status && s->status<100) ||
            !isfinite(s->ttft) || !isfinite(s->gap_p95) || !isfinite(s->completion) ||
            s->ttft<0 || s->gap_p95<0 || s->completion<0 || s->completion>3600 ||
            s->ttft>s->completion || s->gap_p95>s->completion ||
            (s->complete && s->status!=200) || (s->timing_known && (!s->complete ||
                !s->prompt_tokens || s->generated_tokens<2 || s->generated_tokens>r->max_new)) ||
            (!s->timing_known && (s->ttft || s->gap_p95 || s->prompt_tokens || s->generated_tokens))) return 0;
    }
    return 1;
}
static inline int lmb_capacity_order(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y);
}
static inline double lmb_capacity_p95(double *values,uint32_t n) {
    if (!n) return 0;
    qsort(values,n,sizeof *values,lmb_capacity_order); return values[(95*n+99)/100-1];
}
static inline LmbCapacitySummary lmb_capacity_summary(const LmbCapacity *r,uint32_t model) {
    LmbCapacitySummary out={0}; double first[LMB_CAPACITY_SAMPLES],gap[LMB_CAPACITY_SAMPLES],end[LMB_CAPACITY_SAMPLES];
    for (uint32_t i=0;i<r->count && i<LMB_CAPACITY_SAMPLES;i++) {
        const LmbCapacitySample *s=&r->samples[i]; if (s->model!=model) continue;
        out.requests++;
        if (s->complete) out.completed++;
        else if (s->status==429) out.rejected++;
        else out.failed++;
        if (s->timing_known) { uint32_t at=out.measured++; first[at]=s->ttft; gap[at]=s->gap_p95; end[at]=s->completion; }
    }
    out.ttft_p95=lmb_capacity_p95(first,out.measured);
    out.gap_p95_of_response_p95=lmb_capacity_p95(gap,out.measured);
    out.completion_p95=lmb_capacity_p95(end,out.measured); return out;
}
/* No "pass" for missing telemetry, failed clients, too few repetitions,
 * changed configuration, future timestamps or evidence outside its TTL. */
static inline const char *lmb_capacity_evaluate(const LmbCapacity *r,const uint8_t current[32],
    uint64_t now,uint32_t max_age,double ttft_limit,double gap_limit) {
    if (!lmb_capacity_valid(r) || !current || !max_age || max_age>86400 ||
        !isfinite(ttft_limit) || !isfinite(gap_limit) || ttft_limit<=0 || gap_limit<=0) return "invalid_evidence_or_limits";
    if (!r->stable || memcmp(r->fingerprint,current,32)) return "configuration_changed";
    if (now<r->measured_at || now-r->measured_at>max_age) return "expired";
    if (r->rounds<3) return "insufficient_rounds";
    for (uint32_t m=0;m<r->models;m++) {
        LmbCapacitySummary s=lmb_capacity_summary(r,m);
        if (s.rejected || s.failed) return "requests_rejected_or_failed";
        if (s.measured!=s.requests) return "timing_unavailable";
        if (s.ttft_p95>ttft_limit || s.gap_p95_of_response_p95>gap_limit) return "latency_target_missed";
    }
    return "observed_workload_passed";
}
/* Comparisons require the same ordered model mix and actual token lengths.
 * Allocation IDs may differ: that is the point of comparing placements. */
static inline int lmb_capacity_comparable(const LmbCapacity *a,const LmbCapacity *b) {
    if (!lmb_capacity_valid(a) || !lmb_capacity_valid(b) || a->models!=b->models || a->clients!=b->clients ||
        a->rounds!=b->rounds || a->max_new!=b->max_new || memcmp(a->contents,b->contents,a->models*32) ||
        memcmp(a->contracts,b->contracts,a->models*32)) return 0;
    for (uint32_t m=0;m<a->models;m++) {
        uint32_t prompt=0,generated=0;
        const LmbCapacity *set[2]={a,b};
        for (unsigned k=0;k<2;k++) for (uint32_t i=0;i<set[k]->count;i++) {
            const LmbCapacitySample *s=&set[k]->samples[i];
            if (s->model!=m || !s->timing_known) continue;
            if (prompt && (s->prompt_tokens!=prompt || s->generated_tokens!=generated)) return 0;
            prompt=s->prompt_tokens; generated=s->generated_tokens;
        }
        if (!prompt) return 0;
    }
    return 1;
}
#endif
