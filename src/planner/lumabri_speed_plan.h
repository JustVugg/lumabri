/* A placement chosen for speed, not only for fit.
 *
 * Splitting a model across computers does not remove work: every layer still
 * runs once per token, and each computer that is not the chat host adds a
 * network round trip. A split is therefore worth choosing only when no single
 * computer can hold the model, or when a faster computer that is too small to
 * hold all of it can take part of the work. This module estimates tokens per
 * second for candidate placements and never selects a split that it cannot
 * show to be faster than the best single computer able to hold the model.
 *
 * The estimate for one decoded token of one active conversation is
 *
 *   seconds = edge(E) + sum over ranges layers(n, range) + sum over ranges
 *             held by a computer other than E of hop(E, n)
 *   layers(n, range) = observed seconds per layer on n * layer count, when a
 *                      current calibration of THIS checkpoint supplies it;
 *                      otherwise scale * active bytes(range) / segment rate(n)
 *   edge(E)          = scale * edge resident bytes / edge rate(E)
 *   hop(E, n)        = round trip(E, n) + 2 * boundary bytes / link rate(E, n)
 *   tokens/s         = 1 / seconds
 *
 * "rate" is the machine probe's weight-streaming rate for that role. "scale"
 * is 1 until a computer has both an observation of this checkpoint and a
 * probe; their ratio then corrects every probe-only computer. A placement
 * that was measured exactly as it stands uses that measurement instead.
 * A missing term makes the candidate unestimated; it is never guessed.
 *
 * Nothing here loads a model, sends an offer or creates an approval. */
#ifndef LUMABRI_SPEED_PLAN_H
#define LUMABRI_SPEED_PLAN_H
#include <stdio.h>
#include "lumabri_stage_placement.h"
#include "lumabri_machine_speed.h"

#define LMB_SPEED_CANDIDATES 64u
#define LMB_SPEED_POOL 6u            /* computers considered for a split */
#define LMB_SPEED_SPLIT_MAX 4u       /* computers in one split candidate */
#define LMB_SPEED_SPLIT_MARGIN 1.05  /* a split must beat one computer by 5% */
#define LMB_SPEED_SWITCH_MARGIN 1.10 /* re-planning proposes only above 10% */
#define LMB_SPEED_TIE 0.10           /* single computers this close are equal */
/* Above this round trip a hop costs about as much as a layer range does on a
 * fast computer: the link, not the computers, then sets the speed. */
#define LMB_SPEED_SLOW_LINK_SECONDS 0.025

enum { LMB_SPEED_BASIS_PROBE = 1u, LMB_SPEED_BASIS_OBSERVED = 2u, LMB_SPEED_BASIS_MEASURED_PLAN = 4u };

typedef struct {
    LmbClusterNode node;      /* RAM budget, VRAM fact, threads, roles */
    LmbMachineSpeed speed;    /* bounded machine probe, per role */
    double layer_seconds;     /* observed on this checkpoint; 0 = no observation */
} LmbSpeedMachine;

/* Directed observation from one computer to another; zero = unmeasured. */
typedef struct {
    double rtt_seconds, bytes_per_second;
} LmbSpeedLink;

typedef struct {
    const LmbModelShape *shape;
    uint64_t checkpoint_bytes;
    uint32_t context, sessions;
    uint32_t prompt_tokens;   /* reference prompt for the prefill link cost */
} LmbSpeedModel;

/* A placement whose decode rate was actually measured, in machine indices. */
typedef struct {
    LmbClusterPlan plan;
    double tok_s;
} LmbSpeedMeasured;

typedef struct {
    LmbClusterPlan plan;      /* slice and Edge nodes are machine indices */
    uint32_t machines, basis;
    int estimated;            /* every term came from a measurement */
    double tok_s, seconds_per_token;
    double layer_seconds, edge_seconds, link_seconds;
    double prefill_link_seconds; /* hops for the reference prompt; 0 = none or unknown */
    double slowest_link_seconds; /* longest measured round trip among the hops; 0 = none */
    uint32_t slowest_link_node;  /* the computer at the far end of it */
    double tok_s_all_sessions_busy; /* one turn at a time: rate / sessions */
    uint64_t reserved_bytes;
} LmbSpeedCandidate;

typedef enum {
    LMB_SPEED_OK = 0, LMB_SPEED_NO_MEMORY, LMB_SPEED_NO_PLACEMENT, LMB_SPEED_NO_MACHINE,
    LMB_SPEED_INVALID
} LmbSpeedRefusal;

