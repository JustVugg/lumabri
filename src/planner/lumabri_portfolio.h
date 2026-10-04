/* Joint resident placement on an explicitly authorized node set. This is a
 * bounded candidate search, not an optimal solver or a throughput guarantee.
 * Offered RAM is already net of live reservations. Never subtract them twice.
 * No model is loaded and no approval is created by this module. */
#ifndef LUMABRI_PORTFOLIO_H
#define LUMABRI_PORTFOLIO_H
#include "lumabri_stage_placement.h"
#include "lumabri_resource_facts.h"
#include "lumabri_workload_facts.h"

#define LMB_PORTFOLIO_MODELS 8u
#define LMB_PORTFOLIO_CHOICES 64u
#define LMB_PORTFOLIO_SEARCH 20000u

typedef struct {
    const LmbModelShape *shape;
    uint64_t checkpoint_bytes;
    uint32_t context, sessions;
} LmbPortfolioModel;

typedef struct {
    LmbClusterNode node;
    LmbResourceFacts facts;
    LmbWorkloadFacts workload;
} LmbPortfolioNode;

typedef struct {
    LmbClusterPlan plans[LMB_PORTFOLIO_MODELS];
    uint64_t reserved[LMB_CLUSTER_MAX_NODES];
    uint32_t added[LMB_CLUSTER_MAX_NODES];
    uint32_t model_count, node_mask, examined, bounded, cost_objective;
    uint64_t machine_cost_micro_per_hour;
    char currency[4];
} LmbPortfolioPlan;

typedef struct {
    LmbClusterPlan plan;
    uint32_t mask;
    uint64_t memory[LMB_CLUSTER_MAX_NODES];
} LmbPortfolioChoice;

typedef struct {
    LmbPortfolioChoice choices[LMB_PORTFOLIO_MODELS][LMB_PORTFOLIO_CHOICES];
    uint32_t count[LMB_PORTFOLIO_MODELS], selected[LMB_PORTFOLIO_MODELS];
    const LmbPortfolioNode *nodes;
    uint32_t nnodes, nmodels, visited, bounded, cost_objective, found;
    uint64_t best_cost, best_memory;
    uint32_t best_nodes;
    LmbPortfolioPlan *result;
} LmbPortfolioSearch;

static inline uint32_t lmb_portfolio_popcount(uint32_t bits) {
    uint32_t count = 0;
    while (bits) { bits &= bits-1; count++; }
    return count;
}

static inline void lmb_portfolio_candidate(LmbPortfolioSearch *s, uint32_t model,
    const LmbPortfolioModel *m, uint32_t mask) {
    if (!mask || s->count[model] >= LMB_PORTFOLIO_CHOICES) return;
    for (uint32_t i = 0; i < s->count[model]; i++) if (s->choices[model][i].mask == mask) return;
    LmbClusterNode subset[LMB_CLUSTER_MAX_NODES]; uint32_t map[LMB_CLUSTER_MAX_NODES], n = 0;
    for (uint32_t i = 0; i < s->nnodes; i++) if (mask & (UINT32_C(1) << i)) {
        map[n] = i; subset[n++] = s->nodes[i].node;
    }
    if (n > m->shape->layers) return;
    LmbClusterPlan seed, planned;
    if (lmb_plan_cluster_source(m->shape, subset, n, m->context, m->sessions,
        LMB_GOAL_ONE_SESSION, 1, &seed) || lmb_home_plan_selected(m->shape,
        m->checkpoint_bytes, subset, n, m->context, &seed, NULL, &planned)) return;
    LmbPortfolioChoice *choice = &s->choices[model][s->count[model]];
    choice->plan = planned; choice->plan.edge_node = map[planned.edge_node]; choice->mask = mask;
    for (uint32_t i = 0; i < planned.nslices; i++) {
        uint32_t node = map[planned.slices[i].node];
        choice->plan.slices[i].node = node;
        choice->memory[node] = lmb_size_add(choice->memory[node], planned.slices[i].bytes_resident);
        if (choice->memory[node] == UINT64_MAX) { memset(choice, 0, sizeof *choice); return; }
    }
    s->count[model]++;
}

