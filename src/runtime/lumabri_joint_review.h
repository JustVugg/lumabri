/* Exact approved joint placement, reused by both native preparation owners. */
#ifndef LMB_JOINT_REVIEW_H
#define LMB_JOINT_REVIEW_H
_Static_assert(LMB_PORTFOLIO_MODELS<=HOME_PREPARATION_RESULTS,"every prepared model needs an exact operation result");

static int home_joint_validate(const LmbTuiState *reviewed, const LmbTuiState *current,
    uint32_t first, uint32_t mapping[LMB_CLUSTER_MAX_NODES], char *why, size_t cap) {
    const LmbPortfolioSnapshot *joint = &reviewed->joint;
#define JOINT_DENY(message) do { snprintf(why, cap, "%s", message); return -1; } while (0)
    if (!joint->ready || !joint->plan.model_count || joint->plan.model_count > LMB_PORTFOLIO_MODELS ||
        first >= joint->plan.model_count || !joint->node_count || joint->node_count > LMB_CLUSTER_MAX_NODES ||
        !current->inventory_ok || strcmp(reviewed->tracker, current->tracker))
        JOINT_DENY("The reviewed joint plan or household inventory is unavailable. Refresh the catalogue.");
    uint64_t needed[LMB_CLUSTER_MAX_NODES] = {0}; uint32_t added[LMB_CLUSTER_MAX_NODES] = {0};
    for (uint32_t m = first; m < joint->plan.model_count; m++) {
        const LmbClusterPlan *p = &joint->plan.plans[m];
        if (joint->model_indices[m] >= (uint32_t)reviewed->nmodels || p->state != LMB_PLAN_RESIDENT ||
            p->hybrid || p->sessions != reviewed->sessions || !p->nslices ||
            p->nslices > joint->node_count || p->edge_node >= joint->node_count)
            JOINT_DENY("Invalid resident Segment plan. Refresh before requesting approval.");
        uint32_t used = 0;
        for (uint32_t s = 0; s < p->nslices; s++) {
            const LmbSlice *slice = &p->slices[s];
            if (slice->node >= joint->node_count || !slice->bytes_resident ||
                (used & (UINT32_C(1) << slice->node))) JOINT_DENY("Invalid joint plan node assignment.");
            used |= UINT32_C(1) << slice->node;
            needed[slice->node] = lmb_size_add(needed[slice->node], slice->bytes_resident);
            added[slice->node]++;
        }
        if (!(used & (UINT32_C(1) << p->edge_node))) JOINT_DENY("Joint plan has no assigned chat host.");
    }
    for (uint32_t n = 0; n < joint->node_count; n++) {
        mapping[n] = UINT32_MAX;
        if (!added[n]) continue;
        uint32_t old = joint->node_indices[n];
        if (old >= reviewed->nnodes || !lmb_tui_node_enabled(reviewed, old) ||
            !reviewed->identities[old][0]) JOINT_DENY("A joint plan computer was not selected.");
        uint32_t now = UINT32_MAX;
        for (uint32_t i = 0; i < current->nnodes; i++)
            if (!strcmp(reviewed->identities[old], current->identities[i])) { now = i; break; }
        if (now == UINT32_MAX) JOINT_DENY("A planned computer is absent. Refresh the catalogue; no replacement is chosen silently.");
        mapping[n] = now;
        char previous_hardware[65], hardware[65];
        catalog_hardware_id(&reviewed->profiles[old], reviewed->nodes[old].addr, previous_hardware);
        catalog_hardware_id(&current->profiles[now], current->nodes[now].addr, hardware);
        if (!reviewed->runtime_ids[old][0] || strcmp(reviewed->runtime_ids[old], current->runtime_ids[now]) ||
            strcmp(previous_hardware, hardware) || reviewed->nodes[old].threads != current->nodes[now].threads)
            JOINT_DENY("A planned computer changed runtime, hardware, address or thread capacity. Review a new plan.");
        const LmbWorkloadFacts *before = &reviewed->workloads[old], *after = &current->workloads[now];
        if (!before->known || !after->known || after->compute_policy != LMB_COMPUTE_LOCAL_FIFO)
            JOINT_DENY("A planned computer has no current managed-allocation inventory.");
        /* Before the first OFFER nothing is ours: any allocation-set change
         * invalidates the review. Later steps necessarily add our own IDs;
         * donor admission remains the final atomic authority for each OFFER. */
        if (!first && (memcmp(before->allocation_set, after->allocation_set, 32) ||
            before->allocations != after->allocations || before->reserved_bytes != after->reserved_bytes))
            JOINT_DENY("Existing allocations changed after review. Refresh the joint plan before loading models.");
        if (after->allocations > 4 || added[n] > 4-after->allocations || needed[n] == UINT64_MAX ||
            needed[n] > current->nodes[now].ram_budget_bytes)
            JOINT_DENY("The remaining joint plan no longer fits available RAM or allocation slots. Ready models remain loaded.");
    }
#undef JOINT_DENY
    return 0;
}

static int home_joint_prepare_next(const LmbTuiState *reviewed, LmbTuiState *current,
    uint32_t model, char *why, size_t cap) {
    uint32_t map[LMB_CLUSTER_MAX_NODES];
    if (home_joint_validate(reviewed, current, model, map, why, cap)) return -1;
    const LmbPortfolioSnapshot *joint = &reviewed->joint;
    uint32_t index = joint->model_indices[model];
    current->models[index].plan = joint->plan.plans[model];
    current->models[index].planned = 1;
    LmbClusterPlan *plan = &current->models[index].plan;
    plan->edge_node = map[plan->edge_node];
    memset(current->selected_nodes, 0, sizeof current->selected_nodes);
    for (uint32_t i = 0; i < plan->nslices; i++) {
        plan->slices[i].node = map[plan->slices[i].node];
        memcpy(current->selected_nodes[i], current->identities[plan->slices[i].node], sizeof current->selected_nodes[i]);
    }
    return (int)index;
}

#endif