typedef enum {
    LMB_SPEED_WHY_ONLY_FIT = 0,     /* one computer; nothing else could be compared */
    LMB_SPEED_WHY_FASTEST_SINGLE,   /* one computer; estimated fastest */
    LMB_SPEED_WHY_SINGLE_UNPROVEN,  /* one computer; a split was not shown faster */
    LMB_SPEED_WHY_SPLIT_REQUIRED,   /* no single computer holds the model */
    LMB_SPEED_WHY_SPLIT_FASTER      /* estimated faster than the best single */
} LmbSpeedWhy;

typedef struct {
    LmbSpeedCandidate candidates[LMB_SPEED_CANDIDATES];
    uint32_t count, examined, bounded;
    int chosen, best_single, best_split, incumbent;   /* candidate index or -1 */
    uint32_t fits_single, unmeasured_mask;
    double probe_scale;
    LmbSpeedRefusal refusal;
    LmbSpeedWhy why;
    uint64_t needed_bytes, offered_bytes, largest_offer_bytes;
    char reason[640];
} LmbSpeedDecision;

typedef enum { LMB_REPLAN_KEEP = 0, LMB_REPLAN_PROPOSE, LMB_REPLAN_BROKEN } LmbReplan;

static LMB_UNUSED int lmb_speed_machine_eligible(const LmbSpeedMachine *m) {
    return m->node.ram_budget_bytes && m->node.threads;
}

/* Weight bytes one decoded token actually streams through in [begin, end):
 * everything dense, and only the routed top-k of the experts. */
static LMB_UNUSED double lmb_speed_active_bytes(const LmbSpeedModel *model,
    uint32_t begin, uint32_t end) {
    const LmbModelShape *m = model->shape;
    LmbRangeCost c = lmb_estimate_segment(m, begin, end, model->context, 1);
    if (!c.ok) return 0;
    if (!m->memory_contract) return (double)c.working_set_bytes;
    uint64_t slot[3] = {0};
    for (uint32_t i = begin; i < end; i++)
        for (unsigned k = 0; k < 3; k++)
            if (m->memory[i].expert_slot_bytes[k] > slot[k]) slot[k] = m->memory[i].expert_slot_bytes[k];
    double per_expert = (double)slot[0] + (double)slot[1] + (double)slot[2];
    uint32_t topk = m->experts_per_tok && m->experts_per_tok < m->experts ? m->experts_per_tok : m->experts;
    double idle = per_expert * (double)(end - begin) * (double)(m->experts - topk);
    double active = (double)c.resident_bytes - idle;
    return active > 0 ? active : 0;
}

static LMB_UNUSED double lmb_speed_layers(const LmbSpeedMachine *machine,
    const LmbSpeedModel *model, double scale, uint32_t begin, uint32_t end, uint32_t *basis) {
    if (isfinite(machine->layer_seconds) && machine->layer_seconds > 0) {
        *basis |= LMB_SPEED_BASIS_OBSERVED;
        return machine->layer_seconds * (double)(end - begin);
    }
    double bytes = lmb_speed_active_bytes(model, begin, end);
    if (!(machine->speed.known & LMB_SPEED_SEGMENT) || !machine->speed.segment_bytes_per_s || bytes <= 0) return 0;
    *basis |= LMB_SPEED_BASIS_PROBE;
    return scale * bytes / (double)machine->speed.segment_bytes_per_s;
}

static LMB_UNUSED double lmb_speed_edge(const LmbSpeedMachine *machine,
    const LmbSpeedModel *model, double scale) {
    LmbRangeCost edge = lmb_estimate_edge(model->shape, model->context, 1);
    if (!edge.ok || !edge.resident_bytes || !(machine->speed.known & LMB_SPEED_EDGE) ||
        !machine->speed.edge_bytes_per_s) return 0;
    return scale * (double)edge.resident_bytes / (double)machine->speed.edge_bytes_per_s;
}

static LMB_UNUSED const LmbSpeedLink *lmb_speed_link(const LmbSpeedLink *links,
    uint32_t count, uint32_t from, uint32_t to) {
    if (!links) return NULL;
    const LmbSpeedLink *direct = &links[(size_t)from * count + to], *reverse = &links[(size_t)to * count + from];
#define LMB_SPEED_LINK_OK(l) (isfinite((l)->rtt_seconds) && (l)->rtt_seconds > 0 && \
    isfinite((l)->bytes_per_second) && (l)->bytes_per_second > 0)
    if (LMB_SPEED_LINK_OK(direct)) return direct;
    return LMB_SPEED_LINK_OK(reverse) ? reverse : NULL;
