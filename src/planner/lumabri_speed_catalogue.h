/* The speed plan as the catalogue sees it: inventory facts in, a typed
 * decision and one JSON projection out. Shared by the management API, the
 * command line and the screen, so that none of them plans on its own.
 * Read-only: no offer, no reservation, no model is loaded here. */
#ifndef LUMABRI_SPEED_CATALOGUE_H
#define LUMABRI_SPEED_CATALOGUE_H
#include <stdarg.h>
#include "lumabri_speed_plan.h"

#define LMB_SPEED_RECORD_TTL_SECONDS (30.0 * 86400.0)

typedef struct {
    LmbSpeedDecision decision;
    uint32_t node_count, node_indices[LMB_CLUSTER_MAX_NODES]; /* machine -> catalogue node */
    int automatic;            /* the pool was every eligible computer */
    int model_index;
    int has_incumbent;
    LmbReplan replan;
    char replan_reason[256];
} LmbSpeedSnapshot;

typedef struct {
    char *p; size_t len, cap; int bad;
} LmbSpeedText;

/* A computer may be planned on when it is a managed donor with a known,
 * queued compute policy, a free allocation slot and an offer. */
static int catalog_speed_managed(const LmbTuiState *st, uint32_t i) {
    return i < st->nnodes && st->nodes[i].addr[0] && st->identities[i][0] && st->runtime_ids[i][0] &&
        st->workloads[i].known && st->workloads[i].compute_policy == LMB_COMPUTE_LOCAL_FIFO &&
        st->nodes[i].threads;
}
static int catalog_speed_eligible(const LmbTuiState *st, uint32_t i) {
    return catalog_speed_managed(st, i) && st->workloads[i].allocations < 4 && st->nodes[i].ram_budget_bytes;
}

/* Pool: the selected computers, or every eligible one when none is selected
 * or `automatic` is set. `incumbent` is in catalogue node indices; `running`
 * says that it is loaded now rather than merely another candidate.
 * 0 chosen, 1 refused (reason set), -1 no decision could be made. */
