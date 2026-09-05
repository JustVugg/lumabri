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
static LMB_UNUSED int lmb_plan_cluster(const LmbModelShape *m,
                                       const LmbClusterNode *nodes, uint32_t n,
                                       uint32_t context, uint32_t sessions,
                                       LmbPlanGoal goal,
                                       LmbClusterPlan *out) {
    memset(out, 0, sizeof *out);
    out->goal = goal;
    out->sessions = sessions ? sessions : 1;
    out->state = LMB_PLAN_UNRUNNABLE;
    if (!m->layers || !n || n > LMB_CLUSTER_MAX_NODES) return -1;

    /* Until an adapter declares a working GPU backend, RAM and VRAM are not
     * interchangeable. Counting both can accept a plan no engine can load. */
    LmbRangeCost edge = lmb_estimate_edge(m, context, sessions);
    if (!edge.ok) return -1;
    uint64_t edge_need = edge.resident_bytes + edge.state_bytes +
                         edge.scratch_bytes;
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
        total_budget += effective[i];
        if (nodes[i].has_checkpoint) out->data_available = 1;
    }
    if (!total_budget) return -1;

    /* Hand out layers in proportion to budget, in one pass, never leaving a
     * layer unassigned: the last node takes whatever rounding left over. */
    uint32_t assigned = 0;
    uint64_t whole_resident = 0;
    for (uint32_t i = 0; i < n && assigned < m->layers; i++) {
        uint64_t budget = effective[i];
        uint32_t take = (uint32_t)((uint64_t)m->layers * budget / total_budget);
        if (i + 1 == n) take = m->layers - assigned;
        if (!take) continue;
        if (assigned + take > m->layers) take = m->layers - assigned;

        LmbSlice *s = &out->slices[out->nslices];
        s->node = i;
        s->layer_begin = assigned;
        s->layer_end = assigned + take;
        LmbRangeCost c = lmb_estimate_segment(m, s->layer_begin, s->layer_end,
                                              context, sessions);
        uint64_t live = c.state_bytes + c.scratch_bytes;
        s->state = lmb_plan_state(m, &c, budget);
        s->bytes_resident = s->state == LMB_PLAN_DISK
                          ? c.working_set_bytes + live
                          : c.resident_bytes + live;
        uint64_t needed_weights = s->state == LMB_PLAN_DISK
                                ? c.working_set_bytes : c.resident_bytes;
        s->bytes_to_fetch = nodes[i].has_checkpoint ? 0 : needed_weights;
        whole_resident += c.resident_bytes + live;
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
            out->missing_bytes += need > budget ? need - budget : 0;
        }
        out->fetch_bytes += s->bytes_to_fetch;
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
        out->missing_bytes = whole_resident + edge.resident_bytes;
    }

    if (out->state == LMB_PLAN_UNRUNNABLE && out->missing_bytes) {
        uint64_t median = total_budget / n;
        out->missing_nodes = median ?
            (uint32_t)((out->missing_bytes + median - 1) / median) : 0;
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