#undef LMB_SPEED_LINK_OK
}

static LMB_UNUSED int lmb_speed_same_plan(const LmbClusterPlan *a, const LmbClusterPlan *b) {
    if (a->nslices != b->nslices || a->edge_node != b->edge_node || a->hybrid != b->hybrid) return 0;
    for (uint32_t i = 0; i < a->nslices; i++)
        if (a->slices[i].node != b->slices[i].node || a->slices[i].layer_begin != b->slices[i].layer_begin ||
            a->slices[i].layer_end != b->slices[i].layer_end) return 0;
    return 1;
}

/* Fill the estimate of one memory-validated candidate. */
static LMB_UNUSED void lmb_speed_estimate(const LmbSpeedMachine *machines, uint32_t count,
    const LmbSpeedLink *links, const LmbSpeedModel *model, double scale,
    const LmbSpeedMeasured *measured, uint32_t nmeasured, LmbSpeedCandidate *c) {
    const LmbClusterPlan *p = &c->plan;
    c->machines = p->nslices; c->basis = 0; c->estimated = 1;
    c->layer_seconds = c->edge_seconds = c->link_seconds = c->prefill_link_seconds = 0;
    c->slowest_link_seconds = 0; c->slowest_link_node = 0;
    c->reserved_bytes = 0;
    double width = (double)(model->shape->boundary_width ? model->shape->boundary_width : model->shape->hidden) * 4.0;
    int prefill_known = model->prompt_tokens != 0;
    for (uint32_t i = 0; i < p->nslices; i++) {
        const LmbSlice *s = &p->slices[i];
        c->reserved_bytes = lmb_size_add(c->reserved_bytes, s->bytes_resident);
        double seconds = lmb_speed_layers(&machines[s->node], model, scale, s->layer_begin, s->layer_end, &c->basis);
        if (seconds <= 0) c->estimated = 0;
        c->layer_seconds += seconds;
        if (s->node == p->edge_node) continue;
        const LmbSpeedLink *link = lmb_speed_link(links, count, p->edge_node, s->node);
        if (!link) { c->estimated = 0; prefill_known = 0; continue; }
        c->link_seconds += link->rtt_seconds + 2.0 * width / link->bytes_per_second;
        if (link->rtt_seconds > c->slowest_link_seconds) {
            c->slowest_link_seconds = link->rtt_seconds; c->slowest_link_node = s->node;
        }
        c->prefill_link_seconds += link->rtt_seconds +
            2.0 * width * (double)model->prompt_tokens / link->bytes_per_second;
    }
    if (!prefill_known) c->prefill_link_seconds = 0;
    c->edge_seconds = lmb_speed_edge(&machines[p->edge_node], model, scale);
    if (c->edge_seconds <= 0) c->estimated = 0;
    c->seconds_per_token = c->layer_seconds + c->edge_seconds + c->link_seconds;
    if (!isfinite(c->seconds_per_token) || c->seconds_per_token <= 0) c->estimated = 0;
    c->tok_s = c->estimated ? 1.0 / c->seconds_per_token : 0;
    for (uint32_t i = 0; i < nmeasured; i++)
        if (lmb_speed_same_plan(&measured[i].plan, p) && isfinite(measured[i].tok_s) && measured[i].tok_s > 0) {
            c->estimated = 1; c->basis = LMB_SPEED_BASIS_MEASURED_PLAN;
            c->tok_s = measured[i].tok_s; c->seconds_per_token = 1.0 / c->tok_s;
        }
    if (!c->estimated) c->seconds_per_token = 0;
    c->tok_s_all_sessions_busy = c->estimated ? c->tok_s / (double)(model->sessions ? model->sessions : 1) : 0;
}

/* Strict order used for splits and for the final pick among equals. */
static LMB_UNUSED int lmb_speed_better(const LmbSpeedCandidate *a, const LmbSpeedCandidate *b) {
    if (a->estimated != b->estimated) return a->estimated;
    if (a->estimated && a->tok_s != b->tok_s) return a->tok_s > b->tok_s;
    if (a->machines != b->machines) return a->machines < b->machines;
    return a->reserved_bytes < b->reserved_bytes;
}