static int catalog_speed_build(const LmbTuiState *st, int model_index, int automatic,
    const LmbClusterPlan *incumbent, int running, LmbSpeedSnapshot *out) {
    memset(out, 0, sizeof *out);
    out->decision.chosen = out->decision.best_single = out->decision.best_split = out->decision.incumbent = -1;
    out->model_index = model_index;
    if (model_index < 0 || model_index >= st->nmodels || !st->inventory_ok) {
        snprintf(out->decision.reason, sizeof out->decision.reason, "Household inventory is unavailable.");
        return -1;
    }
    const LmbTuiModel *m = &st->models[model_index];
    if (!m->weights_present || !m->checkpoint_inventory_ok || !m->shape.sizing_verified) {
        snprintf(out->decision.reason, sizeof out->decision.reason,
            "This checkpoint lacks validated resident sizing or source weights.");
        return -1;
    }
    int any_selected = 0;
    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++) any_selected |= st->selected_nodes[i][0] != 0;
    out->automatic = automatic || !any_selected;
    LmbSpeedMachine *machines = calloc(LMB_CLUSTER_MAX_NODES, sizeof *machines);
    LmbSpeedLink *links = calloc((size_t)LMB_CLUSTER_MAX_NODES * LMB_CLUSTER_MAX_NODES, sizeof *links);
    if (!machines || !links) { free(machines); free(links); return -1; }
    /* Offered memory is net of what is already loaded. Compared with a
     * placement that is running, the memory that placement holds counts as
     * available to it and to its alternatives: the question is where the
     * model should be, not whether a second copy fits beside the first. */
    uint64_t credit[LMB_CLUSTER_MAX_NODES] = {0};
    for (uint32_t j = 0; running && incumbent && !incumbent->hybrid && j < incumbent->nslices; j++) {
        const LmbSlice *slice = &incumbent->slices[j]; LmbHomeReservation held;
        if (slice->node < st->nnodes && !lmb_home_reservation_sessions(&m->shape, m->checkpoint_bytes,
            slice->layer_begin, slice->layer_end, st->context, st->sessions, slice->node == incumbent->edge_node, &held))
            credit[slice->node] = lmb_size_add(credit[slice->node], held.total_bytes);
    }
    uint32_t n = 0, reverse[LMB_CLUSTER_MAX_NODES];
    for (uint32_t i = 0; i < st->nnodes; i++) {
        reverse[i] = UINT32_MAX;
        if (!(credit[i] ? catalog_speed_managed(st, i) : catalog_speed_eligible(st, i)) ||
            (!out->automatic && !lmb_tui_node_enabled(st, i))) continue;
        machines[n].node = st->nodes[i];
        machines[n].node.ram_budget_bytes = lmb_size_add(machines[n].node.ram_budget_bytes, credit[i]);
        machines[n].node.has_checkpoint = 0;
        machines[n].speed = st->speeds[i];
        reverse[i] = n; out->node_indices[n++] = i;
    }
    out->node_count = n;
    for (uint32_t a = 0; a < n; a++) {
        const LmbMachineLinks *seen = &st->links[out->node_indices[a]];
        for (uint32_t k = 0; k < seen->count; k++) {
            char peer[65]; lmb_hex(peer, seen->links[k].peer, 32);
            for (uint32_t b = 0; b < n; b++)
                if (b != a && !strcmp(peer, st->identities[out->node_indices[b]]))
                    links[(size_t)a * n + b] = (LmbSpeedLink){seen->links[k].rtt_micros / 1e6,
                        (double)seen->links[k].bytes_per_s};
        }
    }
    /* The latest observation of this checkpoint counts only when every
     * computer in it is still the same computer, runtime and thread count. */
    LmbSpeedMeasured measured; uint32_t nmeasured = 0;
    const LmbCalibration *cal = &m->calibration; const LmbCalKey *key = &cal->key;
    double now = (double)time(NULL);
    if (m->has_calibration && lmb_cal_valid(cal) && !strcmp(key->plan_kind, "segment") &&
        key->context == st->context && key->sessions == st->sessions && key->nodes <= n &&
        !strcmp(key->build_id, st->build_id) && now >= cal->measured_at &&
        now - cal->measured_at <= LMB_SPEED_RECORD_TTL_SECONDS && key->edge_node < key->nodes) {
        memset(&measured, 0, sizeof measured);
        int usable = 1; uint32_t next = 0, owner[LMB_CAL_NODES_MAX];
        for (uint32_t j = 0; j < key->nodes && usable; j++) {
            uint32_t at = UINT32_MAX;
            for (uint32_t a = 0; a < n; a++)
                if (!strcmp(key->node_id[j], st->identities[out->node_indices[a]])) at = a;
            usable = at != UINT32_MAX && !key->from_disk[j] && key->layer_begin[j] == next &&
                key->layer_end[j] > next;
            if (!usable) break;
            uint32_t node = out->node_indices[at]; char hardware[65];
            catalog_hardware_id(&st->profiles[node], st->nodes[node].addr, hardware);
            usable = !strcmp(hardware, key->node_hardware_id[j]) &&
                !strcmp(st->runtime_ids[node], key->node_build_id[j]) && st->nodes[node].threads == key->threads[j];
            owner[j] = at; next = key->layer_end[j];
            measured.plan.slices[j] = (LmbSlice){.node = at, .layer_begin = key->layer_begin[j],
                .layer_end = key->layer_end[j]};
        }
        if (usable && next == m->shape.layers) {
            measured.plan.nslices = key->nodes; measured.plan.edge_node = owner[key->edge_node];
            measured.tok_s = cal->decode_tok_s;
            if (isfinite(measured.tok_s) && measured.tok_s > 0) nmeasured = 1;
            if (cal->stage_count == key->nodes)
                for (uint32_t j = 0; j < key->nodes; j++) {
                    double per_layer = cal->stage_decode_seconds[j] / (double)(key->layer_end[j] - key->layer_begin[j]);
                    if (isfinite(per_layer) && per_layer > 0) machines[owner[j]].layer_seconds = per_layer;
                }
        }
    }
    LmbClusterPlan current, *given = NULL;
    if (incumbent && incumbent->nslices && !incumbent->hybrid) {
        out->has_incumbent = running;
        current = *incumbent; given = &current;
        if (incumbent->edge_node >= st->nnodes || reverse[incumbent->edge_node] == UINT32_MAX) given = NULL;
        else current.edge_node = reverse[incumbent->edge_node];
        for (uint32_t j = 0; given && j < incumbent->nslices; j++) {
            uint32_t node = incumbent->slices[j].node;
            if (node >= st->nnodes || reverse[node] == UINT32_MAX) given = NULL;
            else current.slices[j].node = reverse[node];
        }
    }
    int rc;
    if (!n) {
        out->decision.refusal = LMB_SPEED_NO_MACHINE;
        snprintf(out->decision.reason, sizeof out->decision.reason, out->automatic ?
            "No computer is sharing memory in this household yet. Start sharing on at least one computer." :
            "None of the listed computers is sharing memory right now.");
        rc = 1;
    } else {
        LmbSpeedModel model = {&m->shape, m->checkpoint_bytes, st->context, st->sessions, st->context / 2};
        rc = lmb_speed_decide(machines, n, links, &model, nmeasured ? &measured : NULL, nmeasured, given,
                              &out->decision);
    }
    if (!rc && out->has_incumbent)
        out->replan = lmb_speed_replan(&out->decision, out->replan_reason, sizeof out->replan_reason);
    free(machines); free(links);
    return rc;
}

