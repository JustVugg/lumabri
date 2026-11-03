/* Exact execution identity, shared by native catalogue frontends. */
#ifndef LMB_CALIBRATION_KEY_H
#define LMB_CALIBRATION_KEY_H
static void catalog_hardware_id(const LmbMachineProfile *p, const char *address, char out[65]) {
    LmbSha sha; uint8_t digest[32]; lmb_sha_init(&sha);
    const char *fields[] = {p->os, p->arch, p->cpu_model, p->isa, address};
    for (unsigned i = 0; i < sizeof fields / sizeof *fields; i++)
        lmb_sha_update(&sha, fields[i], strlen(fields[i]) + 1);
    lmb_sha_le32(&sha, p->logical_cpus); lmb_sha_le32(&sha, p->physical_cores);
    lmb_sha_le32(&sha, p->numa_nodes); lmb_sha_le64(&sha, p->ram_total_bytes);
    lmb_sha_le32(&sha, p->gpu_count); lmb_sha_le64(&sha, p->vram_total_bytes);
    lmb_sha_final(&sha, digest); lmb_hex(out, digest, sizeof digest);
}

static int catalog_calibration_key(const LmbTuiState *st, const LmbTuiModel *m,
    uint32_t abi, const char *numeric, LmbCalKey *key) {
    memset(key, 0, sizeof *key);
    if (!m->planned || m->plan.state != LMB_PLAN_RESIDENT || !m->content_id[0] ||
        !st->build_id[0] || !m->plan.nslices || m->plan.nslices > LMB_CAL_NODES_MAX) return -1;
    snprintf(key->model_root, sizeof key->model_root, "%s", m->content_id);
    snprintf(key->adapter, sizeof key->adapter, "%s", m->shape.segment_id);
    snprintf(key->build_id, sizeof key->build_id, "%s", st->build_id);
    snprintf(key->plan_kind, sizeof key->plan_kind, "%s", m->plan.hybrid ? "hybrid" : "segment");
    snprintf(key->numeric_class, sizeof key->numeric_class, "%s", numeric ? numeric : "");
    key->adapter_abi = abi; key->goal = m->plan.goal;
    key->context = st->context; key->sessions = st->sessions; key->nodes = m->plan.nslices;
    int edge = 0;
    for (uint32_t i = 0; i < key->nodes; i++) {
        const LmbSlice *slice = &m->plan.slices[i]; uint32_t n = slice->node;
        if (n >= st->nnodes || !st->identities[n][0] || !st->runtime_ids[n][0]) return -1;
        if (n == m->plan.edge_node) { key->edge_node = i; edge = 1; }
        snprintf(key->node_id[i], sizeof key->node_id[i], "%s", st->identities[n]);
        snprintf(key->node_build_id[i], sizeof key->node_build_id[i], "%s", st->runtime_ids[n]);
        snprintf(key->node_backend[i], sizeof key->node_backend[i], "cpu");
        catalog_hardware_id(&st->profiles[n], st->nodes[n].addr, key->node_hardware_id[i]);
        key->layer_begin[i] = slice->layer_begin; key->layer_end[i] = slice->layer_end;
        key->threads[i] = st->nodes[n].threads > 256 ? 256 : st->nodes[n].threads;
        key->workload[i] = st->workloads[n];
        key->workload[i].active = key->workload[i].queued = 0;
    }
    /* ABI/numeric may be unknown until the real Edge greeting. All other
     * fields must already be bound; the writer validates the completed key. */
    return edge ? 0 : -1;
}

/* Missing leased inventory is not evidence of a changed executable. A loaded
 * donor can briefly disappear while its worker reconnects. Never substitute a
 * new identity: retry absence, but reject an observed mismatch immediately. */
static int catalog_runtime_match(const LmbCalKey *key, const LmbMachineReport *reports,
    uint32_t count, char *why, size_t cap) {
    int missing = 0;
    for (uint32_t j = 0; j < key->nodes; j++) {
        const LmbMachineReport *found = NULL;
        for (uint32_t k = 0; k < count; k++) {
            char peer[65]; lmb_hex(peer, reports[k].identity, 32);
            if (!strcmp(peer, key->node_id[j])) { found = &reports[k]; break; }
        }
        if (!found) {
            if (!missing) snprintf(why, cap, "Donor %u inventory temporarily unavailable", j + 1);
            missing = 1; continue;
        }
        char hardware[65];
        catalog_hardware_id(&found->machine, found->control_addr, hardware);
        unsigned threads = found->machine.logical_cpus;
        if (found->runtime_threads && threads > found->runtime_threads) threads = found->runtime_threads;
        if (threads > 256) threads = 256;
        const char *changed = strcmp(hardware, key->node_hardware_id[j]) ? "hardware or endpoint" :
            !found->runtime_id[0] ? "runtime identity unavailable" :
            strcmp(found->runtime_id, key->node_build_id[j]) ? "runtime identity" :
            threads != key->threads[j] ? "thread capacity" : NULL;
        if (changed) {
            snprintf(why, cap, "Donor %u: %s differs from the approved snapshot", j + 1, changed);
            return -1;
        }
    }
    return missing;
}

/* Apply measured Segment costs consistently in every catalogue frontend.
 * A changed placement gets a new key; its old speed is not reused. */
static void catalog_calibration_apply(LmbTuiState *st, LmbTuiModel *m, int household,
    const LmbClusterNode *selected, const uint32_t *mapping, uint32_t nselected) {
    m->calibration_key_valid = !catalog_calibration_key(st, m, m->calibration.key.adapter_abi,
        m->calibration.key.numeric_class, &m->calibration_key) && lmb_cal_key_valid(&m->calibration_key);
    double stage_costs[LMB_CAL_NODES_MAX], node_costs[LMB_CLUSTER_MAX_NODES] = {0};
    if (household && !m->plan.hybrid && m->calibration_key_valid &&
        !lmb_cal_stage_costs(&m->calibration, &m->calibration_key, stage_costs)) {
        LmbClusterPlan local = m->plan, candidate;
        for (uint32_t a = 0; a < nselected; a++) {
            if (mapping[a] == m->plan.edge_node) local.edge_node = a;
            for (uint32_t b = 0; b < local.nslices; b++)
                if (mapping[a] == m->plan.slices[b].node) {
                    local.slices[b].node = a; node_costs[a] = stage_costs[b];
                }
        }
        if (!lmb_home_plan_selected(&m->shape, m->checkpoint_bytes, selected,
            nselected, st->context, &local, node_costs, &candidate)) {
            candidate.edge_node = mapping[candidate.edge_node];
            for (uint32_t j = 0; j < candidate.nslices; j++)
                candidate.slices[j].node = mapping[candidate.slices[j].node];
            m->plan = candidate; m->stage_cost_placement = 1;
            m->calibration_key_valid = !catalog_calibration_key(st, m,
                m->calibration.key.adapter_abi, m->calibration.key.numeric_class,
                &m->calibration_key) && lmb_cal_key_valid(&m->calibration_key);
        }
    }
}

#endif
