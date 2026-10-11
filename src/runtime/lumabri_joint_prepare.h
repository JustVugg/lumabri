/* Apply a reviewed joint plan through the existing per-model keeper workflow.
 * This is deliberately a sequence, not a distributed atomic transaction:
 * READY models survive rejection/failure of a later model. No approval replay. */
#ifndef LUMABRI_JOINT_PREPARE_H
#define LUMABRI_JOINT_PREPARE_H

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

static int home_prepare_portfolio_begin(const LmbTuiState *reviewed,
    const HomePreparationObserver *observer, HomeServiceSnapshot *started) {
    home_error[0] = 0;
    if (home_service_foreground() || !home_resident_required())
        return home_fail("Joint preparation requires the resident background service.");
    g_stopping = 0; install_chat_signal_handlers();
    if (!reviewed->tracker[0] || home_private_network()) return home_fail("Joint preparation needs an authenticated household.");
    LmbTuiState *current = malloc(sizeof *current);
    if (!current) return home_fail("Cannot allocate the preparation snapshot.");
    memcpy(current, reviewed, sizeof *current);
    uint32_t mapping[LMB_CLUSTER_MAX_NODES]; char why[384];
    int inventory = catalog_inventory(current);
    if (inventory || home_joint_validate(reviewed, current, 0, mapping, why, sizeof why)) {
        free(current); return home_fail("%s", inventory ? "Cannot refresh household inventory; no model was loaded." : why);
    }
    if (home_service_ensure()) { free(current); return home_fail("Cannot start the private preparation service."); }
    HomeService job;
    if (home_service_open(&job, "prepare")) {
        free(current); return home_fail("Another preparation is running. No second operation was requested.");
    }
    if (observer) {
        job.observer = *observer;
        memcpy(job.snapshot.instance, observer->instance, 32);
    }
    job.batch_total = reviewed->joint.plan.model_count;
    snprintf(job.snapshot.name, sizeof job.snapshot.name, "Joint resident plan");
    snprintf(job.snapshot.tracker, sizeof job.snapshot.tracker, "%s", reviewed->tracker);
    home_service_preparation_detail(&job, "Preparing reviewed models; separate donor approvals required");
    if (home_service_save(&job)) {
        home_service_close(&job); free(current); return home_fail("Cannot persist the joint preparation operation.");
    }
    int detached = home_service_detach(&job, observer ? observer->fd : -1);
    if (detached < 0) { home_service_close(&job); free(current); return home_fail("Cannot start joint preparation keeper."); }
    if (!detached) {
        home_background_job = &job;
        int rc = 0;
        for (uint32_t model = 0; model < job.batch_total; model++) {
            if (g_stopping || home_service_key() == 3) { rc = home_fail("Preparation cancelled before the next model."); break; }
            memcpy(current, reviewed, sizeof *current);
            if (catalog_inventory(current)) { rc = home_fail("Cannot refresh the household before the next model."); break; }
            int index = home_joint_prepare_next(reviewed, current, model, why, sizeof why);
            if (index < 0) { rc = home_fail("%s", why); break; }
            current->quick_calibration = 0;
            rc = home_request_chat_direct(current, index);
            if (rc) break;
            job.batch_completed++;
            home_service_preparation_detail(&job, "Approved model ready; its allocation is saved in Resident models");
            if (home_service_save(&job)) { rc = home_fail("Cannot persist joint preparation progress."); break; }
        }
        job.snapshot.state = rc ? HOME_SVC_FAILED : HOME_SVC_DONE;
        char outcome[480];
        if (rc) snprintf(outcome, sizeof outcome,
            "%.300s Ready models remain loaded; incomplete allocations cancelled. See Resident models.", home_error);
        else snprintf(outcome, sizeof outcome, "All selected models ready. Open conversations from Resident models.");
        home_service_preparation_detail(&job, outcome);
        if (home_service_save(&job)) rc = 1;
        home_service_answer(&job); home_service_close(&job); free(current); _exit(rc != 0);
    }
    free(current);
    *started = job.snapshot; free(job.last);
    return 0;
}

static int home_request_portfolio(const LmbTuiState *reviewed) {
    HomeServiceSnapshot started;
    if (home_prepare_portfolio_begin(reviewed, NULL, &started)) return 1;
    HomeServiceSnapshot status;
    int rc = home_prepare_wait(&started, "Prepare selected models together", &status);
    return rc < 0 ? 1 : 0;
}
#endif
