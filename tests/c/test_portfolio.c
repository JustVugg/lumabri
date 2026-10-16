#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "src/planner/lumabri_portfolio.h"

static void alternative_subsets_and_residuals(void) {
    const uint64_t mib=UINT64_C(1)<<20;
    LmbModelShape shape={0}; strcpy(shape.model_type,"olmoe"); strcpy(shape.segment_id,"olmoe");
    shape.layers=4; shape.hidden=64; shape.vocab=128; shape.max_context=4096;
    shape.memory_contract=shape.sizing_verified=1; shape.edge_resident_bytes=8*mib;
    for (unsigned i=0;i<4;i++) shape.memory[i].resident_bytes=400*mib;
    uint64_t bytes=1608*mib;
    LmbPortfolioModel models[2]={{&shape,bytes,128,2},{&shape,bytes,128,2}};
    LmbHomeReservation whole,first,tail,left,right;
    assert(!lmb_home_reservation_sessions(&shape,bytes,0,4,128,2,1,&whole));
    assert(!lmb_home_reservation_sessions(&shape,bytes,0,1,128,2,1,&first));
    assert(!lmb_home_reservation_sessions(&shape,bytes,1,4,128,2,0,&tail));
    assert(!lmb_home_reservation_sessions(&shape,bytes,0,2,128,2,1,&left));
    assert(!lmb_home_reservation_sessions(&shape,bytes,2,4,128,2,0,&right));
    LmbPortfolioNode nodes[3]={0};
    for (unsigned i=0;i<3;i++) {
        nodes[i].node.threads=2;
        nodes[i].workload=(LmbWorkloadFacts){.known=1,.compute_policy=LMB_COMPUTE_LOCAL_FIFO,.allocation_set={1}};
        nodes[i].facts=(LmbResourceFacts){.known=LMB_FACT_PRICE,.price_micro_per_hour=100,.currency="EUR"};
    }
    nodes[0].node.ram_budget_bytes=2*whole.total_bytes; nodes[0].facts.price_micro_per_hour=10000;
    nodes[1].node.ram_budget_bytes=left.total_bytes; nodes[2].node.ram_budget_bytes=right.total_bytes;
    LmbPortfolioPlan plan;
    assert(!lmb_portfolio_plan(nodes,3,models,1,&plan));
    /* The affordable pair is not a descending-RAM prefix. */
    assert(plan.node_mask==6 && plan.machine_cost_micro_per_hour==200);
    assert(!plan.bounded);
    LmbPortfolioPlan again;
    assert(!lmb_portfolio_plan(nodes,3,models,1,&again));
    assert(!memcmp(&plan,&again,sizeof plan));

    nodes[0].node.ram_budget_bytes=whole.total_bytes+first.total_bytes;
    nodes[1].node.ram_budget_bytes=tail.total_bytes;
    nodes[0].facts.price_micro_per_hour=100;
    assert(!lmb_portfolio_plan(nodes,2,models,2,&plan));
    assert(plan.model_count==2 && plan.node_mask==3);
    uint64_t charged[2]={0};
    for (unsigned m=0;m<2;m++) {
        LmbClusterNode remaining[2]={nodes[0].node,nodes[1].node};
        for (unsigned n=0;n<2;n++) remaining[n].ram_budget_bytes-=charged[n];
        LmbClusterPlan checked=plan.plans[m];
        assert(!lmb_home_plan_budgets(&shape,bytes,remaining,2,128,&checked));
        assert(checked.state==LMB_PLAN_RESIDENT);
        for (unsigned i=0;i<checked.nslices;i++) charged[checked.slices[i].node]+=checked.slices[i].bytes_resident;
    }
    assert(charged[0]==plan.reserved[0] && charged[1]==plan.reserved[1]);
    assert(charged[0]<=nodes[0].node.ram_budget_bytes && charged[1]<=nodes[1].node.ram_budget_bytes);

    /* A missed candidate is not a feasibility proof. All 32 node bits and the
     * limited candidate family remain safe, deterministic and independently
     * checked, even with eight simultaneous model requests. */
    LmbPortfolioNode many[32];
    LmbPortfolioModel batch[8];
    for (unsigned n=0;n<32;n++) {
        many[n]=nodes[0]; many[n].node.ram_budget_bytes=8*whole.total_bytes;
        many[n].facts.price_micro_per_hour=n==31 ? 1 : 100+n;
    }
    for (unsigned m=0;m<8;m++) batch[m]=models[0];
    assert(!lmb_portfolio_plan(many,32,batch,8,&plan));
    assert(plan.bounded && plan.examined<=LMB_PORTFOLIO_SEARCH && plan.model_count==8);
    assert(plan.node_mask & (UINT32_C(1)<<31));
    for (unsigned n=0;n<32;n++) {
        assert(plan.added[n]<=4 && plan.reserved[n]<=many[n].node.ram_budget_bytes);
        many[n].node.ram_budget_bytes=1;
    }
    assert(lmb_portfolio_plan(many,32,batch,1,&plan)==1);
    assert(plan.bounded && !plan.model_count && !plan.node_mask);
    for (unsigned n=0;n<32;n++) assert(!plan.reserved[n] && !plan.added[n]);

    /* Changing models must never cause output indices to be swapped while
     * searching, since the preparation journal uses the original order. */
    nodes[0].node.ram_budget_bytes=4*whole.total_bytes;
    models[1].sessions=1;
    assert(!lmb_portfolio_plan(nodes,2,models,2,&plan));
    assert(plan.plans[0].sessions==2 && plan.plans[1].sessions==1);
    shape.layers=LMB_PLAN_LAYER_MAX+1;
    assert(lmb_portfolio_plan(nodes,2,models,2,&plan)==-1);
}

int main(void) {
    alternative_subsets_and_residuals();
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
    puts("PORTFOLIO: PASS (alternative subsets, residual cuts, bounded search, summed resident budgets, cost union, allocation cap, unknown/mixed prices)");
    return 0;
}
