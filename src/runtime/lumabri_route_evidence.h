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
        const LmbResourceFacts *facts=&found->facts;
        if (!(facts->known&LMB_FACT_PRICE) || !lmb_resource_facts_valid(facts)) out->price_known=0;
        else {
            if (!out->currency[0]) memcpy(out->currency,facts->currency,4);
            else if (memcmp(out->currency,facts->currency,4)) out->price_known=0;
            if (UINT64_MAX-out->micro_per_hour<facts->price_micro_per_hour) out->price_known=0;
            else out->micro_per_hour+=facts->price_micro_per_hour;
        }
    }
    seed.key.adapter_abi=route->numeric_abi;
    snprintf(seed.key.numeric_class,sizeof seed.key.numeric_class,"%s",route->numeric_class);
    LmbCalibration observed;
    if (!lmb_cal_profile_load(records,&seed.key,&observed)) {
        out->observation_current=1; out->decode_tok_s=observed.decode_tok_s;
        out->measured_at=observed.measured_at; out->prompt_tokens=observed.prompt_tokens;
        out->generated_tokens=observed.generated_tokens;
    }
    return 0;
}
static void api_route_order(const char *tracker, const LmbModelRoute *route,
    uint32_t order[LMB_ROUTE_REPLICAS]) {
    for (uint32_t i=0;i<route->count;i++) order[i]=i;
    if (!route->policy) return;
    LmbMachineReport reports[LMB_INVENTORY_MAX]; uint32_t count=0;
    const char *reason="inventory_unavailable"; LmbReplicaEvidence facts[LMB_ROUTE_REPLICAS]={0};
    if (!lmb_inventory_fetch(tracker,reports,&count)) {
        LmbResidentPlan *plan=calloc(1,sizeof *plan);
        if (!plan) return;
        for (uint32_t i=0;i<route->count;i++)
            if (!api_find_plan(tracker,route->replicas[i].allocation,plan) && api_route_plan_matches(route,i,plan) &&
                api_replica_evidence(route,plan,reports,count,&facts[i])) {
                /* Partial facts cannot drive either objective. Keep usable
                 * so unknown is not accidentally filtered as a cheap node. */
                facts[i]=(LmbReplicaEvidence){.usable=1};
            }
        free(plan);
        reason=lmb_replica_rank(route->policy,facts,route->count,(double)time(NULL),order);
    }
    char model[65]; lmb_hex(model,route->id,32);
    fprintf(stderr,"[model-policy] model=%s policy=%s evidence=%s first_replica=%u\n",
        model,lmb_route_policy_name(route->policy),reason,order[0]+1);
}
#endif