static LMB_UNUSED int lmb_speed_add(LmbSpeedDecision *d, const LmbSpeedCandidate *c) {
    for (uint32_t i = 0; i < d->count; i++)
        if (lmb_speed_same_plan(&d->candidates[i].plan, &c->plan)) return (int)i;
    if (d->count < LMB_SPEED_CANDIDATES) { d->candidates[d->count] = *c; return (int)d->count++; }
    /* Full: a single-computer candidate is never evicted. */
    int worst = -1;
    for (uint32_t i = 0; i < d->count; i++)
        if (d->candidates[i].machines > 1 && (int)i != d->incumbent &&
            (worst < 0 || lmb_speed_better(&d->candidates[worst], &d->candidates[i]))) worst = (int)i;
    d->bounded = 1;
    if (worst < 0 || (c->machines > 1 && !lmb_speed_better(c, &d->candidates[worst]))) return -1;
    d->candidates[worst] = *c; return worst;
}

static LMB_UNUSED void lmb_speed_names(const LmbSpeedMachine *machines, const LmbClusterPlan *p,
    char *out, size_t cap) {
    size_t at = 0; out[0] = 0;
    for (uint32_t i = 0; i < p->nslices && at < cap; i++) {
        int n = snprintf(out + at, cap - at, "%s%.40s", i ? " + " : "", machines[p->slices[i].node].node.name);
        if (n < 0 || (size_t)n >= cap - at) break;
        at += (size_t)n;
    }
}

static LMB_UNUSED void lmb_speed_explain(LmbSpeedDecision *d, const LmbSpeedMachine *machines) {
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    const LmbSpeedCandidate *single = d->best_single >= 0 ? &d->candidates[d->best_single] : NULL;
    const LmbSpeedCandidate *split = d->best_split >= 0 ? &d->candidates[d->best_split] : NULL;
    char names[120], other[120], speed[48] = "speed not measured yet";
    lmb_speed_names(machines, &c->plan, names, sizeof names);
    if (c->estimated) snprintf(speed, sizeof speed, "estimated %.2f tok/s", c->tok_s);
    switch (d->why) {
    case LMB_SPEED_WHY_SPLIT_REQUIRED:
        snprintf(d->reason, sizeof d->reason, "No single computer can hold this model (largest offer %.2f GB). "
            "Split across %s: %s. Each computer other than the chat host adds a network round trip per token.",
            d->largest_offer_bytes / 1e9, names, speed);
        break;
    case LMB_SPEED_WHY_SPLIT_FASTER:
        lmb_speed_names(machines, &single->plan, other, sizeof other);
        snprintf(d->reason, sizeof d->reason, "Split across %s: %s, faster than the best single computer "
            "(%s, %.2f tok/s) including %.1f ms of network per token.", names, speed, other, single->tok_s,
            c->link_seconds * 1e3);
        break;
    case LMB_SPEED_WHY_FASTEST_SINGLE:
        if (split && split->estimated) {
            lmb_speed_names(machines, &split->plan, other, sizeof other);
            snprintf(d->reason, sizeof d->reason, "Whole model on %s: %s. Splitting across %s is estimated at "
                "%.2f tok/s, which is not faster: a split adds network hops without removing work.",
                names, speed, other, split->tok_s);
        } else snprintf(d->reason, sizeof d->reason, "Whole model on %s: %s. It is the fastest computer that "
            "can hold the model; one computer avoids every network hop.", names, speed);
        break;
    case LMB_SPEED_WHY_SINGLE_UNPROVEN:
        snprintf(d->reason, sizeof d->reason, "Whole model on %s: %s. A split is chosen only when it is "
            "estimated faster than one computer, and that needs a speed and link measurement of every "
            "participant.", names, speed);
        break;
    default:
        snprintf(d->reason, sizeof d->reason, "Whole model on %s: %s. It is the only computer that can hold "
            "the model.", names, speed);
    }
    /* Say so when a hop of the chosen split is slow enough to set the pace. */
    if (c->machines > 1 && c->slowest_link_seconds > LMB_SPEED_SLOW_LINK_SECONDS) {
        size_t used = strlen(d->reason);
        snprintf(d->reason + used, sizeof d->reason - used, " The link to %.40s takes %.0f ms per round trip: "
            "above about %.0f ms the network, not the computers, limits the speed.",
            machines[c->slowest_link_node].node.name, c->slowest_link_seconds * 1e3,
            LMB_SPEED_SLOW_LINK_SECONDS * 1e3);
    }
}

/* Choose among validated candidates. The rule that matters: with at least one
 * single-computer candidate, a split wins only when both are estimated and
 * the split is faster by the margin. */
