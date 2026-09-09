/* lumabri_cluster.h — what this collection of computers can actually run.
 *
 * The catalogue's whole job is to answer three questions about every model,
 * and to keep them apart:
 *
 *   can it run at all?      resident, from disk, or not at all
 *   what is missing?        gigabytes, and how many machines that is
 *   how would it be split?  a range per node, and who runs Edge
 *
 * A fourth question — how fast — is deliberately NOT answered here. A plan
 * knows what fits; only a calibration knows what it does, and the two must
 * never be printed in the same voice.
 *
 * The final placement has two objectives and they pull apart: one serial
 * session minimises the SUM of stages and hops, while concurrent sessions
 * minimise the SLOWEST pipeline stage. This preliminary planner records the
 * requested goal but has no calibrated stage times yet; it therefore makes a
 * memory-feasible proportional split and never claims that split is a speed
 * optimum. Calibration is the prerequisite for performance placement. */
#ifndef LUMABRI_CLUSTER_H
#define LUMABRI_CLUSTER_H

#include "lumabri_machine.h"   /* LMB_GPU_* : the backends an engine can use */
#include "lumabri_planner.h"
#include "lumabri_memory_budget.h"

#define LMB_CLUSTER_MAX_NODES 32

typedef struct {
    char name[64];
    char addr[64];
    uint64_t ram_budget_bytes;   /* what this machine offers, reserve removed */
    uint64_t vram_budget_bytes;  /* inventory only; never fungible with RAM */
    uint64_t disk_read_bps;      /* 0 = unmeasured: disk mode cannot be judged */
    uint64_t lan_bps;            /* to the node running Edge; 0 = unmeasured */
    double rtt_ms;               /* to the node running Edge */
    uint32_t gpu_backends;       /* inventory; adapter-specific use comes later */
    uint32_t threads;
    int has_checkpoint;          /* the weights are already on this machine */
} LmbClusterNode;

typedef struct {
    uint32_t node;               /* index into the node array */
    uint32_t layer_begin, layer_end;
    LmbPlanState state;
    uint64_t bytes_resident;     /* what it will actually hold */
    uint64_t bytes_to_fetch;     /* what it must receive first */
} LmbSlice;

typedef enum {
    LMB_GOAL_ONE_SESSION = 0,    /* minimise the sum: a chain */
    LMB_GOAL_THROUGHPUT,         /* minimise the slowest stage: a pipeline */
} LmbPlanGoal;

typedef struct {
    LmbPlanState state;
    LmbPlanGoal goal;
    uint32_t nslices;
    LmbSlice slices[LMB_CLUSTER_MAX_NODES];
    uint32_t edge_node;          /* who holds embedding and head */
    uint64_t missing_bytes;      /* 0 when it fits */
    uint32_t missing_nodes;      /* machines of the median size that would fix it */
    uint64_t fetch_bytes;        /* total that must cross the network first */
    double ready_seconds;        /* download and distribution, NOT speed */
    int ready_known;             /* 0 when no bandwidth was measured */
    uint32_t sessions;
    int data_available;          /* at least one node owns the checkpoint */
} LmbClusterPlan;

/* Give each node a share of the layers proportional to what it can hold, so
 * a 32 GB machine takes twice the range of a 16 GB one instead of the same.
 * Equal splits are the obvious thing and they are wrong on the hardware a
 * house actually has, where one box is always the big one. */
