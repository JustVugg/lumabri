/* Contiguous resident placement for an explicitly selected household.
 * No new protocol or inferred GPU support. Every selected node owns >=1 layer.
 * Search is exact for a FIXED node order and Edge owner, not all permutations.
 * Observed per-layer averages are only a cost heuristic, never measured tok/s. */
#ifndef LUMABRI_STAGE_PLACEMENT_H
#define LUMABRI_STAGE_PLACEMENT_H
#include <float.h>
#include <math.h>
#include "lumabri_cluster.h"
#include "lumabri_calibration.h"

/* Only ranges may differ. Hardware, builds, context, order and thread counts
 * must match before observations may guide a new candidate. */
static LMB_UNUSED int lmb_cal_stage_costs(const LmbCalibration *record,
    const LmbCalKey *current, double costs[LMB_CAL_NODES_MAX]) {
    if (!costs) return -1;
    memset(costs, 0, sizeof(double) * LMB_CAL_NODES_MAX);
    if (!lmb_cal_valid(record) || !lmb_cal_key_valid(current) ||
        !record->stage_count || record->stage_count != current->nodes) return -1;
    LmbCalKey equivalent = *current;
    for (uint32_t i = 0; i < current->nodes; i++) {
        if (record->key.layer_begin[i] != (i ? record->key.layer_end[i - 1] : 0) ||
            current->layer_begin[i] != (i ? current->layer_end[i - 1] : 0)) return -1;
        equivalent.layer_begin[i] = record->key.layer_begin[i];
        equivalent.layer_end[i] = record->key.layer_end[i];
    }
    if (current->layer_end[current->nodes - 1] != record->key.layer_end[current->nodes - 1]) return -1;
    if (!lmb_cal_matches(&record->key, &equivalent)) return -1;
    for (uint32_t i = 0; i < current->nodes; i++)
        costs[i] = record->stage_decode_seconds[i] /
            (record->key.layer_end[i] - record->key.layer_begin[i]);
    return 0;
}

/* out changes only after a complete independent reservation validation.
 * costs, when supplied, are indexed by node, not by range. Unknown costs are
 * NOT replaced with hardware guesses. NULL keeps a balanced memory prior. */