static void speed_text(LmbSpeedText *t, const char *format, ...) {
    if (t->bad) return;
    for (;;) {
        va_list args; va_start(args, format);
        int n = vsnprintf(t->p ? t->p + t->len : NULL, t->p ? t->cap - t->len : 0, format, args);
        va_end(args);
        if (n < 0) { t->bad = 1; return; }
        if (t->p && (size_t)n < t->cap - t->len) { t->len += (size_t)n; return; }
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap - t->len <= (size_t)n) cap *= 2;
        char *p = cap > (1u << 22) ? NULL : realloc(t->p, cap);
        if (!p) { t->bad = 1; return; }
        t->p = p; t->cap = cap;
    }
}

static void speed_json_string(LmbSpeedText *t, const char *s) {
    speed_text(t, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') speed_text(t, "\\%c", *p);
        else if (*p < 32 || *p >= 127) speed_text(t, "\\u%04x", *p);
        else speed_text(t, "%c", *p);
    }
    speed_text(t, "\"");
}

static void speed_json_number(LmbSpeedText *t, int known, double value) {
    if (known && isfinite(value)) speed_text(t, "%.6g", value); else speed_text(t, "null");
}

static void speed_json_candidate(LmbSpeedText *t, const LmbTuiState *st, const LmbSpeedSnapshot *s, int index) {
    if (index < 0) { speed_text(t, "null"); return; }
    const LmbSpeedCandidate *c = &s->decision.candidates[index];
    speed_text(t, "{\"computers\":[");
    for (uint32_t i = 0; i < c->plan.nslices; i++) {
        const LmbSlice *slice = &c->plan.slices[i]; uint32_t node = s->node_indices[slice->node];
        speed_text(t, "%s{\"id\":\"%s\",\"name\":", i ? "," : "", st->identities[node]);
        speed_json_string(t, st->nodes[node].name);
        speed_text(t, ",\"begin\":%u,\"end\":%u,\"reserved_bytes\":%llu,\"reserved_vram_bytes\":0,"
            "\"device\":\"cpu\",\"chat_host\":%s}", slice->layer_begin, slice->layer_end,
            (unsigned long long)slice->bytes_resident, slice->node == c->plan.edge_node ? "true" : "false");
    }
    speed_text(t, "],\"estimated_tok_s\":"); speed_json_number(t, c->estimated, c->tok_s);
    speed_text(t, ",\"estimated_tok_s_all_sessions_busy\":"); speed_json_number(t, c->estimated, c->tok_s_all_sessions_busy);
    speed_text(t, ",\"basis\":[");
    const char *const names[] = {"machine_probe", "observed_stage", "measured_plan"}; int first = 1;
    for (unsigned b = 0; b < 3; b++) if (c->estimated && (c->basis & (1u << b))) {
        speed_text(t, "%s\"%s\"", first ? "" : ",", names[b]); first = 0;
    }
    int terms = c->estimated && c->basis != LMB_SPEED_BASIS_MEASURED_PLAN;
    speed_text(t, "],\"seconds_per_token\":{\"layers\":"); speed_json_number(t, terms, c->layer_seconds);
    speed_text(t, ",\"chat_host\":"); speed_json_number(t, terms, c->edge_seconds);
    speed_text(t, ",\"network\":"); speed_json_number(t, terms, c->link_seconds);
    speed_text(t, "},\"slowest_link_seconds\":");
    speed_json_number(t, c->machines > 1 && c->slowest_link_seconds > 0, c->slowest_link_seconds);
    speed_text(t, ",\"slow_link\":%s", c->machines > 1 && c->slowest_link_seconds > LMB_SPEED_SLOW_LINK_SECONDS ? "true" : "false");
    speed_text(t, ",\"reference_prompt_network_seconds\":");
    speed_json_number(t, c->machines > 1 && c->prefill_link_seconds > 0, c->prefill_link_seconds);
    speed_text(t, ",\"reserved_bytes\":%llu}", (unsigned long long)c->reserved_bytes);
}

/* Caller frees out->p. Estimates are estimates: decode_tok_s stays the name
 * of a measurement and never appears here. */
