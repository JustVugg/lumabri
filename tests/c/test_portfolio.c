#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "src/planner/lumabri_portfolio.h"

int main(void) {
    const uint64_t mib = UINT64_C(1) << 20;
    LmbModelShape shape = {0}; strcpy(shape.model_type, "olmoe"); strcpy(shape.segment_id, "olmoe");
    shape.layers = 4; shape.hidden = 64; shape.vocab = 128; shape.max_context = 4096;
    shape.memory_contract = shape.sizing_verified = 1; shape.edge_resident_bytes = 8*mib;
    for (unsigned i = 0; i < 4; i++) shape.memory[i].resident_bytes = 100*mib;
    LmbPortfolioModel models[5];
    for (unsigned i = 0; i < 5; i++) models[i] = (LmbPortfolioModel){&shape, 408*mib, 128, 2};
    LmbHomeReservation whole;
    assert(!lmb_home_reservation_sessions(&shape, 408*mib, 0, 4, 128, 2, 1, &whole));
    LmbPortfolioNode nodes[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        nodes[i].node.ram_budget_bytes = 2*whole.total_bytes; nodes[i].node.threads = 2;
        nodes[i].workload = (LmbWorkloadFacts){.known=1,.compute_policy=LMB_COMPUTE_LOCAL_FIFO,.allocation_set={1}};
        nodes[i].facts = (LmbResourceFacts){.known=LMB_FACT_PRICE,.price_micro_per_hour=(i+1)*100,.currency="EUR"};
    }
    LmbPortfolioPlan plan;
    assert(!lmb_portfolio_plan(nodes, 3, models, 2, &plan));
    assert(plan.node_mask == 1 && plan.added[0] == 2 && plan.reserved[0] == 2*whole.total_bytes);
    assert(plan.cost_objective && plan.machine_cost_micro_per_hour == 100); /* not per-model double billing */
    nodes[0].node.ram_budget_bytes--; /* both fit alone, but not together */
    assert(!lmb_portfolio_plan(nodes, 3, models, 2, &plan));
    assert(plan.node_mask == 2 && plan.machine_cost_micro_per_hour == 200);
    nodes[1].node.ram_budget_bytes = nodes[2].node.ram_budget_bytes = whole.total_bytes;
    assert(!lmb_portfolio_plan(nodes, 3, models, 2, &plan) && lmb_portfolio_popcount(plan.node_mask) == 2);
    for (unsigned i = 0; i < 3; i++) assert(plan.reserved[i] <= nodes[i].node.ram_budget_bytes);
    nodes[0].node.ram_budget_bytes = 8*whole.total_bytes;
    nodes[0].workload.allocations = 3; nodes[0].workload.reserved_bytes = 1;
    assert(!lmb_portfolio_plan(nodes, 3, models, 2, &plan));
    assert(plan.added[0] <= 1 && plan.added[1]+plan.added[2] >= 1);
    nodes[0].workload.allocations = 0; nodes[0].workload.reserved_bytes = 0;
    assert(!lmb_portfolio_plan(nodes, 3, models, 5, &plan));
    assert(plan.added[0] <= 4 && plan.added[1]+plan.added[2] >= 1);
    nodes[1].facts = (LmbResourceFacts){0};
    assert(!lmb_portfolio_plan(nodes, 3, models, 2, &plan) && !plan.cost_objective && !plan.currency[0]);
    nodes[1].facts = (LmbResourceFacts){.known=LMB_FACT_PRICE,.price_micro_per_hour=1,.currency="USD"};
    assert(!lmb_portfolio_plan(nodes, 3, models, 2, &plan) && !plan.cost_objective);
    for (unsigned i = 0; i < 3; i++) nodes[i].workload = (LmbWorkloadFacts){0};
    assert(lmb_portfolio_plan(nodes, 3, models, 2, &plan) == 1 && !plan.model_count);
    nodes[0].workload.known = 2;
    assert(lmb_portfolio_plan(nodes, 3, models, 2, &plan) == -1);
    assert(lmb_portfolio_plan(nodes, 33, models, 2, &plan) == -1);
    assert(lmb_portfolio_plan(nodes, 3, models, 9, &plan) == -1);
    puts("PORTFOLIO: PASS (summed resident budgets, cost union, allocation cap, unknown/mixed prices, no invented compute)");
    return 0;
}
