/* Read-only joint-plan preview over the SAME catalogue/inventory snapshot.
 * Application and approvals remain separate. No synthetic calibration. */
#ifndef LUMABRI_PORTFOLIO_CATALOGUE_H
#define LUMABRI_PORTFOLIO_CATALOGUE_H
#include "lumabri_portfolio.h"

static int catalog_portfolio_json(const LmbTuiState *st, const char *const *names, uint32_t count) {
    LmbPortfolioNode nodes[LMB_CLUSTER_MAX_NODES] = {0}; uint32_t mapping[LMB_CLUSTER_MAX_NODES], nnodes = 0;
    LmbPortfolioModel models[LMB_PORTFOLIO_MODELS] = {0}; uint32_t model_ids[LMB_PORTFOLIO_MODELS];
    if (!st->inventory_ok || !count || count > LMB_PORTFOLIO_MODELS) return 2;
    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++) if (st->selected_nodes[i][0]) {
        int present = 0;
        for (uint32_t j = 0; j < st->nnodes; j++) if (!strcmp(st->selected_nodes[i], st->identities[j])) present = 1;
        if (!present) { fprintf(stderr, "A selected computer is absent from the leased inventory.\n"); return 2; }
    }
    for (uint32_t i = 0; i < st->nnodes; i++) if (lmb_tui_node_enabled(st, i)) {
        nodes[nnodes] = (LmbPortfolioNode){st->nodes[i], st->facts[i], st->workloads[i]};
        /* CAS presence is not yet a per-model fact in machine inventory. */
        nodes[nnodes].node.has_checkpoint = 0;
        mapping[nnodes++] = i;
    }
    if (!nnodes) { fprintf(stderr, "Choose authorized computers with --node ID; no computer is selected implicitly.\n"); return 2; }
    for (uint32_t m = 0; m < count; m++) {
        int found = -1;
        for (uint32_t j = 0; j < m; j++) if (!strcmp(names[m], names[j])) {
            fprintf(stderr, "Duplicate model selection.\n"); return 2;
        }
        for (int i = 0; i < st->nmodels; i++) if (!strcmp(names[m], st->models[i].name)) { found = i; break; }
        if (found < 0) { fprintf(stderr, "A requested model is absent from the local catalogue.\n"); return 2; }
        const LmbTuiModel *model = &st->models[found]; model_ids[m] = (uint32_t)found;
        if (!model->weights_present || !model->checkpoint_inventory_ok || !model->shape.sizing_verified) {
            fprintf(stderr, "A requested checkpoint lacks validated resident sizing or source weights.\n"); return 2;
        }
        models[m] = (LmbPortfolioModel){&model->shape, model->checkpoint_bytes, st->context, st->sessions};
    }
    LmbPortfolioPlan plan;
    int rc = lmb_portfolio_plan(nodes, nnodes, models, count, &plan);
    if (rc < 0) { fprintf(stderr, "Invalid joint planning inputs.\n"); return 2; }
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
        uint32_t original = mapping[i]; if (i) fputc(',', stdout);
        fputs("{\"identity\":", stdout); json_string(stdout, st->identities[original]);
        fputs(",\"runtime_id\":", stdout); json_string(stdout, st->runtime_ids[original]);
        char set[65]; lmb_hex(set, nodes[i].workload.allocation_set, 32);
        fputs(",\"allocation_set\":", stdout); json_string(stdout, nodes[i].workload.known ? set : "");
        fputs(",\"existing_allocations\":", stdout);
        if (nodes[i].workload.known) printf("%u", nodes[i].workload.allocations); else fputs("null", stdout);
        printf(",\"offered_ram_bytes\":%llu,\"added_allocations\":%u,\"added_reserved_bytes\":%llu}",
            (unsigned long long)nodes[i].node.ram_budget_bytes,
            plan.added[i], (unsigned long long)plan.reserved[i]);
    }
    fputs("],\"models\":[", stdout);
    for (uint32_t m = 0; m < plan.model_count; m++) {
        const LmbClusterPlan *p = &plan.plans[m]; if (m) fputc(',', stdout);
        fputs("{\"name\":", stdout); json_string(stdout, names[m]);
        fputs(",\"checkpoint_id\":", stdout); json_string(stdout, st->models[model_ids[m]].content_id);
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