static LMB_UNUSED int lmb_plan_cluster_source(const LmbModelShape *m,
                                       const LmbClusterNode *nodes, uint32_t n,
                                       uint32_t context, uint32_t sessions,
                                       LmbPlanGoal goal,
                                       int external_checkpoint,
                                       LmbClusterPlan *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->goal = goal;
    out->sessions = sessions ? sessions : 1;
    out->state = LMB_PLAN_UNRUNNABLE;
    out->data_available = external_checkpoint != 0;
    if (!m || !nodes || !m->layers || !n || n > LMB_CLUSTER_MAX_NODES) return -1;

    /* Until an adapter declares a working GPU backend, RAM and VRAM are not
     * interchangeable. Counting both can accept a plan no engine can load. */
    LmbRangeCost edge = lmb_estimate_edge(m, context, sessions);
    if (!edge.ok) return -1;
    uint64_t edge_need = lmb_size_add(edge.resident_bytes,
                         lmb_size_add(edge.state_bytes, edge.scratch_bytes));
    if (edge_need == UINT64_MAX) return -1;
    uint32_t best_edge = UINT32_MAX;
    uint64_t best_edge_room = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t room = nodes[i].ram_budget_bytes;
        if (room < edge_need) continue;
        if (best_edge == UINT32_MAX || room > best_edge_room) {
            best_edge = i; best_edge_room = room;
        }
    }
    if (best_edge == UINT32_MAX) return -1;      /* nobody can hold Edge */
    out->edge_node = best_edge;

    uint64_t effective[LMB_CLUSTER_MAX_NODES];
    uint64_t total_budget = 0;
    for (uint32_t i = 0; i < n; i++) {
        effective[i] = nodes[i].ram_budget_bytes;
        if (i == best_edge) effective[i] -= edge_need;
        total_budget = lmb_size_add(total_budget, effective[i]);
        if (nodes[i].has_checkpoint) out->data_available = 1;
    }
    if (!total_budget || total_budget == UINT64_MAX) return -1;

    /* Hand out layers in proportion to budget, in one pass, never leaving a
     * layer unassigned: the last node takes whatever rounding left over. */
    uint32_t assigned = 0;
    uint64_t whole_resident = 0;
    for (uint32_t i = 0; i < n && assigned < m->layers; i++) {
        uint64_t budget = effective[i];
        uint64_t scaled = lmb_size_mul(m->layers, budget);
        if (scaled == UINT64_MAX) return -1;
        uint32_t take = (uint32_t)(scaled / total_budget);
        if (i + 1 == n) take = m->layers - assigned;
        if (!take) continue;
        if (take > m->layers - assigned) take = m->layers - assigned;

        LmbSlice *s = &out->slices[out->nslices];
        s->node = i;
        s->layer_begin = assigned;
        s->layer_end = assigned + take;
        LmbRangeCost c = lmb_estimate_segment(m, s->layer_begin, s->layer_end,
                                              context, sessions);
        if (!c.ok) return -1;
        uint64_t live = lmb_size_add(c.state_bytes, c.scratch_bytes);
        s->state = lmb_plan_state(m, &c, budget);
        s->bytes_resident = s->state == LMB_PLAN_DISK
                          ? c.working_set_bytes + live
                          : c.resident_bytes + live;
        uint64_t needed_weights = s->state == LMB_PLAN_DISK
                                ? c.working_set_bytes : c.resident_bytes;
        s->bytes_to_fetch = nodes[i].has_checkpoint ? 0 : needed_weights;
        whole_resident = lmb_size_add(whole_resident, c.resident_bytes + live);
        if (s->state == LMB_PLAN_UNRUNNABLE) {
            /* Missing is measured against what this node would ACTUALLY have
             * to hold, which is the resident cost unless the adapter has
             * demonstrated streaming. Measuring it against the working set
             * regardless reports zero missing for a model that plainly does
             * not fit — the cache floor is small, so it almost always fits —
             * and a catalogue that says "does not run, nothing missing"
             * tells a person nothing they can act on. */
            uint64_t need = m->disk_streaming ? c.working_set_bytes + live
                                              : c.resident_bytes + live;
            out->missing_bytes = lmb_size_add(out->missing_bytes, need > budget ? need - budget : 0);
        }
        out->fetch_bytes = lmb_size_add(out->fetch_bytes, s->bytes_to_fetch);
        if (whole_resident == UINT64_MAX || out->missing_bytes == UINT64_MAX ||
            out->fetch_bytes == UINT64_MAX) return -1;
        out->nslices++;
        assigned += take;
    }
    if (assigned < m->layers) return -1;          /* no coverage: not a plan */

    /* The plan is only as good as its worst slice. A cluster where one node
     * cannot hold its range does not "mostly run" the model — it does not
     * run it, and saying otherwise is the failure mode this whole catalogue
     * exists to avoid. */
    out->state = LMB_PLAN_RESIDENT;
    for (uint32_t i = 0; i < out->nslices; i++) {
        if (out->slices[i].state == LMB_PLAN_UNRUNNABLE)
            { out->state = LMB_PLAN_UNRUNNABLE; break; }
        if (out->slices[i].state == LMB_PLAN_DISK)
            out->state = LMB_PLAN_DISK;
    }
    if (!out->data_available) {
        out->state = LMB_PLAN_UNRUNNABLE;
        out->missing_bytes = lmb_size_add(whole_resident, edge.resident_bytes);
        if (out->missing_bytes == UINT64_MAX) return -1;
    }

    if (out->state == LMB_PLAN_UNRUNNABLE && out->missing_bytes) {
        uint64_t median = total_budget / n;
        uint64_t additional = median ? out->missing_bytes / median +
            (out->missing_bytes % median != 0) : 0;
        out->missing_nodes = additional > UINT32_MAX ? UINT32_MAX : (uint32_t)additional;
    }

    /* How long before it can answer, which is bytes over measured bandwidth
     * and nothing else. Unmeasured bandwidth means unknown, not a guess. */
    int all_measured = 1;
    for (uint32_t i = 0; i < out->nslices; i++) {
        const LmbClusterNode *nd = &nodes[out->slices[i].node];
        if (!out->slices[i].bytes_to_fetch) continue;
        if (!nd->lan_bps) { all_measured = 0; continue; }
        double secs = (double)out->slices[i].bytes_to_fetch /
                      (double)nd->lan_bps;
        if (secs > out->ready_seconds) out->ready_seconds = secs;
    }
    out->ready_known = all_measured;
    return 0;
}

