/* Read-only joint-plan preview over the SAME catalogue/inventory snapshot.
 * Application and approvals remain separate. No synthetic calibration. */
#ifndef LUMABRI_PORTFOLIO_CATALOGUE_H
#define LUMABRI_PORTFOLIO_CATALOGUE_H
#include "lumabri_portfolio.h"

#include "lumabri_portfolio_snapshot.h"

static int catalog_portfolio_json(const LmbTuiState *st, const char *const *names, uint32_t count) {
    LmbPortfolioSnapshot snapshot;
    int rc = catalog_portfolio_build(st, names, count, &snapshot);
    if (rc == 2) { fprintf(stderr, "%s\n", snapshot.reason); return rc; }
    const LmbPortfolioPlan plan = snapshot.plan;
    uint32_t nnodes = snapshot.node_count;
    printf("{\"schema\":1,\"mode\":\"preview\",\"state\":\"%s\",\"requires_approval\":true,"
           "\"performance_validated\":false,\"decode_tok_s\":null,\"search_bounded\":%s,\"examined\":%u,"
           "\"objective\":\"%s\",\"declared_machine_cost\":",
           rc ? "no_candidate" : "joint_resident_candidate", plan.bounded ? "true" : "false", plan.examined,
           plan.cost_objective ? "declared_hourly_cost" : "resident_feasibility");
    if (!rc && plan.cost_objective) printf("{\"micro_units_per_hour\":%llu,\"currency\":\"%.3s\"}",
        (unsigned long long)plan.machine_cost_micro_per_hour, plan.currency);
    else fputs("null", stdout);
    fputs(",\"nodes\":[", stdout);
    for (uint32_t i = 0; i < nnodes; i++) {
        uint32_t original = snapshot.node_indices[i]; if (i) fputc(',', stdout);
        const LmbWorkloadFacts *workload = &st->workloads[original];
        fputs("{\"identity\":", stdout); json_string(stdout, st->identities[original]);
        fputs(",\"runtime_id\":", stdout); json_string(stdout, st->runtime_ids[original]);
        char set[65]; lmb_hex(set, workload->allocation_set, 32);
        fputs(",\"allocation_set\":", stdout); json_string(stdout, workload->known ? set : "");
        fputs(",\"existing_allocations\":", stdout);
        if (workload->known) printf("%u", workload->allocations); else fputs("null", stdout);
        printf(",\"offered_ram_bytes\":%llu,\"added_allocations\":%u,\"added_reserved_bytes\":%llu}",
            (unsigned long long)st->nodes[original].ram_budget_bytes,
            plan.added[i], (unsigned long long)plan.reserved[i]);
    }
    fputs("],\"models\":[", stdout);
    for (uint32_t m = 0; m < plan.model_count; m++) {
        const LmbClusterPlan *p = &plan.plans[m]; if (m) fputc(',', stdout);
        fputs("{\"name\":", stdout); json_string(stdout, names[m]);
        fputs(",\"checkpoint_id\":", stdout); json_string(stdout, st->models[snapshot.model_indices[m]].content_id);
        printf(",\"context\":%u,\"slots\":%u,\"edge_node\":%u,\"slices\":[", st->context, st->sessions, p->edge_node);
        for (uint32_t i = 0; i < p->nslices; i++) {
            const LmbSlice *slice = &p->slices[i]; if (i) fputc(',', stdout);
            printf("{\"node\":%u,\"begin\":%u,\"end\":%u,\"reserved_bytes\":%llu}",
                slice->node, slice->layer_begin, slice->layer_end, (unsigned long long)slice->bytes_resident);
        }
        fputs("]}", stdout);
    }
    fputs("]}\n", stdout); return rc;
}
#endif
