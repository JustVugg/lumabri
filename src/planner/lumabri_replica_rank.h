/* Preferences among existing approved replicas, never a capacity certificate.
 * Missing/incomparable facts preserve the operator's explicit ordering. */
#ifndef LMB_REPLICA_RANK_H
#define LMB_REPLICA_RANK_H
#include "src/runtime/lumabri_model_routes.h"
#include "lumabri_replica_load.h"
#include "lumabri_calibration.h"
#include <math.h>

typedef struct {
    int usable, observation_current, price_known;
    double decode_tok_s, ttft_seconds, measured_at;
    uint32_t prompt_tokens, generated_tokens, observation_source;
    uint64_t micro_per_hour;
    char currency[4];
    LmbReplicaLoad load;
} LmbReplicaEvidence;

/* Timing is a preference over comparable completed observations, never a
 * prediction for the incoming prompt. Probe and conversation clocks must not
 * be mixed, and zero TTFT means missing evidence, not instant response. */
static inline const char *lmb_replica_timing_evidence(const LmbReplicaEvidence *e,
    uint32_t count, double now, int first_token) {
    uint32_t first=count;
    for (uint32_t i=0;i<count;i++) if (e[i].usable) {
        if (first==count) first=i;
        double value=first_token ? e[i].ttft_seconds : e[i].decode_tok_s;
        if (!isfinite(now) || !e[i].observation_current || !isfinite(value) ||
            value<=0 || !isfinite(e[i].measured_at) || e[i].measured_at<=0 ||
            now<e[i].measured_at || now-e[i].measured_at>300 ||
            !e[i].prompt_tokens || e[i].generated_tokens<(first_token ? 1u : 8u))
            return "observations_missing_or_expired";
        if (e[i].prompt_tokens!=e[first].prompt_tokens || e[i].generated_tokens!=e[first].generated_tokens)
            return "observation_lengths_differ";
        if (first_token && ((e[i].observation_source!=LMB_CAL_SOURCE_PROBE &&
                e[i].observation_source!=LMB_CAL_SOURCE_SESSION) ||
                e[i].observation_source!=e[first].observation_source))
            return "observation_sources_differ_or_unknown";
    }
    return NULL;
}

static inline const char *lmb_replica_rank(uint32_t policy, const LmbReplicaEvidence *e,
    uint32_t count, double now, uint32_t order[LMB_ROUTE_REPLICAS]) {
    for (uint32_t i=0;i<count && i<LMB_ROUTE_REPLICAS;i++) order[i]=i;
    if (!e || !count || count>LMB_ROUTE_REPLICAS || policy>LMB_ROUTE_POLICY_LAST) return "invalid_policy";
    if (!policy) return "operator_order";
    int latency_aware=policy==LMB_ROUTE_LATENCY_AWARE;
    int load_aware=policy==LMB_ROUTE_LOAD_AWARE || latency_aware;
    const char *decode_problem=lmb_replica_timing_evidence(e,count,now,0);
    int first_token=latency_aware && !lmb_replica_timing_evidence(e,count,now,1);
    if (policy==LMB_ROUTE_OBSERVED_DECODE && decode_problem) return decode_problem;
    int decode=policy==LMB_ROUTE_OBSERVED_DECODE || (policy==LMB_ROUTE_LOAD_AWARE && !decode_problem);
    uint32_t first=count;
    for (uint32_t i=0;i<count;i++) if (e[i].usable) {
        if (first==count) first=i;
        if (load_aware && !lmb_replica_load_valid(&e[i].load)) return "workload_missing_or_expired";
        if (policy==LMB_ROUTE_DECLARED_COST && (!e[i].price_known || !e[i].currency[0] || e[i].currency[3] ||
            memcmp(e[i].currency,e[first].currency,4))) return "prices_missing_or_mixed_currency";
    }
    if (first==count) return "no_approved_plan";
    /* Stable ordering: equal scores retain the requested order. A 5% timing
     * preference band avoids reversing near-equal replicas for small noise.
     * Sorting does not move weights or interrupt any active conversation. */
    uint32_t used=0;
    for (uint32_t at=0;at<count;at++) {
        uint32_t best=count;
        for (uint32_t i=0;i<count;i++) if (!(used&(1u<<i)) && e[i].usable) {
            if (best==count) { best=i; continue; }
            int load_order=load_aware ? lmb_replica_load_compare(&e[i].load,&e[best].load) : 0;
            if (load_order<0 || (!load_order &&
                (first_token ? e[i].ttft_seconds<e[best].ttft_seconds :
                 decode ? e[i].decode_tok_s>e[best].decode_tok_s :
                 policy==LMB_ROUTE_DECLARED_COST && e[i].micro_per_hour<e[best].micro_per_hour))) best=i;
        }
        if (best!=count && (decode || first_token)) {
            /* Compare with the best remaining score, not adjacent pairs:
             * a chain of 4% differences must not hide a 30% improvement. */
            for (uint32_t i=0;i<best;i++) if (!(used&(1u<<i)) && e[i].usable &&
                (!load_aware || !lmb_replica_load_compare(&e[i].load,&e[best].load)) &&
                (first_token ? e[i].ttft_seconds<=e[best].ttft_seconds*1.05 :
                 e[i].decode_tok_s>=e[best].decode_tok_s/1.05)) { best=i; break; }
        }
        if (best==count) for (uint32_t i=0;i<count;i++) if (!(used&(1u<<i))) { best=i; break; }
        order[at]=best; used|=1u<<best;
    }
    if (first_token) return "recent_workload_and_ttft_observations";
    if (load_aware) return decode ? "recent_workload_and_decode_observations" : "recent_workload_observations";
    return policy==LMB_ROUTE_OBSERVED_DECODE ? "recent_matching_decode_observations" : "declared_machine_footprint";
}
#endif