static inline void lmb_portfolio_search(LmbPortfolioSearch *s, uint32_t depth,
    uint32_t mask, uint64_t used[LMB_CLUSTER_MAX_NODES], uint32_t added[LMB_CLUSTER_MAX_NODES]) {
    if (s->visited >= LMB_PORTFOLIO_SEARCH) { s->bounded = 1; return; }
    s->visited++;
    uint64_t cost = 0, memory = 0;
    for (uint32_t i = 0; i < s->nnodes; i++) {
        memory = lmb_size_add(memory, used[i]);
        if (mask & (UINT32_C(1) << i)) cost += s->nodes[i].facts.price_micro_per_hour;
    }
    if (memory == UINT64_MAX) return;
    uint32_t machines = lmb_portfolio_popcount(mask);
    uint64_t score = s->cost_objective ? cost : 0;
    if (s->found && (score > s->best_cost ||
        (score == s->best_cost && machines > s->best_nodes))) return;
    if (depth == s->nmodels) {
        if (s->found && score == s->best_cost && machines == s->best_nodes && memory >= s->best_memory) return;
        s->found = 1; s->best_cost = score; s->best_nodes = machines; s->best_memory = memory;
        LmbPortfolioPlan *out = s->result;
        out->node_mask = mask; out->machine_cost_micro_per_hour = s->cost_objective ? cost : 0;
        memcpy(out->reserved, used, sizeof out->reserved); memcpy(out->added, added, sizeof out->added);
        for (uint32_t i = 0; i < s->nmodels; i++) out->plans[i] = s->choices[i][s->selected[i]].plan;
        return;
    }
    for (uint32_t c = 0; c < s->count[depth] && !s->bounded; c++) {
        const LmbPortfolioChoice *choice = &s->choices[depth][c];
        int fits = 1;
        for (uint32_t i = 0; i < s->nnodes; i++) if (choice->mask & (UINT32_C(1) << i)) {
            if (s->nodes[i].workload.allocations + added[i] >= 4 ||
                used[i] > s->nodes[i].node.ram_budget_bytes ||
                choice->memory[i] > s->nodes[i].node.ram_budget_bytes-used[i]) { fits = 0; break; }
        }
        if (!fits) continue;
        for (uint32_t i = 0; i < s->nnodes; i++) if (choice->mask & (UINT32_C(1) << i)) {
            used[i] += choice->memory[i]; added[i]++;
        }
        s->selected[depth] = c;
        lmb_portfolio_search(s, depth+1, mask | choice->mask, used, added);
        for (uint32_t i = 0; i < s->nnodes; i++) if (choice->mask & (UINT32_C(1) << i)) {
            used[i] -= choice->memory[i]; added[i]--;
        }
    }
}

/* Candidate family: every eligible single node plus descending-RAM prefixes.
 * Cross-model search then checks SUMMED memory and allocation limits. Missing
 * or mixed-currency prices disable the cost objective globally; no unknown
 * node is treated as free. The caller must revalidate and obtain approvals. */
static inline int lmb_portfolio_plan(const LmbPortfolioNode *nodes, uint32_t nnodes,
    const LmbPortfolioModel *models, uint32_t nmodels, LmbPortfolioPlan *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!nodes || !models || !nnodes || nnodes > LMB_CLUSTER_MAX_NODES ||
        !nmodels || nmodels > LMB_PORTFOLIO_MODELS) return -1;
    LmbPortfolioSearch *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->nodes = nodes; s->nnodes = nnodes; s->nmodels = nmodels; s->result = out; s->cost_objective = 1;
    uint32_t order[LMB_CLUSTER_MAX_NODES], eligible = 0;
    for (uint32_t i = 0; i < nnodes; i++) {
        const LmbPortfolioNode *n = &nodes[i];
        if (!lmb_resource_facts_valid(&n->facts) || !lmb_workload_valid(&n->workload, UINT64_MAX)) { free(s); return -1; }
        if (!n->workload.known || n->workload.compute_policy != LMB_COMPUTE_LOCAL_FIFO ||
            n->workload.allocations >= 4 || !n->node.ram_budget_bytes || !n->node.threads) continue;
        if (!(n->facts.known & LMB_FACT_PRICE)) s->cost_objective = 0;
        else if (!out->currency[0]) memcpy(out->currency, n->facts.currency, 4);
        else if (memcmp(out->currency, n->facts.currency, 4)) s->cost_objective = 0;
        uint32_t at = eligible++;
        while (at && nodes[order[at-1]].node.ram_budget_bytes < n->node.ram_budget_bytes) {
            order[at] = order[at-1]; at--;
        }
        order[at] = i;
    }
    if (!s->cost_objective) memset(out->currency, 0, sizeof out->currency);
    for (uint32_t m = 0; m < nmodels; m++) {
        if (!models[m].shape || !models[m].checkpoint_bytes || !models[m].context ||
            !models[m].sessions || models[m].sessions > LMB_HOST_MAX_SESSIONS) { free(s); return -1; }
        for (uint32_t i = 0; i < eligible; i++) lmb_portfolio_candidate(s, m, &models[m], UINT32_C(1) << order[i]);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < eligible; i++) {
            mask |= UINT32_C(1) << order[i];
            if (i) lmb_portfolio_candidate(s, m, &models[m], mask);
        }
        if (!s->count[m]) { free(s); return 1; }
    }
    uint64_t used[LMB_CLUSTER_MAX_NODES] = {0}; uint32_t added[LMB_CLUSTER_MAX_NODES] = {0};
    lmb_portfolio_search(s, 0, 0, used, added);
    out->model_count = s->found ? nmodels : 0; out->examined = s->visited; out->bounded = s->bounded;
    out->cost_objective = s->cost_objective;
    int rc = s->found ? 0 : 1; free(s); return rc;
}
#endif