static LMB_UNUSED int lmb_plan_cluster(const LmbModelShape *m,
    const LmbClusterNode *nodes, uint32_t n, uint32_t context,
    uint32_t sessions, LmbPlanGoal goal, LmbClusterPlan *out) {
    return lmb_plan_cluster_source(m, nodes, n, context, sessions, goal, 0, out);
}

/* Household resident admission uses the same process budgets as launch.
 * Keep this separate from adapter arithmetic: guard overhead is neither a
 * tensor nor a measured working set. No speed/optimality claim is made. */
static LMB_UNUSED int lmb_home_plan_budgets(const LmbModelShape *shape,
    uint64_t checkpoint_bytes, const LmbClusterNode *nodes, uint32_t count,
    uint32_t context, LmbClusterPlan *plan) {
    if (!shape || !nodes || !plan || !count || count > LMB_CLUSTER_MAX_NODES ||
        !plan->nslices || plan->nslices > count || plan->edge_node >= count ||
        plan->sessions != 1 || !plan->data_available) return -1;
    LmbClusterPlan updated = *plan;
    LmbClusterPlan *destination = plan;
    plan = &updated; /* Invalid input never leaves a partially updated plan. */
    uint32_t next = 0, has_edge = 0;
    uint64_t total_budget = 0, missing = 0;
    for (uint32_t i = 0; i < count; i++)
        total_budget = lmb_budget_add(total_budget, nodes[i].ram_budget_bytes);
    for (uint32_t i = 0; i < plan->nslices; i++) {
        LmbSlice *s = &plan->slices[i];
        if (s->node >= count || s->layer_begin != next) return -1;
        for (uint32_t j = 0; j < i; j++)
            if (plan->slices[j].node == s->node) return -1;
        LmbHomeReservation r;
        int edge = s->node == plan->edge_node;
        if (lmb_home_reservation(shape, checkpoint_bytes, s->layer_begin,
                                s->layer_end, context, edge, &r)) return -1;
        next = s->layer_end; has_edge += (uint32_t)edge;
        s->bytes_resident = r.total_bytes;
        s->state = r.total_bytes <= nodes[s->node].ram_budget_bytes ?
                   LMB_PLAN_RESIDENT : LMB_PLAN_UNRUNNABLE;
        if (s->state == LMB_PLAN_UNRUNNABLE)
            missing = lmb_budget_add(missing, r.total_bytes - nodes[s->node].ram_budget_bytes);
    }
    if (next != shape->layers || has_edge != 1) return -1;
    plan->missing_bytes = missing;
    plan->missing_nodes = 0;
    uint64_t average = total_budget / count;
    if (missing && average) {
        uint64_t additional = missing / average + (missing % average != 0);
        plan->missing_nodes = additional > UINT32_MAX ? UINT32_MAX : (uint32_t)additional;
    }
    plan->state = missing ? LMB_PLAN_UNRUNNABLE : LMB_PLAN_RESIDENT;
    *destination = updated;
    return 0;
}

