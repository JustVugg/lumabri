/* Shared conservative resident admission policy. This is NOT a verified
 * streaming working-set contract and must not enable disk execution. */
#ifndef LUMABRI_MEMORY_BUDGET_H
#define LUMABRI_MEMORY_BUDGET_H
#include "lumabri_planner.h"
#include "src/runtime/lumabri_prepare_limits.h"
#include "src/runtime/lumabri_session_limits.h"

typedef struct {
    uint64_t segment_bytes, edge_bytes, total_bytes;
    uint64_t vram_bytes; /* zero until an adapter declares a GPU memory contract */
} LmbHomeReservation;

/* Admission compares two budgets that are never fungible: host RAM and
 * device VRAM. Every current adapter executes on the CPU, so every need
 * carries vram_bytes == 0 today. A future GPU role fills the second field;
 * it must not turn VRAM into RAM or the reverse. */
typedef struct {
    uint64_t ram_bytes, vram_bytes;
} LmbMemoryBudget;

typedef enum {
    LMB_ADMIT_OK = 0, LMB_ADMIT_RAM_SHORT, LMB_ADMIT_VRAM_SHORT, LMB_ADMIT_INVALID
} LmbAdmission;

static LMB_UNUSED LmbAdmission lmb_budget_admit(const LmbMemoryBudget *need,
                                                const LmbMemoryBudget *offer) {
    if (!need || !offer || need->ram_bytes == UINT64_MAX || need->vram_bytes == UINT64_MAX ||
        offer->ram_bytes == UINT64_MAX || offer->vram_bytes == UINT64_MAX ||
        (!need->ram_bytes && !need->vram_bytes)) return LMB_ADMIT_INVALID;
    if (need->ram_bytes > offer->ram_bytes) return LMB_ADMIT_RAM_SHORT;
    if (need->vram_bytes > offer->vram_bytes) return LMB_ADMIT_VRAM_SHORT;
    return LMB_ADMIT_OK;
}

static LMB_UNUSED const char *lmb_admission_name(LmbAdmission a) {
    switch (a) {
    case LMB_ADMIT_OK: return "admitted";
    case LMB_ADMIT_RAM_SHORT: return "not enough RAM";
    case LMB_ADMIT_VRAM_SHORT: return "not enough VRAM";
    default: return "invalid memory request";
    }
}

typedef struct {
    uint64_t weights, state, scratch, overhead, reserved;
    int reservation_complete;
} LmbMemoryEvidence;

/* Adapter-derived estimates alongside, not confused with, the reservation.
 * Preliminary local previews may not include the Edge reservation yet. */
static LMB_UNUSED int lmb_memory_evidence(const LmbModelShape *shape,
    uint32_t begin, uint32_t end, uint32_t context, uint32_t sessions,
    int runs_edge, uint64_t reserved, LmbMemoryEvidence *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!shape || (runs_edge != 0 && runs_edge != 1)) return -1;
    LmbRangeCost c = lmb_estimate_segment(shape, begin, end, context, sessions);
    if (runs_edge) {
        LmbRangeCost edge = lmb_estimate_edge(shape, context, sessions);
        c.ok = c.ok && edge.ok;
        c.resident_bytes = lmb_size_add(c.resident_bytes, edge.resident_bytes);
        c.state_bytes = lmb_size_add(c.state_bytes, edge.state_bytes);
        c.scratch_bytes = lmb_size_add(c.scratch_bytes, edge.scratch_bytes);
    }
    uint64_t total = lmb_size_add(c.resident_bytes, lmb_size_add(c.state_bytes, c.scratch_bytes));
    if (!c.ok || total == UINT64_MAX || reserved == UINT64_MAX) return -1;
    *out = (LmbMemoryEvidence){.weights = c.resident_bytes, .state = c.state_bytes,
        .scratch = c.scratch_bytes, .reserved = reserved, .reservation_complete = reserved >= total};
    if (out->reservation_complete) out->overhead = reserved - total;
    return 0;
}

static LMB_UNUSED uint64_t lmb_budget_add(uint64_t a, uint64_t b) {
    return lmb_size_add(a, b);
}