static LMB_UNUSED void lmb_speed_choose(LmbSpeedDecision *d, const LmbSpeedMachine *machines) {
    d->chosen = d->best_single = d->best_split = -1;
    double fastest = 0;
    for (uint32_t i = 0; i < d->count; i++) {
        const LmbSpeedCandidate *c = &d->candidates[i];
        if (c->machines == 1 && c->estimated && c->tok_s > fastest) fastest = c->tok_s;
    }
    for (uint32_t i = 0; i < d->count; i++) {
        const LmbSpeedCandidate *c = &d->candidates[i];
        if (c->machines > 1) {
            const LmbSpeedCandidate *b = d->best_split < 0 ? NULL : &d->candidates[d->best_split];
            /* With nothing to compare, a placement already found or running stays. */
            if (!b || (!c->estimated && !b->estimated ? (int)i == d->incumbent ||
                (d->best_split != d->incumbent && lmb_speed_better(c, b)) : lmb_speed_better(c, b)))
                d->best_split = (int)i;
            continue;
        }
        if (d->best_single < 0) { d->best_single = (int)i; continue; }
        const LmbSpeedCandidate *b = &d->candidates[d->best_single];
        /* Estimated beats unestimated. Estimates within the tie band, or two
         * unestimated computers, fall back to a stable order: the placement
         * already found or running, then more offered RAM, then more
         * threads, then inventory order. */
        int c_top = c->estimated && c->tok_s >= fastest * (1.0 - LMB_SPEED_TIE);
        int b_top = b->estimated && b->tok_s >= fastest * (1.0 - LMB_SPEED_TIE);
        const LmbClusterNode *cn = &machines[c->plan.edge_node].node, *bn = &machines[b->plan.edge_node].node;
        int stable = (int)i == d->incumbent ? 1 : d->best_single == d->incumbent ? 0 :
            cn->ram_budget_bytes != bn->ram_budget_bytes ? cn->ram_budget_bytes > bn->ram_budget_bytes :
            cn->threads > bn->threads;
        if (c->estimated != b->estimated) { if (c->estimated) d->best_single = (int)i; }
        else if (!c->estimated) { if (stable) d->best_single = (int)i; }
        else if (c_top != b_top) { if (c_top) d->best_single = (int)i; }
        else if (c_top ? stable : c->tok_s > b->tok_s) d->best_single = (int)i;
    }
    if (d->best_single < 0) {
        d->chosen = d->best_split; d->why = LMB_SPEED_WHY_SPLIT_REQUIRED;
    } else {
        const LmbSpeedCandidate *single = &d->candidates[d->best_single];
        const LmbSpeedCandidate *split = d->best_split >= 0 ? &d->candidates[d->best_split] : NULL;
        /* fastest, not the tie-broken pick, is what a split has to beat. */
        if (split && split->estimated && single->estimated && split->tok_s >= fastest * LMB_SPEED_SPLIT_MARGIN) {
            d->chosen = d->best_split; d->why = LMB_SPEED_WHY_SPLIT_FASTER;
        } else {
            d->chosen = d->best_single;
            d->why = single->estimated ? LMB_SPEED_WHY_FASTEST_SINGLE :
                d->fits_single == 1 && !split ? LMB_SPEED_WHY_ONLY_FIT : LMB_SPEED_WHY_SINGLE_UNPROVEN;
        }
    }
}

