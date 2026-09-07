/* Shared conservative resident admission policy. This is NOT a verified
 * streaming working-set contract and must not enable disk execution. */
#ifndef LUMABRI_MEMORY_BUDGET_H
#define LUMABRI_MEMORY_BUDGET_H
#include "lumabri_planner.h"

typedef struct {
    uint64_t segment_bytes, edge_bytes, total_bytes;
} LmbHomeReservation;

static LMB_UNUSED uint64_t lmb_budget_add(uint64_t a, uint64_t b) {
    return lmb_size_add(a, b);
}

static LMB_UNUSED uint64_t lmb_budget_mib(uint64_t bytes) {
    const uint64_t mask = (UINT64_C(1) << 20) - 1;
    uint64_t rounded = lmb_budget_add(bytes, mask);
    return rounded == UINT64_MAX ? UINT64_MAX : rounded & ~mask;
}

/* ceil-by-MiB is deliberately left to the caller. The proportional share
 * keeps its remainder; bytes/layers*count alone silently drops it. */
static LMB_UNUSED uint64_t lmb_checkpoint_floor(uint64_t bytes,
                                               uint32_t layers,
                                               uint32_t begin, uint32_t end) {
    if (!bytes || !layers || begin >= end || end > layers) return UINT64_MAX;
    uint64_t count = end - begin;
    uint64_t share = bytes / layers * count + (bytes % layers) * count / layers;
    return lmb_budget_add(share, bytes / 20u);
}

static LMB_UNUSED int lmb_home_reservation(const LmbModelShape *shape,
    uint64_t checkpoint_bytes, uint32_t begin, uint32_t end,
    uint32_t context, int runs_edge, LmbHomeReservation *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!shape || (runs_edge != 0 && runs_edge != 1)) return -1;
    LmbRangeCost segment = lmb_estimate_segment(shape, begin, end, context, 1);
    if (!segment.ok) return -1;
    uint64_t floor = lmb_checkpoint_floor(checkpoint_bytes, shape->layers, begin, end);
    uint64_t live = lmb_budget_add(segment.state_bytes, segment.scratch_bytes);
    uint64_t guarded = lmb_budget_add(lmb_budget_add(floor, UINT64_C(128) << 20), live);
    uint64_t described = lmb_budget_add(segment.resident_bytes, live);
    out->segment_bytes = lmb_budget_mib(guarded > described ? guarded : described);
    if (runs_edge) {
        LmbRangeCost edge = lmb_estimate_edge(shape, context, 1);
        if (!edge.ok) return -1;
        uint64_t cost = lmb_budget_add(edge.resident_bytes,
                        lmb_budget_add(edge.state_bytes, edge.scratch_bytes));
        out->edge_bytes = lmb_budget_mib(lmb_budget_add(cost, UINT64_C(64) << 20));
    }
    out->total_bytes = lmb_budget_add(out->segment_bytes, out->edge_bytes);
    /* Both process budgets are individually aligned. Subtracting an
     * unaligned Edge budget and truncating it in --memory-limit-mb could
     * otherwise give Segment less memory than the approved calculation. */
    if (out->segment_bytes == UINT64_MAX || out->edge_bytes == UINT64_MAX ||
        out->total_bytes == UINT64_MAX) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}
#endif