/* A failed proportional split is not proof that the selected computers cannot
 * hold the model. Fixed per-process floors and Edge can make that split fail
 * even when another contiguous assignment fits. Preserve successful plans;
 * otherwise try each Edge owner first, followed by descending/ascending RAM.
 * This bounded greedy search is NOT an exhaustive solver or a speed optimizer.
 * Only the selected nodes are candidates; an unused selection receives no
 * offer. Edge must own at least one layer in the household protocol. */
static LMB_UNUSED int lmb_home_plan_source(const LmbModelShape *shape,
    uint64_t checkpoint_bytes, const LmbClusterNode *nodes, uint32_t count,
    uint32_t context, uint32_t sessions, LmbPlanGoal goal,
    int external_checkpoint, LmbClusterPlan *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->state = LMB_PLAN_UNRUNNABLE;
    out->goal = goal; out->sessions = sessions;
    if (!shape || !nodes || !checkpoint_bytes || !count ||
        count > LMB_CLUSTER_MAX_NODES || sessions != 1) return -1;
    LmbClusterPlan proportional;
    int original = lmb_plan_cluster_source(shape, nodes, count, context,
        sessions, goal, external_checkpoint, &proportional);
    if (!original) original = lmb_home_plan_budgets(shape, checkpoint_bytes,
        nodes, count, context, &proportional);
    if (!original) {
        *out = proportional;
        if (out->state == LMB_PLAN_RESIDENT) return 0;
    }
    int available = external_checkpoint != 0;
    for (uint32_t i = 0; i < count; i++) available |= nodes[i].has_checkpoint != 0;
    if (!available || !shape->layers || !shape->sizing_verified) return -1;

    for (uint32_t owner = 0; owner < count; owner++) {
        for (int ascending = 0; ascending < 2; ascending++) {
            uint32_t order[LMB_CLUSTER_MAX_NODES], used = 1;
            order[0] = owner;
            for (uint32_t i = 0; i < count; i++) if (i != owner) {
                uint32_t pos = used++;
                while (pos > 1 && (ascending ?
                    nodes[order[pos - 1]].ram_budget_bytes > nodes[i].ram_budget_bytes :
                    nodes[order[pos - 1]].ram_budget_bytes < nodes[i].ram_budget_bytes)) {
                    order[pos] = order[pos - 1]; pos--;
                }
                order[pos] = i;
            }
            LmbClusterPlan candidate = {0};
            candidate.goal = goal; candidate.sessions = 1;
            candidate.edge_node = owner; candidate.data_available = 1;
            uint32_t next = 0;
            for (uint32_t j = 0; j < count && next < shape->layers; j++) {
                uint32_t nd = order[j], lo = next, hi = shape->layers;
                /* All current resident contracts have nonnegative layer
                 * costs and fixed scratch. Their cost for [next,end) is
                 * monotone; an overflow is not a fit. The final shared
                 * validator independently checks every chosen reservation. */
                while (lo < hi) {
                    uint32_t mid = lo + (hi - lo) / 2 + (hi - lo) % 2;
                    LmbHomeReservation r;
                    int fits = !lmb_home_reservation(shape, checkpoint_bytes,
                        next, mid, context, nd == owner, &r) &&
                        r.total_bytes <= nodes[nd].ram_budget_bytes;
                    if (fits) lo = mid; else hi = mid - 1;
                }
                if (lo == next) {
                    if (nd == owner) break; /* Edge cannot be an empty slice. */
                    continue;
                }
                LmbSlice *s = &candidate.slices[candidate.nslices++];
                s->node = nd; s->layer_begin = next; s->layer_end = lo;
                next = lo;
            }
            if (next != shape->layers || lmb_home_plan_budgets(shape,
                checkpoint_bytes, nodes, count, context, &candidate) ||
                candidate.state != LMB_PLAN_RESIDENT) continue;
            /* Preparation metadata follows this candidate, not the rejected
             * proportional plan. These are adapter weight estimates, not
             * measured wire bytes or a generation speed. */
            candidate.ready_known = 1;
            for (uint32_t j = 0; j < candidate.nslices; j++) {
                LmbSlice *s = &candidate.slices[j];
                const LmbClusterNode *nd = &nodes[s->node];
                if (nd->has_checkpoint) continue;
                LmbRangeCost cost = lmb_estimate_segment(shape, s->layer_begin,
                    s->layer_end, context, 1);
                if (!cost.ok) return -1;
                s->bytes_to_fetch = cost.resident_bytes;
                if (s->node == owner) {
                    LmbRangeCost edge = lmb_estimate_edge(shape, context, 1);
                    if (!edge.ok) return -1;
                    s->bytes_to_fetch = lmb_size_add(s->bytes_to_fetch, edge.resident_bytes);
                }
                candidate.fetch_bytes = lmb_size_add(candidate.fetch_bytes, s->bytes_to_fetch);
                if (candidate.fetch_bytes == UINT64_MAX) return -1;
                if (!s->bytes_to_fetch) continue;
                if (!nd->lan_bps) candidate.ready_known = 0;
                else {
                    double seconds = (double)s->bytes_to_fetch / nd->lan_bps;
                    if (seconds > candidate.ready_seconds) candidate.ready_seconds = seconds;
                }
            }
            *out = candidate;
            return 0;
        }
    }
    /* Retain a valid unsuccessful proportional plan for deficit display.
     * A failed search means "no plan found", never a feasibility proof. */
    return original;
}