static LMB_UNUSED uint64_t lmb_budget_mib(uint64_t bytes) {
    const uint64_t mask = (UINT64_C(1) << 20) - 1;
    uint64_t rounded = lmb_budget_add(bytes, mask);
    return rounded == UINT64_MAX ? UINT64_MAX : rounded & ~mask;
}

/* An explicit process ceiling is not permission to consume the system
 * reserve. Unknown/exhausted physical RAM must never become a zero
 * (unlimited) runtime budget. The runtime governor still handles subsequent
 * changes in availability; this preflight is not an atomic reservation. */
static LMB_UNUSED int lmb_process_budget(uint64_t requested, uint64_t available,
                                        uint64_t reserve, uint64_t *out) {
    if (!out) return -1;
    *out = 0;
    if (!available || available == UINT64_MAX || available <= reserve) return -1;
    uint64_t usable = available - reserve;
    *out = requested && requested < usable ? requested : usable;
    return 0;
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

/* The weight bytes a range is guarded for. A historical estimate keeps the
 * proportional share of the checkpoint plus 5% of the whole. That share
 * assumes equal layers: when one layer holds a large table, every other
 * range is charged for it, and a model that fits cannot be placed. With an
 * explicit layer map the described bytes are the tensors actually assigned,
 * so the guard is those bytes plus 5%, and never more than the old one. */
static LMB_UNUSED uint64_t lmb_range_weight_guard(const LmbModelShape *shape,
    uint64_t checkpoint_bytes, uint32_t begin, uint32_t end, uint64_t resident_bytes) {
    uint64_t share = lmb_checkpoint_floor(checkpoint_bytes, shape->layers, begin, end);
    if (shape->memory_contract != 1) return share;
    uint64_t described = lmb_budget_add(resident_bytes, resident_bytes / 20u);
    return described < share ? described : share;
}

static LMB_UNUSED int lmb_home_reservation_sessions(const LmbModelShape *shape,
    uint64_t checkpoint_bytes, uint32_t begin, uint32_t end,
    uint32_t context, uint32_t sessions, int runs_edge, LmbHomeReservation *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!shape || !sessions || sessions > LMB_HOST_MAX_SESSIONS || (runs_edge != 0 && runs_edge != 1)) return -1;
    LmbRangeCost segment = lmb_estimate_segment(shape, begin, end, context, sessions);
    if (!segment.ok) return -1;
    uint64_t floor = lmb_range_weight_guard(shape, checkpoint_bytes, begin, end, segment.resident_bytes);
    uint64_t live = lmb_budget_add(segment.state_bytes, segment.scratch_bytes);
    uint64_t guarded = lmb_budget_add(lmb_budget_add(floor, UINT64_C(128) << 20), live);
    uint64_t described = lmb_budget_add(segment.resident_bytes, live);
    out->segment_bytes = lmb_budget_mib(lmb_budget_add(
        guarded > described ? guarded : described, LMB_PREPARE_CACHE_BYTES));
    if (runs_edge) {
        LmbRangeCost edge = lmb_estimate_edge(shape, context, sessions);
        if (!edge.ok) return -1;
        uint64_t cost = lmb_budget_add(edge.resident_bytes,
                        lmb_budget_add(edge.state_bytes, edge.scratch_bytes));
        if (sessions > 1) cost = lmb_budget_add(cost, lmb_size_mul(sessions, LMB_HOST_SESSION_BYTES));
        out->edge_bytes = lmb_budget_mib(lmb_budget_add(
            lmb_budget_add(cost, UINT64_C(64) << 20), LMB_PREPARE_CACHE_BYTES));
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

static LMB_UNUSED int lmb_home_reservation(const LmbModelShape *shape,
    uint64_t checkpoint_bytes, uint32_t begin, uint32_t end,
    uint32_t context, int runs_edge, LmbHomeReservation *out) {
    return lmb_home_reservation_sessions(shape, checkpoint_bytes, begin, end, context, 1, runs_edge, out);
}
#endif
