/* Bounded policy over whole measured portfolios. No composition of isolated
 * model speeds, new allocation authority or cloud spending. Cost is the
 * declared machine footprint, not a bill or realized savings. */
#ifndef LMB_PORTFOLIO_POLICY_H
#define LMB_PORTFOLIO_POLICY_H
#include "lumabri_capacity.h"
#include "src/runtime/lumabri_api_access.h"

typedef struct {
    uint32_t revision,enabled,models,candidates,current;
    uint32_t ttft_ms,gap_ms,max_age,cooldown,horizon,min_saving_bps;
    uint64_t ceiling_micro_per_hour,switch_cost_micro;
    char currency[4],tracker[256],names[LMB_CAPACITY_MODELS][33];
    uint8_t records[LMB_CAPACITY_MODELS][32],model_ids[LMB_CAPACITY_MODELS][32];
    uint8_t routes[LMB_CAPACITY_MODELS][32],targets[LMB_CAPACITY_MODELS][32];
    uint32_t route_revisions[LMB_CAPACITY_MODELS];
    uint64_t last_change,last_check;
    uint32_t pending,faulted; /* pending candidate index + 1; zero is none */
    char reason[64];
} LmbPortfolioPolicy;

static inline int lmb_portfolio_policy_valid(const LmbPortfolioPolicy *p) {
    if (!p || !p->revision || p->enabled>1 || !p->models || p->models>LMB_CAPACITY_MODELS ||
        p->candidates<2 || p->candidates>LMB_CAPACITY_MODELS || p->current>=p->candidates ||
        !p->ttft_ms || p->ttft_ms>3600000 || !p->gap_ms || p->gap_ms>3600000 ||
        !p->max_age || p->max_age>86400 || p->cooldown<30 || p->cooldown>86400 ||
        !p->horizon || p->horizon>86400 || p->min_saving_bps>10000 ||
        p->ceiling_micro_per_hour>UINT64_C(32000000000000) ||
        p->switch_cost_micro>UINT64_C(768000000000000) ||
        p->pending>p->candidates || p->faulted>1 ||
        !lmb_cal_text(p->tracker,sizeof p->tracker) || !lmb_cal_text(p->reason,sizeof p->reason)) return 0;
    if (p->currency[3]) return 0;
    for (unsigned i=0;i<3;i++) if (p->currency[i]<'A' || p->currency[i]>'Z') return 0;
    uint8_t zero[32]={0};
    for (uint32_t i=0;i<p->models;i++) {
        if (!p->route_revisions[i] || !memcmp(p->model_ids[i],zero,32) ||
            !memcmp(p->routes[i],zero,32) || (p->pending && !memcmp(p->targets[i],zero,32))) return 0;
        for (uint32_t j=0;j<i;j++) if (!memcmp(p->model_ids[i],p->model_ids[j],32)) return 0;
    }
    for (uint32_t i=0;i<p->candidates;i++) {
        if (!lmb_api_username(p->names[i]) || !memcmp(p->records[i],zero,32)) return 0;
        for (uint32_t j=0;j<i;j++) if (!strcmp(p->names[i],p->names[j])) return 0;
    }
    return 1;
}
static inline int lmb_portfolio_policy_price(const LmbPortfolioPolicy *p,const LmbCapacityCost *c) {
    return c->known && !memcmp(c->currency,p->currency,4) && c->micro_per_hour<=p->ceiling_micro_per_hour;
}
/* Explicit cooldown also covers failure recovery: this controller never
 * advertises instant HA. Existing route-level replay remains independent. */
static inline int lmb_portfolio_policy_choose(const LmbPortfolioPolicy *p,const int *passing,
    const LmbCapacityCost *costs,uint64_t now,const char **reason) {
    *reason="invalid_policy";
    if (!lmb_portfolio_policy_valid(p)) return -1;
    if (!p->enabled) { *reason="disabled"; return -1; }
    if (p->faulted) { *reason="manual_reconfiguration_required"; return -1; }
    if (p->pending) { *reason="reconciling_previous_change"; return -1; }
    if (now<p->last_change || now<p->last_check) { *reason="clock_moved_backwards"; return -1; }
    if (p->last_change && now-p->last_change<p->cooldown) { *reason="cooldown"; return -1; }
    int best=-1;
    for (uint32_t i=0;i<p->candidates;i++) if (passing[i] && lmb_portfolio_policy_price(p,&costs[i])) {
        if (best<0 || costs[i].micro_per_hour<costs[best].micro_per_hour ||
            (costs[i].micro_per_hour==costs[best].micro_per_hour && i==p->current)) best=(int)i;
    }
    if (best<0) { *reason="no_measured_candidate_within_budget"; return -1; }
    if ((uint32_t)best==p->current) { *reason="current_portfolio_preferred"; return -1; }
    if (!passing[p->current] || !lmb_portfolio_policy_price(p,&costs[p->current])) {
        *reason="restore_observed_requirements"; return best;
    }
    uint64_t old=costs[p->current].micro_per_hour,new_cost=costs[best].micro_per_hour;
    if (new_cost>=old) { *reason="no_cost_improvement"; return -1; }
    uint64_t saving=old-new_cost;
    /* Values are bounded so these products fit uint64. Rounding never
     * overstates a projected benefit. Equality does not pay the switch cost. */
    if (saving*10000<old*p->min_saving_bps) { *reason="saving_below_hysteresis"; return -1; }
    uint64_t benefit=(saving/3600)*p->horizon+(saving%3600)*p->horizon/3600;
    if (benefit<=p->switch_cost_micro) { *reason="switch_cost_not_recovered"; return -1; }
    *reason="lower_declared_cost_within_observed_limits"; return best;
}
#endif