static int catalog_speed_json(const LmbTuiState *st, const LmbSpeedSnapshot *s, int rc, LmbSpeedText *t) {
    const LmbSpeedDecision *d = &s->decision;
    static const char *const refusals[] = {"none", "not_enough_memory", "no_placement_found", "no_computer_sharing", "invalid_input"};
    static const char *const whys[] = {"only_computer_that_fits", "fastest_single_computer", "split_not_shown_faster",
        "no_single_computer_fits", "split_estimated_faster"};
    static const char *const replans[] = {"keep", "propose", "current_placement_broken"};
    speed_text(t, "{\"schema\":1,\"objective\":\"decode_tokens_per_second_one_active_conversation\","
        "\"selection\":\"%s\",\"status\":\"%s\",\"refusal\":", s->automatic ? "automatic" : "listed_computers",
        !rc ? "chosen" : rc > 0 ? "refused" : "unavailable");
    if (rc > 0) speed_text(t, "\"%s\"", refusals[d->refusal <= LMB_SPEED_INVALID ? d->refusal : LMB_SPEED_INVALID]);
    else speed_text(t, "null");
    speed_text(t, ",\"why\":");
    if (!rc) speed_text(t, "\"%s\"", whys[d->why]); else speed_text(t, "null");
    speed_text(t, ",\"reason\":"); speed_json_string(t, d->reason);
    speed_text(t, ",\"model\":"); speed_json_string(t, s->model_index >= 0 && s->model_index < st->nmodels ?
        st->models[s->model_index].name : "");
    speed_text(t, ",\"context\":%u,\"sessions\":%u,\"needed_bytes\":%llu,\"offered_bytes\":%llu,"
        "\"computers_considered\":%u,\"computers_that_fit_alone\":%u,\"search_bounded\":%s,"
        "\"split_margin\":%.2f,\"execution\":\"cpu\",\"vram_budget_used\":false,\"unmeasured\":[",
        st->context, st->sessions, (unsigned long long)d->needed_bytes, (unsigned long long)d->offered_bytes,
        s->node_count, d->fits_single, d->bounded ? "true" : "false", LMB_SPEED_SPLIT_MARGIN);
    int first = 1;
    for (uint32_t i = 0; i < s->node_count; i++) if (d->unmeasured_mask & (UINT32_C(1) << i)) {
        speed_text(t, "%s\"%s\"", first ? "" : ",", st->identities[s->node_indices[i]]); first = 0;
    }
    speed_text(t, "],\"chosen\":"); speed_json_candidate(t, st, s, rc ? -1 : d->chosen);
    speed_text(t, ",\"best_single\":"); speed_json_candidate(t, st, s, rc ? -1 : d->best_single);
    speed_text(t, ",\"best_split\":"); speed_json_candidate(t, st, s, rc ? -1 : d->best_split);
    speed_text(t, ",\"candidates\":[");
    uint32_t written = 0;
    for (uint32_t i = 0; !rc && i < d->count && written < 24; i++) {
        if (d->candidates[i].machines > 1 && (int)i != d->chosen && (int)i != d->best_split &&
            (int)i != d->incumbent && written >= 12) continue;
        speed_text(t, "%s", written++ ? "," : ""); speed_json_candidate(t, st, s, (int)i);
    }
    speed_text(t, "],\"replan\":");
    if (!rc && s->has_incumbent) {
        speed_text(t, "{\"advice\":\"%s\",\"automatic_action\":false,\"reason\":", replans[s->replan]);
        speed_json_string(t, s->replan_reason);
        speed_text(t, ",\"current\":"); speed_json_candidate(t, st, s, d->incumbent);
        speed_text(t, "}");
    } else speed_text(t, "null");
    speed_text(t, "}");
    return t->bad ? -1 : 0;
}

/* Plain text for a terminal: which computer holds what, the estimate, why. */
static int catalog_speed_print(const LmbTuiState *st, const LmbSpeedSnapshot *s, int rc, FILE *out) {
    const LmbSpeedDecision *d = &s->decision;
    if (rc) { fprintf(out, "No placement: %s\n", d->reason); return rc > 0 ? 1 : 2; }
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    fprintf(out, "Placement for %s (%s, %u conversation slot%s)\n", st->models[s->model_index].name,
        s->automatic ? "computers chosen automatically" : "among the listed computers",
        st->sessions, st->sessions == 1 ? "" : "s");
    for (uint32_t i = 0; i < c->plan.nslices; i++) {
        const LmbSlice *slice = &c->plan.slices[i];
        fprintf(out, "  %-24.24s layers %u-%u  %.2f GB reserved%s\n", st->nodes[s->node_indices[slice->node]].name,
            slice->layer_begin, slice->layer_end - 1, slice->bytes_resident / 1e9,
            slice->node == c->plan.edge_node ? "  chat host" : "");
    }
    if (c->estimated) {
        fprintf(out, "Estimated speed: %.2f tok/s for one active conversation", c->tok_s);
        if (st->sessions > 1) fprintf(out, "; %.2f tok/s each when all %u are busy", c->tok_s_all_sessions_busy, st->sessions);
        fprintf(out, " (an estimate, not a measurement)\n");
    } else fprintf(out, "Estimated speed: unknown until every computer has been measured\n");
    fprintf(out, "Why: %s\n", d->reason);
    if (s->has_incumbent) fprintf(out, "Re-planning: %s\n", s->replan_reason);
    return 0;
}
#endif