/* Would adding this machine help, and at what?
 *
 * Three different answers, and collapsing them is how a cluster tells
 * someone their laptop made things faster when it did not. A node that
 * completes the coverage belongs in the chain even if it slows every
 * stage down — without it there is nothing to slow down. */
typedef struct {
    int makes_runnable;       /* it was unrunnable, now it is not */
    int64_t missing_delta;    /* how much of the shortfall it removes */
    int enters_critical_path; /* it should carry layers */
    const char *reason;
} LmbNodeEffect;

static LMB_UNUSED LmbNodeEffect lmb_node_effect(const LmbModelShape *m,
                                                const LmbClusterNode *nodes,
                                                uint32_t n, uint32_t context,
                                                uint32_t sessions,
                                                LmbPlanGoal goal) {
    LmbNodeEffect e;
    memset(&e, 0, sizeof e);
    if (!n) { e.reason = "no machines"; return e; }
    LmbClusterPlan without, with;
    int ok_without = n > 1 &&
        !lmb_plan_cluster(m, nodes, n - 1, context, sessions, goal, &without);
    int ok_with =
        !lmb_plan_cluster(m, nodes, n, context, sessions, goal, &with);
    if (!ok_with) { e.reason = "the model still does not fit"; return e; }
    if (!ok_without || without.state == LMB_PLAN_UNRUNNABLE) {
        if (with.state != LMB_PLAN_UNRUNNABLE) {
            e.makes_runnable = 1;
            e.enters_critical_path = 1;
            e.reason = "it completes the coverage: without it the model "
                       "does not run at all";
            e.missing_delta = ok_without ? (int64_t)without.missing_bytes : 0;
            return e;
        }
        e.missing_delta = ok_without
            ? (int64_t)without.missing_bytes - (int64_t)with.missing_bytes : 0;
        e.reason = "it reduces what is missing, but the model still does not fit";
        return e;
    }
    /* Already runnable: the node has to earn its place in the chain. With no
     * calibration there is nothing to compare, and a plan may not invent
     * one — so it carries no layers until a measurement says it should. */
    e.enters_critical_path = 0;
    e.reason = "the model already runs: this machine adds capacity, replicas "
               "and failover, and enters the chain only if a measurement "
               "shows it lowers the time";
    return e;
}

#endif /* LUMABRI_CLUSTER_H */
