/* Read-only joint-plan preview over the SAME catalogue/inventory snapshot.
 * Application and approvals remain separate. No synthetic calibration. */
#ifndef LUMABRI_PORTFOLIO_CATALOGUE_H
#define LUMABRI_PORTFOLIO_CATALOGUE_H
#include "lumabri_portfolio.h"

static int catalog_portfolio_build(const LmbTuiState *st, const char *const *names, uint32_t count,
    LmbPortfolioSnapshot *out) {
    memset(out, 0, sizeof *out);
    LmbPortfolioNode nodes[LMB_CLUSTER_MAX_NODES] = {0}; uint32_t nnodes = 0;
    LmbPortfolioModel models[LMB_PORTFOLIO_MODELS] = {0};
#define JOINT_FAIL(text) do { snprintf(out->reason, sizeof out->reason, "%s", text); return 2; } while (0)
    if (!st->inventory_ok || !count || count > LMB_PORTFOLIO_MODELS) JOINT_FAIL("Select models and refresh household inventory.");
    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++) if (st->selected_nodes[i][0]) {
        int present = 0;
        for (uint32_t j = 0; j < st->nnodes; j++) if (!strcmp(st->selected_nodes[i], st->identities[j])) present = 1;
        if (!present) JOINT_FAIL("A selected computer is absent from the leased inventory.");
    }
    for (uint32_t i = 0; i < st->nnodes; i++) if (lmb_tui_node_enabled(st, i)) {
        nodes[nnodes] = (LmbPortfolioNode){st->nodes[i], st->facts[i], st->workloads[i]};
        /* CAS presence is not yet a per-model fact in machine inventory. */
        nodes[nnodes].node.has_checkpoint = 0;
        out->node_indices[nnodes++] = i;
    }
    out->node_count = nnodes;
    if (!nnodes) JOINT_FAIL("Choose authorized computers; no computer is selected implicitly.");
    for (uint32_t m = 0; m < count; m++) {
        int found = -1;
        for (uint32_t j = 0; j < m; j++) if (!strcmp(names[m], names[j])) {
            JOINT_FAIL("Duplicate model selection.");
        }
        for (int i = 0; i < st->nmodels; i++) if (!strcmp(names[m], st->models[i].name)) { found = i; break; }
        if (found < 0) JOINT_FAIL("A requested model is absent from the local catalogue.");
        const LmbTuiModel *model = &st->models[found]; out->model_indices[m] = (uint32_t)found;
        if (!model->weights_present || !model->checkpoint_inventory_ok || !model->shape.sizing_verified) {
            JOINT_FAIL("A requested checkpoint lacks validated resident sizing or source weights.");
        }
        models[m] = (LmbPortfolioModel){&model->shape, model->checkpoint_bytes, st->context, st->sessions};
    }
    int rc = lmb_portfolio_plan(nodes, nnodes, models, count, &out->plan);
    if (rc < 0) JOINT_FAIL("Invalid joint planning inputs.");
    out->ready = !rc;
    snprintf(out->reason, sizeof out->reason, "%s", rc ? "No joint resident candidate fits the known donor budgets." :
        "Resident candidate. Each model requires donor approval; performance is not yet calibrated.");
#undef JOINT_FAIL
    return rc;
}

static void catalog_joint_refresh(LmbTuiState *st) {
    memset(&st->joint, 0, sizeof st->joint);
    const char *names[LMB_PORTFOLIO_MODELS]; uint32_t count = 0;
    for (uint32_t i = 0; i < LMB_PORTFOLIO_MODELS; i++) if (st->selected_models[i][0]) {
        int found = -1;
        for (int j = 0; j < st->nmodels; j++) if (!strcmp(st->selected_models[i], st->models[j].dir)) { found = j; break; }
        if (found < 0) {
            snprintf(st->joint.reason, sizeof st->joint.reason, "A selected checkpoint disappeared. Review the model selection."); return;
        }
        names[count++] = st->models[found].name;
    }
    if (count) (void)catalog_portfolio_build(st, names, count, &st->joint);
}

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