/* 0: a plan was chosen. 1: refused, with refusal and reason set. -1: invalid. */
static LMB_UNUSED int lmb_speed_decide(const LmbSpeedMachine *machines, uint32_t count,
    const LmbSpeedLink *links, const LmbSpeedModel *model,
    const LmbSpeedMeasured *measured, uint32_t nmeasured,
    const LmbClusterPlan *incumbent, LmbSpeedDecision *d) {
    if (!d) return -1;
    memset(d, 0, sizeof *d);
    d->chosen = d->best_single = d->best_split = d->incumbent = -1; d->probe_scale = 1.0;
    d->refusal = LMB_SPEED_INVALID;
    const LmbModelShape *shape = model ? model->shape : NULL;
    if (!machines || !count || count > LMB_CLUSTER_MAX_NODES || !shape || !shape->layers ||
        shape->layers > LMB_PLAN_LAYER_MAX || !model->checkpoint_bytes || !model->context ||
        !model->sessions || model->sessions > LMB_HOST_MAX_SESSIONS || (nmeasured && !measured)) {
        snprintf(d->reason, sizeof d->reason, "Invalid planning input."); return -1;
    }
    LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES];
    uint32_t eligible = 0;
    for (uint32_t i = 0; i < count; i++) {
        nodes[i] = machines[i].node;
        if (!lmb_speed_machine_eligible(&machines[i])) { nodes[i].ram_budget_bytes = 0; continue; }
        eligible++;
        d->offered_bytes = lmb_size_add(d->offered_bytes, nodes[i].ram_budget_bytes);
        if (nodes[i].ram_budget_bytes > d->largest_offer_bytes) d->largest_offer_bytes = nodes[i].ram_budget_bytes;
        if (!(machines[i].layer_seconds > 0) && (machines[i].speed.known & (LMB_SPEED_SEGMENT | LMB_SPEED_EDGE)) !=
            (LMB_SPEED_SEGMENT | LMB_SPEED_EDGE)) d->unmeasured_mask |= UINT32_C(1) << i;
    }
    LmbRangeCost whole = lmb_estimate_segment(shape, 0, shape->layers, model->context, model->sessions);
    LmbRangeCost edge = lmb_estimate_edge(shape, model->context, model->sessions);
    d->needed_bytes = lmb_size_add(lmb_size_add(whole.resident_bytes, whole.state_bytes),
        lmb_size_add(edge.resident_bytes, edge.state_bytes));
    if (!whole.ok || !edge.ok || d->needed_bytes == UINT64_MAX) {
        snprintf(d->reason, sizeof d->reason, "This checkpoint has no validated memory sizing; it cannot be placed.");
        return -1;
    }
    if (!eligible) {
        d->refusal = LMB_SPEED_NO_MACHINE;
        snprintf(d->reason, sizeof d->reason, "No computer is offering memory. Start sharing on at least one computer.");
        return 1;
    }
    /* One computer's own observation of this checkpoint against its probe
     * calibrates what the probe means for this model on the others. */
    double ratios[LMB_CLUSTER_MAX_NODES]; uint32_t nratios = 0;
    double whole_active = lmb_speed_active_bytes(model, 0, shape->layers);
    for (uint32_t i = 0; i < count && whole_active > 0; i++) {
        const LmbSpeedMachine *m = &machines[i];
        if (!(m->layer_seconds > 0) || !isfinite(m->layer_seconds) || !(m->speed.known & LMB_SPEED_SEGMENT) ||
            !m->speed.segment_bytes_per_s) continue;
        double ratio = m->layer_seconds * shape->layers / (whole_active / (double)m->speed.segment_bytes_per_s);
        if (isfinite(ratio) && ratio > 0) {
            uint32_t at = nratios++;
            while (at && ratios[at - 1] > ratio) { ratios[at] = ratios[at - 1]; at--; }
            ratios[at] = ratio;
        }
    }
    if (nratios) {
        d->probe_scale = ratios[nratios / 2];
        if (d->probe_scale < 0.02) d->probe_scale = 0.02;
        if (d->probe_scale > 50.0) d->probe_scale = 50.0;
    }
    LmbSpeedCandidate c;
