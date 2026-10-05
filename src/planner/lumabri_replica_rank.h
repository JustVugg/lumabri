/* Preferences among existing approved replicas, never a capacity certificate.
 * Missing/incomparable facts preserve the operator's explicit ordering. */
#ifndef LMB_REPLICA_RANK_H
#define LMB_REPLICA_RANK_H
#include "src/runtime/lumabri_model_routes.h"
#include <math.h>

typedef struct {
    int usable, observation_current, price_known;
    double decode_tok_s, measured_at;
    uint32_t prompt_tokens, generated_tokens;
    uint64_t micro_per_hour;
    char currency[4];
} LmbReplicaEvidence;

static inline const char *lmb_replica_rank(uint32_t policy, const LmbReplicaEvidence *e,
    uint32_t count, double now, uint32_t order[LMB_ROUTE_REPLICAS]) {
    for (uint32_t i=0;i<count && i<LMB_ROUTE_REPLICAS;i++) order[i]=i;
    if (!count || count>LMB_ROUTE_REPLICAS || policy>LMB_ROUTE_DECLARED_COST) return "invalid_policy";
    if (!policy) return "operator_order";
    uint32_t first=count;
    for (uint32_t i=0;i<count;i++) if (e[i].usable) {
        if (first==count) first=i;
        if (policy==LMB_ROUTE_OBSERVED_DECODE) {
            if (!isfinite(now) || !e[i].observation_current || !isfinite(e[i].decode_tok_s) ||
                e[i].decode_tok_s<=0 || !isfinite(e[i].measured_at) || e[i].measured_at<=0 ||
                now<e[i].measured_at || now-e[i].measured_at>300 ||
                !e[i].prompt_tokens || e[i].generated_tokens<8) return "observations_missing_or_expired";
            if (e[i].prompt_tokens!=e[first].prompt_tokens || e[i].generated_tokens!=e[first].generated_tokens)
                return "observation_lengths_differ";
        } else if (!e[i].price_known || !e[i].currency[0] || e[i].currency[3] ||
            memcmp(e[i].currency,e[first].currency,4)) return "prices_missing_or_mixed_currency";
    }
    if (first==count) return "no_approved_plan";
    /* Stable ordering: equal scores retain the requested order. A 5% decode
     * preference band avoids reversing near-equal replicas for small noise.
     * Sorting does not move weights or interrupt any active conversation. */
    uint32_t used=0;
    for (uint32_t at=0;at<count;at++) {
        uint32_t best=count;
        for (uint32_t i=0;i<count;i++) if (!(used&(1u<<i)) && e[i].usable &&
            (best==count || (policy==LMB_ROUTE_OBSERVED_DECODE ? e[i].decode_tok_s>e[best].decode_tok_s :
                e[i].micro_per_hour<e[best].micro_per_hour))) best=i;
        if (best!=count && policy==LMB_ROUTE_OBSERVED_DECODE) {
            /* Compare with the best remaining score, not adjacent pairs:
             * a chain of 4% differences must not hide a 30% improvement. */
            for (uint32_t i=0;i<best;i++) if (!(used&(1u<<i)) && e[i].usable &&
                e[i].decode_tok_s>=e[best].decode_tok_s/1.05) { best=i; break; }
        }
        if (best==count) for (uint32_t i=0;i<count;i++) if (!(used&(1u<<i))) { best=i; break; }
        order[at]=best; used|=1u<<best;
    }
    return policy==LMB_ROUTE_OBSERVED_DECODE ? "recent_matching_decode_observations" : "declared_machine_footprint";
}
#endif