static LMB_UNUSED int lmb_home_plan_selected(const LmbModelShape *shape,
    uint64_t bytes, const LmbClusterNode *nodes, uint32_t count,
    uint32_t context, const LmbClusterPlan *seed, const double *costs,
    LmbClusterPlan *out) {
    if (!shape || !nodes || !seed || !out || !count || count > LMB_CLUSTER_MAX_NODES ||
        !shape->layers || shape->layers > LMB_PLAN_LAYER_MAX || count > shape->layers ||
        seed->edge_node >= count || seed->nslices > count || seed->sessions != 1 ||
        !seed->data_available || (seed->goal != LMB_GOAL_ONE_SESSION &&
                                 seed->goal != LMB_GOAL_THROUGHPUT)) return -1;
    if (!costs && seed->nslices == count) {
        LmbClusterPlan checked = *seed;
        if (!lmb_home_plan_budgets(shape, bytes, nodes, count, context, &checked) &&
            checked.state == LMB_PLAN_RESIDENT) { *out = checked; return 0; }
    }
    uint32_t order[LMB_CLUSTER_MAX_NODES], used = 0;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < seed->nslices; i++) {
        uint32_t node = seed->slices[i].node;
        if (node >= count || (seen & (UINT32_C(1) << node))) return -1;
        order[used++] = node; seen |= UINT32_C(1) << node;
    }
    for (uint32_t node = 0; node < count; node++)
        if (!(seen & (UINT32_C(1) << node))) order[used++] = node;
    double total_ram = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!nodes[i].ram_budget_bytes || (costs && (!isfinite(costs[i]) || costs[i] <= 0))) return -1;
        total_ram += (double)nodes[i].ram_budget_bytes;
    }
    const size_t width = (size_t)shape->layers + 1, cells = width * width;
    uint64_t *segment = malloc(cells * sizeof *segment), *edge = malloc(cells * sizeof *edge);
    double *dp = malloc((count + 1) * width * sizeof *dp);
    uint32_t *previous = calloc((count + 1) * width, sizeof *previous);
    if (!segment || !edge || !dp || !previous) { free(segment); free(edge); free(dp); free(previous); return -1; }
    int rc = -1;
    for (uint32_t begin = 0; begin < shape->layers; begin++)
        for (uint32_t end = begin + 1; end <= shape->layers; end++) {
            LmbHomeReservation r;
            segment[begin * width + end] = lmb_home_reservation(shape, bytes, begin, end,
                context, 0, &r) ? UINT64_MAX : r.total_bytes;
            edge[begin * width + end] = lmb_home_reservation(shape, bytes, begin, end,
                context, 1, &r) ? UINT64_MAX : r.total_bytes;
        }
    for (size_t i = 0; i < (count + 1) * width; i++) dp[i] = DBL_MAX;
    dp[0] = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t node = order[i];
        const uint64_t *reservations = node == seed->edge_node ? edge : segment;
        double target = shape->layers * ((double)nodes[node].ram_budget_bytes / total_ram);
        for (uint32_t end = i + 1; end <= shape->layers - (count - i - 1); end++)
            for (uint32_t begin = i; begin < end; begin++) {
                if (dp[i * width + begin] == DBL_MAX ||
                    reservations[begin * width + end] == UINT64_MAX ||
                    reservations[begin * width + end] > nodes[node].ram_budget_bytes) continue;
                double local = costs ? costs[node] * (end - begin) :
                    ((end - begin) - target) * ((end - begin) - target);
                double prior = dp[i * width + begin];
                double value = costs && seed->goal == LMB_GOAL_THROUGHPUT ?
                    (prior > local ? prior : local) : prior + local;
                if (value < dp[(i + 1) * width + end]) {
                    dp[(i + 1) * width + end] = value;
                    previous[(i + 1) * width + end] = begin;
                }
            }
    }
    if (dp[count * width + shape->layers] == DBL_MAX) goto done;
    LmbClusterPlan candidate = {0};
    candidate.goal = seed->goal; candidate.sessions = 1; candidate.data_available = 1;
    candidate.edge_node = seed->edge_node; candidate.nslices = count;
    uint32_t end = shape->layers;
    for (uint32_t i = count; i; i--) {
        LmbSlice *s = &candidate.slices[i - 1];
        s->node = order[i - 1]; s->layer_end = end;
        end = s->layer_begin = previous[i * width + end];
    }
    if (lmb_home_plan_budgets(shape, bytes, nodes, count, context, &candidate) ||
        candidate.state != LMB_PLAN_RESIDENT) goto done;
    candidate.ready_known = 1;
    for (uint32_t i = 0; i < count; i++) {
        LmbSlice *s = &candidate.slices[i]; const LmbClusterNode *node = &nodes[s->node];
        if (node->has_checkpoint) continue;
        LmbRangeCost weights = lmb_estimate_segment(shape, s->layer_begin, s->layer_end, context, 1);
        if (!weights.ok) goto done;
        s->bytes_to_fetch = weights.resident_bytes;
        if (s->node == candidate.edge_node) {
            weights = lmb_estimate_edge(shape, context, 1);
            if (!weights.ok) goto done;
            s->bytes_to_fetch = lmb_size_add(s->bytes_to_fetch, weights.resident_bytes);
        }
        candidate.fetch_bytes = lmb_size_add(candidate.fetch_bytes, s->bytes_to_fetch);
        if (candidate.fetch_bytes == UINT64_MAX) goto done;
        if (!node->lan_bps && s->bytes_to_fetch) candidate.ready_known = 0;
        else if (node->lan_bps) {
            double seconds = (double)s->bytes_to_fetch / node->lan_bps;
            if (seconds > candidate.ready_seconds) candidate.ready_seconds = seconds;
        }
    }
    *out = candidate; rc = 0;
done:
    free(segment); free(edge); free(dp); free(previous); return rc;
}
#endif