#define LMB_SPEED_BASE(c) do { memset(&(c), 0, sizeof (c)); (c).plan.goal = LMB_GOAL_ONE_SESSION; \
    (c).plan.sessions = model->sessions; (c).plan.data_available = 1; } while (0)
    /* 1. Every computer alone. Always examined, never evicted. */
    for (uint32_t i = 0; i < count; i++) {
        if (!nodes[i].ram_budget_bytes) continue;
        LMB_SPEED_BASE(c);
        c.plan.edge_node = i; c.plan.nslices = 1;
        c.plan.slices[0] = (LmbSlice){.node = i, .layer_begin = 0, .layer_end = shape->layers};
        d->examined++;
        if (lmb_home_plan_budgets(shape, model->checkpoint_bytes, nodes, count, model->context, &c.plan) ||
            c.plan.state != LMB_PLAN_RESIDENT) continue;
        lmb_speed_estimate(machines, count, links, model, d->probe_scale, measured, nmeasured, &c);
        if (lmb_speed_add(d, &c) >= 0) d->fits_single++;
    }
    /* 2. Placements that were measured as they stand, and the incumbent. */
    for (uint32_t i = 0; i <= nmeasured; i++) {
        const LmbClusterPlan *given = i < nmeasured ? &measured[i].plan : incumbent;
        if (!given || !given->nslices || given->nslices > count || given->hybrid) continue;
        LMB_SPEED_BASE(c);
        c.plan.edge_node = given->edge_node; c.plan.nslices = given->nslices;
        int usable = given->edge_node < count;
        for (uint32_t j = 0; j < given->nslices && usable; j++) {
            c.plan.slices[j] = (LmbSlice){.node = given->slices[j].node,
                .layer_begin = given->slices[j].layer_begin, .layer_end = given->slices[j].layer_end};
            usable = given->slices[j].node < count && nodes[given->slices[j].node].ram_budget_bytes;
        }
        if (!usable || lmb_home_plan_budgets(shape, model->checkpoint_bytes, nodes, count, model->context, &c.plan) ||
            c.plan.state != LMB_PLAN_RESIDENT) continue;
        lmb_speed_estimate(machines, count, links, model, d->probe_scale, measured, nmeasured, &c);
        int at = lmb_speed_add(d, &c);
        if (i == nmeasured) d->incumbent = at;
    }
    /* 3. Splits over a bounded pool: the computers with the most memory. */
    uint32_t pool[LMB_SPEED_POOL], npool = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!nodes[i].ram_budget_bytes) continue;
        uint32_t at = npool < LMB_SPEED_POOL ? npool++ : LMB_SPEED_POOL;
        if (at == LMB_SPEED_POOL) {
            d->bounded = 1;
            if (nodes[pool[LMB_SPEED_POOL - 1]].ram_budget_bytes >= nodes[i].ram_budget_bytes) continue;
            at = LMB_SPEED_POOL - 1;
        }
        while (at && nodes[pool[at - 1]].ram_budget_bytes < nodes[i].ram_budget_bytes) { pool[at] = pool[at - 1]; at--; }
        pool[at] = i;
    }
    /* Each split costs a range search. With a computer that holds the model
     * alone, a split can only win when one of its members runs the layers
     * faster than that computer runs the whole token; the others are not
     * searched, except a few pairs kept to explain the choice. */
    double cube = (double)shape->layers * shape->layers * shape->layers;
    double afford = 1.5e8 / cube;
    uint32_t allowance = afford > 160.0 ? 160u : afford < 8.0 ? 8u : (uint32_t)afford;
    uint32_t runs = 0, shown = 0;
    double single_seconds = 0;
    for (uint32_t i = 0; i < d->count; i++)
        if (d->candidates[i].machines == 1 && d->candidates[i].estimated &&
            (!(single_seconds > 0) || d->candidates[i].seconds_per_token < single_seconds))
            single_seconds = d->candidates[i].seconds_per_token;
    double whole_layers[LMB_CLUSTER_MAX_NODES];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t unused = 0;
        whole_layers[i] = nodes[i].ram_budget_bytes ?
            lmb_speed_layers(&machines[i], model, d->probe_scale, 0, shape->layers, &unused) : 0;
    }
    for (uint32_t size = 2; size <= npool && size <= LMB_SPEED_SPLIT_MAX && size <= shape->layers; size++)
        for (uint32_t mask = 1; mask < (UINT32_C(1) << npool); mask++) {
            uint32_t bits = 0, members[LMB_SPEED_SPLIT_MAX];
            for (uint32_t b = 0; b < npool; b++) if (mask & (UINT32_C(1) << b)) {
                if (bits < LMB_SPEED_SPLIT_MAX) members[bits] = pool[b];
                bits++;
            }
            if (bits != size) continue;
            uint64_t together = 0;
            for (uint32_t b = 0; b < size; b++) together = lmb_size_add(together, nodes[members[b]].ram_budget_bytes);
            if (together < d->needed_bytes) continue;   /* necessary condition, not admission */
            if (d->fits_single) {
                double fastest_member = 0; int priced = 1;
                for (uint32_t b = 0; b < size; b++) {
                    double w = whole_layers[members[b]];
                    if (!(w > 0)) priced = 0;
                    else if (!(fastest_member > 0) || w < fastest_member) fastest_member = w;
                }
                int could_win = priced && single_seconds > 0 && fastest_member < single_seconds;
                if (!could_win && (size > 2 || shown >= 3)) continue;
                if (!could_win) shown++;
            }
            for (uint32_t e = 0; e < size; e++) {
                if (runs >= allowance) { d->bounded = 1; goto searched; }
                /* The chat host keeps the first range; the others follow,
                 * fastest first. Their order does not change the estimate. */
                LmbClusterNode subset[LMB_SPEED_SPLIT_MAX]; uint32_t map[LMB_SPEED_SPLIT_MAX], n = 0;
                double costs[LMB_SPEED_SPLIT_MAX] = {0}; int priced = 1;
                map[n++] = members[e];
                for (uint32_t b = 0; b < size; b++) if (b != e) map[n++] = members[b];
                for (uint32_t b = 0; b < size; b++) {
                    subset[b] = nodes[map[b]];
                    costs[b] = whole_layers[map[b]] / (double)shape->layers;
                    if (!(costs[b] > 0) || !isfinite(costs[b])) priced = 0;
                }
                LmbClusterPlan seed = {0}, planned = {0};
                seed.goal = LMB_GOAL_ONE_SESSION; seed.sessions = model->sessions; seed.data_available = 1;
                seed.edge_node = 0;
                runs++; d->examined++;
                if (lmb_home_plan_selected(shape, model->checkpoint_bytes, subset, size, model->context,
                    &seed, priced ? costs : NULL, &planned) || planned.state != LMB_PLAN_RESIDENT) continue;
                LMB_SPEED_BASE(c);
                c.plan.edge_node = map[planned.edge_node]; c.plan.nslices = planned.nslices;
                for (uint32_t j = 0; j < planned.nslices; j++)
                    c.plan.slices[j] = (LmbSlice){.node = map[planned.slices[j].node],
                        .layer_begin = planned.slices[j].layer_begin, .layer_end = planned.slices[j].layer_end};
                if (lmb_home_plan_budgets(shape, model->checkpoint_bytes, nodes, count, model->context, &c.plan) ||
                    c.plan.state != LMB_PLAN_RESIDENT) continue;
                lmb_speed_estimate(machines, count, links, model, d->probe_scale, measured, nmeasured, &c);
                (void)lmb_speed_add(d, &c);
            }
        }
