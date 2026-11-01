/* One leased inventory snapshot per preference decision. This is not a
 * measured concurrency envelope or a promise about the next prompt. */
#ifndef LMB_ROUTE_EVIDENCE_H
#define LMB_ROUTE_EVIDENCE_H
#include "src/planner/lumabri_replica_rank.h"
#include "src/planner/lumabri_calibration_profiles.h"

static int api_replica_evidence(const LmbModelRoute *route, const LmbResidentPlan *plan,
    const LmbMachineReport *reports, uint32_t count, LmbReplicaEvidence *out) {
    memset(out,0,sizeof *out); out->usable=1;
    LmbCalibration seed; char records[1200],why[200];
    if (home_resident_observation_seed(plan,&seed,records) ||
        catalog_runtime_match(&seed.key,reports,count,why,sizeof why)) return -1;
    out->price_known=1;
    int load_valid=1;
    for (uint32_t i=0;i<seed.key.nodes;i++) {
        const LmbMachineReport *found=NULL;
        for (uint32_t j=0;j<count;j++) if (!memcmp(plan->peer_keys[i],reports[j].identity,32)) { found=&reports[j]; break; }
        if (!found || !found->workload.known || !found->workload.allocations ||
            found->workload.reserved_bytes<plan->execution.nodes[i].reserved_bytes) return -1;
        seed.key.workload[i]=found->workload;
        seed.key.workload[i].active=seed.key.workload[i].queued=0;
        int duplicate=0;
        for (uint32_t k=0;k<i;k++) if (!memcmp(plan->peer_keys[k],plan->peer_keys[i],32)) duplicate=1;
        if (duplicate) continue;
        if (lmb_replica_load_add(&out->load,&found->workload,found->machine.ram_total_bytes,
                plan->execution.nodes[i].reserved_bytes,found->age_ms)) load_valid=0;
        const LmbResourceFacts *facts=&found->facts;
        if (!(facts->known&LMB_FACT_PRICE) || !lmb_resource_facts_valid(facts)) out->price_known=0;
        else {
            if (!out->currency[0]) memcpy(out->currency,facts->currency,4);
            else if (memcmp(out->currency,facts->currency,4)) out->price_known=0;
            if (UINT64_MAX-out->micro_per_hour<facts->price_micro_per_hour) out->price_known=0;
            else out->micro_per_hour+=facts->price_micro_per_hour;
        }
    }
    if (!load_valid) memset(&out->load,0,sizeof out->load);
    seed.key.adapter_abi=route->numeric_abi;
    snprintf(seed.key.numeric_class,sizeof seed.key.numeric_class,"%s",route->numeric_class);
    LmbCalibration observed;
    if (!lmb_cal_profile_load(records,&seed.key,&observed)) {
        out->observation_current=1; out->decode_tok_s=observed.decode_tok_s;
        out->ttft_seconds=observed.ttft_seconds; out->observation_source=observed.source;
        out->measured_at=observed.measured_at; out->prompt_tokens=observed.prompt_tokens;
        out->generated_tokens=observed.generated_tokens;
    }
    return 0;
}
/* The inspector and live routing consume the same bounded evidence. This
 * snapshot never reserves a slot or proves that a host will admit a request. */
typedef struct {
    LmbReplicaEvidence facts[LMB_ROUTE_REPLICAS];
    uint32_t order[LMB_ROUTE_REPLICAS];
    const char *reason;
    double captured_at;
    int inventory_ok;
} LmbRouteDecision;

static int api_route_capture(const char *tracker,const LmbModelRoute *route,LmbRouteDecision *decision) {
    memset(decision,0,sizeof *decision);decision->captured_at=(double)time(NULL);
    decision->reason=!route->count ? "model_dormant" : !route->policy ? "operator_order" : "inventory_unavailable";
    for (uint32_t i=0;i<route->count;i++) decision->order[i]=i;
    if (!route->count) return 0;
    LmbMachineReport reports[LMB_INVENTORY_MAX];uint32_t count=0;
    if (lmb_inventory_fetch(tracker,reports,&count)) return 0;
    decision->inventory_ok=1;
    LmbResidentPlan *plan=calloc(1,sizeof *plan);
    if (!plan) {decision->reason="observation_unavailable";return -1;}
    for (uint32_t i=0;i<route->count;i++)
        if (!api_find_plan(tracker,route->replicas[i].allocation,plan) && api_route_plan_matches(route,i,plan) &&
            api_replica_evidence(route,plan,reports,count,&decision->facts[i])) {
            /* Keep a matching saved plan eligible even when its measurements
             * are missing. Unknown must not look idle or inexpensive. */
            decision->facts[i]=(LmbReplicaEvidence){.usable=1};
        }
    free(plan);
    decision->captured_at=(double)time(NULL);
    decision->reason=lmb_replica_rank(route->policy,decision->facts,route->count,decision->captured_at,decision->order);
    return 0;
}
static void api_route_order(const char *tracker,const LmbModelRoute *route,
    uint32_t order[LMB_ROUTE_REPLICAS]) {
    for (uint32_t i=0;i<route->count;i++) order[i]=i;
    if (!route->policy || !route->count) return;
    LmbRouteDecision decision;
    (void)api_route_capture(tracker,route,&decision);
    memcpy(order,decision.order,route->count*sizeof *order);
    const char *reason=decision.reason;const LmbReplicaEvidence *facts=decision.facts;
    char model[65]; lmb_hex(model,route->id,32);
    fprintf(stderr,"[model-policy] model=%s policy=%s evidence=%s first_replica=%u\n",
        model,lmb_route_policy_name(route->policy),reason,order[0]+1);
    if (route->policy==LMB_ROUTE_LOAD_AWARE || route->policy==LMB_ROUTE_LATENCY_AWARE) {
        const LmbReplicaLoad *load=&facts[order[0]].load;
        fprintf(stderr,"[model-load] model=%s replica=%u known=%u nodes=%u max_pending=%u total_pending=%u age_ms=%u\n",
            model,order[0]+1,(unsigned)lmb_replica_load_valid(load),load->nodes,load->max_pending,
            load->total_pending,load->max_age_ms);
    }
    if (route->policy==LMB_ROUTE_LATENCY_AWARE) {
        const LmbReplicaEvidence *chosen=&facts[order[0]];
        int known=!strcmp(reason,"recent_workload_and_ttft_observations");
        /* A zero in this diagnostic is explicitly unknown. No fabricated
         * latency estimate is added to the catalogue or client response. */
        fprintf(stderr,"[model-latency] model=%s replica=%u known=%d observed_ttft_seconds=%.6f source=%s prompt_tokens=%u generated_tokens=%u\n",
            model,order[0]+1,known,known ? chosen->ttft_seconds : 0.,
            known ? lmb_cal_source_name(chosen->observation_source) : "unknown",
            known ? chosen->prompt_tokens : 0,known ? chosen->generated_tokens : 0);
    }
}
#endif