searched:
#undef LMB_SPEED_BASE
    if (!d->count) {
        if (d->offered_bytes < d->needed_bytes) {
            d->refusal = LMB_SPEED_NO_MEMORY;
            snprintf(d->reason, sizeof d->reason, "Not enough memory: this model needs at least %.2f GB resident "
                "and the %u available computer%s offer%s %.2f GB in total (%.2f GB short). Add a computer or "
                "offer more memory; weights are never run from disk.", d->needed_bytes / 1e9, eligible,
                eligible == 1 ? "" : "s", eligible == 1 ? "s" : "", d->offered_bytes / 1e9,
                (d->needed_bytes - d->offered_bytes) / 1e9);
        } else {
            d->refusal = LMB_SPEED_NO_PLACEMENT;
            snprintf(d->reason, sizeof d->reason, "No placement found: the computers offer %.2f GB in total for a "
                "model needing %.2f GB, but each computer also pays a fixed cost per process and no "
                "contiguous split of at most %u computers fits. Offer more memory on the largest computer "
                "(%.2f GB now).%s", d->offered_bytes / 1e9, d->needed_bytes / 1e9, LMB_SPEED_SPLIT_MAX,
                d->largest_offer_bytes / 1e9, d->bounded ? " The search was bounded; this is not a proof." : "");
        }
        return 1;
    }
    lmb_speed_choose(d, machines);
    d->refusal = LMB_SPEED_OK;
    lmb_speed_explain(d, machines);
    return 0;
}

/* Re-planning never acts. With a working incumbent it proposes only above
 * the switch margin, and only when both sides are estimated; an incumbent
 * that no longer fits is reported as broken, still without unloading it. */
static LMB_UNUSED LmbReplan lmb_speed_replan(const LmbSpeedDecision *d, char *why, size_t cap) {
    if (why && cap) why[0] = 0;
    if (!d || d->chosen < 0) return LMB_REPLAN_KEEP;
    const LmbSpeedCandidate *best = &d->candidates[d->chosen];
    if (d->incumbent < 0) {
        if (why) snprintf(why, cap, "The current placement no longer fits the available computers. A new "
            "placement is proposed; nothing is loaded or unloaded without approval.");
        return LMB_REPLAN_BROKEN;
    }
    const LmbSpeedCandidate *now = &d->candidates[d->incumbent];
    if (d->incumbent == d->chosen) {
        if (why) snprintf(why, cap, "The current placement is still the fastest known one.");
        return LMB_REPLAN_KEEP;
    }
    if (best->estimated && now->estimated && best->tok_s >= now->tok_s * LMB_SPEED_SWITCH_MARGIN) {
        if (why) snprintf(why, cap, "A different placement is estimated at %.2f tok/s against %.2f tok/s now. "
            "This is a proposal: the working model stays loaded until its owner approves a change.",
            best->tok_s, now->tok_s);
        return LMB_REPLAN_PROPOSE;
    }
    if (why) snprintf(why, cap, "%s", best->estimated && now->estimated ?
        "A different placement is not faster by a clear margin; the working model is kept." :
        "The current placement works and no measured alternative is clearly faster; it is kept.");
    return LMB_REPLAN_KEEP;
}
#endif
