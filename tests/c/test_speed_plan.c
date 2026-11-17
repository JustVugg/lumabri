/* A planner that plans for speed: simulated mixed households, no model loaded. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "src/planner/lumabri_speed_plan.h"

static const uint64_t mib = UINT64_C(1) << 20;
static LmbModelShape shape;
static uint64_t checkpoint;
static LmbHomeReservation whole, half_edge, half_tail;

static void fixture(void) {
    memset(&shape, 0, sizeof shape);
    strcpy(shape.model_type, "olmoe"); strcpy(shape.segment_id, "olmoe");
    shape.layers = 8; shape.hidden = 1024; shape.vocab = 32000; shape.max_context = 4096;
    shape.memory_contract = shape.sizing_verified = 1; shape.edge_resident_bytes = 64 * mib;
    for (unsigned i = 0; i < 8; i++) shape.memory[i].resident_bytes = 400 * mib;
    checkpoint = (8 * 400 + 64) * mib;
    assert(!lmb_home_reservation_sessions(&shape, checkpoint, 0, 8, 256, 1, 1, &whole));
    assert(!lmb_home_reservation_sessions(&shape, checkpoint, 0, 4, 256, 1, 1, &half_edge));
    assert(!lmb_home_reservation_sessions(&shape, checkpoint, 4, 8, 256, 1, 0, &half_tail));
}

/* rate in MiB/s for both roles; 0 = this computer has no measurement. */
static LmbSpeedMachine machine(const char *name, uint64_t ram, uint64_t rate) {
    LmbSpeedMachine m = {0};
    snprintf(m.node.name, sizeof m.node.name, "%s", name);
    m.node.ram_budget_bytes = ram; m.node.threads = 4;
    if (rate) m.speed = (LmbMachineSpeed){.known = LMB_SPEED_SEGMENT | LMB_SPEED_EDGE,
        .segment_bytes_per_s = rate * mib, .edge_bytes_per_s = rate * mib, .threads = 4};
    return m;
}

static void lan(LmbSpeedLink *links, uint32_t n, double rtt, double rate) {
    for (uint32_t i = 0; i < n * n; i++) links[i] = (LmbSpeedLink){rtt, rate};
}

static void check_plan(const LmbSpeedMachine *machines, uint32_t n, const LmbSpeedModel *model,
                       const LmbSpeedCandidate *c) {
    LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES];
    for (uint32_t i = 0; i < n; i++) nodes[i] = machines[i].node;
    LmbClusterPlan again = c->plan;
    assert(!lmb_home_plan_budgets(model->shape, model->checkpoint_bytes, nodes, n, model->context, &again));
    assert(again.state == LMB_PLAN_RESIDENT);
    uint32_t next = 0, edge = 0;
    for (uint32_t i = 0; i < again.nslices; i++) {
        assert(again.slices[i].layer_begin == next); next = again.slices[i].layer_end;
        edge += again.slices[i].node == again.edge_node;
    }
    assert(next == model->shape->layers && edge == 1);
}

/* The rule: with a computer that holds the whole model, the chosen plan is
 * that computer, or a split that is estimated faster than every such one. */
static void never_slower(const LmbSpeedDecision *d) {
    double fastest = 0; int singles = 0;
    for (uint32_t i = 0; i < d->count; i++) if (d->candidates[i].machines == 1) {
        singles++;
        if (d->candidates[i].estimated && d->candidates[i].tok_s > fastest) fastest = d->candidates[i].tok_s;
    }
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    if (!singles) { assert(c->machines > 1 && d->why == LMB_SPEED_WHY_SPLIT_REQUIRED); return; }
    if (c->machines > 1) {
        assert(c->estimated && fastest > 0 && c->tok_s >= fastest * LMB_SPEED_SPLIT_MARGIN);
        assert(d->why == LMB_SPEED_WHY_SPLIT_FASTER);
    } else if (fastest > 0) {
        assert(c->estimated && c->tok_s >= fastest * (1.0 - LMB_SPEED_TIE) - 1e-12);
    }
}

static LmbSpeedDecision *decide(const LmbSpeedMachine *machines, uint32_t n, const LmbSpeedLink *links,
    const LmbSpeedModel *model, const LmbSpeedMeasured *measured, uint32_t nmeasured,
    const LmbClusterPlan *incumbent, int expected) {
    LmbSpeedDecision *d = malloc(sizeof *d), *again = malloc(sizeof *again);
    assert(d && again);
    assert(lmb_speed_decide(machines, n, links, model, measured, nmeasured, incumbent, d) == expected);
    assert(lmb_speed_decide(machines, n, links, model, measured, nmeasured, incumbent, again) == expected);
    assert(!memcmp(d, again, sizeof *d));   /* deterministic */
    free(again);
    if (!expected) {
        assert(d->chosen >= 0 && d->reason[0] && d->refusal == LMB_SPEED_OK);
        check_plan(machines, n, model, &d->candidates[d->chosen]);
        never_slower(d);
    } else assert(d->chosen < 0 && d->reason[0]);
    return d;
}

static void measured_small_model(void) {
    /* The measurement this rule comes from: two computers that each hold a
     * small model, and a two-computer split that was 10% slower. */
    LmbSpeedMachine m[2] = {machine("desk", 2 * whole.total_bytes, 2000), machine("laptop", 2 * whole.total_bytes, 1900)};
    LmbSpeedLink links[4]; lan(links, 2, 0.0006, 80e6);
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    LmbSpeedDecision *d = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    assert(c->machines == 1 && d->fits_single == 2 && d->why == LMB_SPEED_WHY_FASTEST_SINGLE);
    assert(d->best_split >= 0 && d->candidates[d->best_split].estimated);
    assert(d->candidates[d->best_split].tok_s < c->tok_s);
    assert(d->candidates[d->best_split].link_seconds > 0 && c->link_seconds == 0);
    assert(strstr(d->reason, "not faster"));
    free(d);
    /* The same household with the two placements measured as they stand. */
    LmbSpeedMeasured seen[2] = {0};
    seen[0].plan.nslices = 1; seen[0].plan.edge_node = 0;
    seen[0].plan.slices[0] = (LmbSlice){.node = 0, .layer_begin = 0, .layer_end = 8}; seen[0].tok_s = 10.0;
    seen[1].plan.nslices = 2; seen[1].plan.edge_node = 0;
    seen[1].plan.slices[0] = (LmbSlice){.node = 0, .layer_begin = 0, .layer_end = 4};
    seen[1].plan.slices[1] = (LmbSlice){.node = 1, .layer_begin = 4, .layer_end = 8}; seen[1].tok_s = 9.0;
    LmbSpeedMachine blind[2] = {machine("desk", 2 * whole.total_bytes, 0), machine("laptop", 2 * whole.total_bytes, 0)};
    d = decide(blind, 2, NULL, &model, seen, 2, NULL, 0);
    c = &d->candidates[d->chosen];
    assert(c->machines == 1 && c->plan.edge_node == 0 && c->basis == LMB_SPEED_BASIS_MEASURED_PLAN && c->tok_s == 10.0);
    free(d);
    /* Even a split measured FASTER than one computer by less than the margin stays out. */
    seen[1].tok_s = 10.3;
    d = decide(blind, 2, NULL, &model, seen, 2, NULL, 0);
    assert(d->candidates[d->chosen].machines == 1);
    free(d);
}

static void does_not_fit_one(void) {
    LmbSpeedMachine m[3] = {machine("desk", half_edge.total_bytes + 8 * mib, 2000),
        machine("laptop", half_tail.total_bytes + 8 * mib, 1000), machine("old", 64 * mib, 500)};
    LmbSpeedLink links[9]; lan(links, 3, 0.001, 50e6);
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 512};
    LmbSpeedDecision *d = decide(m, 3, links, &model, NULL, 0, NULL, 0);
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    assert(!d->fits_single && d->best_single < 0 && c->machines == 2 && d->why == LMB_SPEED_WHY_SPLIT_REQUIRED);
    assert(c->estimated && c->link_seconds > 0.001 && c->prefill_link_seconds > c->link_seconds);
    assert(c->plan.slices[0].layer_end == 4 && strstr(d->reason, "No single computer"));
    free(d);
    /* Without any measurement it is still placed, and says the speed is unknown. */
    for (unsigned i = 0; i < 3; i++) m[i].speed = (LmbMachineSpeed){0};
    d = decide(m, 3, NULL, &model, NULL, 0, NULL, 0);
    c = &d->candidates[d->chosen];
    assert(c->machines == 2 && !c->estimated && c->tok_s == 0 && d->unmeasured_mask == 7 &&
           strstr(d->reason, "not measured"));
    free(d);
}

static void fast_small_computer(void) {
    /* The big computer is slow; the fast one cannot hold the model. */
    LmbSpeedMachine m[2] = {machine("big", 2 * whole.total_bytes, 400), machine("fast", half_edge.total_bytes + 8 * mib, 4000)};
    LmbSpeedLink links[4]; lan(links, 2, 0.0005, 100e6);
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    LmbSpeedDecision *d = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    assert(d->fits_single == 1 && c->machines == 2 && d->why == LMB_SPEED_WHY_SPLIT_FASTER);
    assert(c->tok_s > d->candidates[d->best_single].tok_s * 1.2 && strstr(d->reason, "faster than the best single"));
    free(d);
    /* A slow link takes the gain away: one computer again. */
    lan(links, 2, 5.0, 100e6);
    d = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    assert(d->candidates[d->chosen].machines == 1 && d->candidates[d->chosen].plan.edge_node == 0);
    free(d);
    /* An unmeasured link is not assumed fast. */
    d = decide(m, 2, NULL, &model, NULL, 0, NULL, 0);
    assert(d->candidates[d->chosen].machines == 1 && !d->candidates[d->best_split].estimated);
    free(d);
    /* Nor is an unmeasured computer. */
    lan(links, 2, 0.0005, 100e6); m[1].speed = (LmbMachineSpeed){0};
    d = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    assert(d->candidates[d->chosen].machines == 1 && d->unmeasured_mask == 2);
    free(d);
}

static void weak_chat_computer(void) {
    /* The person chats from a slow laptop; the model belongs on the desk. */
    LmbSpeedMachine m[2] = {machine("laptop", 2 * whole.total_bytes, 300), machine("desk", 2 * whole.total_bytes, 3000)};
    LmbSpeedLink links[4]; lan(links, 2, 0.0005, 100e6);
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    LmbSpeedDecision *d = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    assert(d->candidates[d->chosen].machines == 1 && d->candidates[d->chosen].plan.edge_node == 1);
    free(d);
    /* Equal within the tie band: stable order, here more offered RAM. */
    m[0] = machine("a", 2 * whole.total_bytes, 1000); m[1] = machine("b", 3 * whole.total_bytes, 1010);
    d = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    assert(d->candidates[d->chosen].plan.edge_node == 1);
    m[1] = machine("b", 3 * whole.total_bytes, 990);
    LmbSpeedDecision *other = decide(m, 2, links, &model, NULL, 0, NULL, 0);
    assert(other->candidates[other->chosen].plan.edge_node == 1);
    free(d); free(other);
}

static void refusals_and_sessions(void) {
    LmbSpeedMachine m[2] = {machine("a", whole.total_bytes / 4, 1000), machine("b", whole.total_bytes / 4, 1000)};
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    LmbSpeedDecision *d = decide(m, 2, NULL, &model, NULL, 0, NULL, 1);
    assert(d->refusal == LMB_SPEED_NO_MEMORY && strstr(d->reason, "GB short") && d->needed_bytes > d->offered_bytes);
    free(d);
    m[0].node.ram_budget_bytes = m[1].node.ram_budget_bytes = 0;
    d = decide(m, 2, NULL, &model, NULL, 0, NULL, 1);
    assert(d->refusal == LMB_SPEED_NO_MACHINE);
    free(d);
    /* Total above the model, but every computer below one process floor. */
    LmbSpeedMachine many[5];
    for (unsigned i = 0; i < 5; i++) many[i] = machine("tiny", 150 * mib, 1000);
    many[0].node.ram_budget_bytes = (uint64_t)3200 * mib;
    d = decide(many, 5, NULL, &model, NULL, 0, NULL, 1);
    assert(d->refusal == LMB_SPEED_NO_PLACEMENT && d->offered_bytes >= d->needed_bytes);
    free(d);
    LmbSpeedDecision invalid;
    assert(lmb_speed_decide(NULL, 1, NULL, &model, NULL, 0, NULL, &invalid) == -1);
    model.sessions = 9;
    assert(lmb_speed_decide(m, 2, NULL, &model, NULL, 0, NULL, &invalid) == -1);

    /* More conversations need more memory, and share one turn at a time. */
    LmbHomeReservation eight;
    assert(!lmb_home_reservation_sessions(&shape, checkpoint, 0, 8, 256, 8, 1, &eight));
    assert(eight.total_bytes > whole.total_bytes);
    LmbSpeedMachine s[2] = {machine("a", whole.total_bytes, 1000), machine("b", whole.total_bytes, 1000)};
    LmbSpeedLink links[4]; lan(links, 2, 0.0005, 100e6);
    model.sessions = 1;
    d = decide(s, 2, links, &model, NULL, 0, NULL, 0);
    assert(d->candidates[d->chosen].machines == 1 &&
           d->candidates[d->chosen].tok_s_all_sessions_busy == d->candidates[d->chosen].tok_s);
    free(d);
    model.sessions = 8;
    d = decide(s, 2, links, &model, NULL, 0, NULL, 0);
    const LmbSpeedCandidate *c = &d->candidates[d->chosen];
    assert(!d->fits_single && c->machines == 2 && c->plan.sessions == 8);
    assert(fabs(c->tok_s_all_sessions_busy * 8 - c->tok_s) < 1e-9);
    free(d);
}

static void observation_corrects_probe(void) {
    /* "a" observed this checkpoint at twice the cost its probe predicts. */
    LmbSpeedMachine m[2] = {machine("a", 2 * whole.total_bytes, 1000), machine("b", 2 * whole.total_bytes, 1500)};
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    double predicted = lmb_speed_active_bytes(&model, 0, 8) / (1000.0 * mib);
    m[0].layer_seconds = 2.0 * predicted / 8;
    LmbSpeedDecision *d = decide(m, 2, NULL, &model, NULL, 0, NULL, 0);
    assert(fabs(d->probe_scale - 2.0) < 1e-9);
    const LmbSpeedCandidate *a = NULL, *b = NULL;
    for (uint32_t i = 0; i < d->count; i++) if (d->candidates[i].machines == 1)
        *(d->candidates[i].plan.edge_node ? &b : &a) = &d->candidates[i];
    assert(a && b && (a->basis & LMB_SPEED_BASIS_OBSERVED) && b->basis == LMB_SPEED_BASIS_PROBE);
    assert(fabs(a->layer_seconds - 2.0 * predicted) < 1e-9 && fabs(b->layer_seconds - 2.0 * predicted / 1.5) < 1e-9);
    assert(d->candidates[d->chosen].plan.edge_node == 1);
    free(d);
    /* Routed experts: only the top-k of each layer is streamed per token. */
    LmbModelShape moe = shape; moe.experts = 16; moe.experts_per_tok = 4;
    for (unsigned i = 0; i < 8; i++) { moe.memory[i].resident_bytes = 40 * mib; moe.memory[i].expert_slot_bytes[0] = 10 * mib; }
    LmbSpeedModel routed = {&moe, checkpoint, 256, 1, 0};
    assert(lmb_speed_active_bytes(&routed, 0, 8) == (double)(8 * (40 + 4 * 10) * mib));
}

static void replanning(void) {
    LmbSpeedMachine m[2] = {machine("slow", 2 * whole.total_bytes, 500), machine("fast", 2 * whole.total_bytes, 520)};
    LmbSpeedLink links[4]; lan(links, 2, 0.0005, 100e6);
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    LmbClusterPlan running = {0};
    running.nslices = 1; running.edge_node = 0;
    running.slices[0] = (LmbSlice){.node = 0, .layer_begin = 0, .layer_end = 8};
    char why[256];
    LmbSpeedDecision *d = decide(m, 2, links, &model, NULL, 0, &running, 0);
    assert(d->incumbent >= 0 && lmb_speed_replan(d, why, sizeof why) == LMB_REPLAN_KEEP && why[0]);
    free(d);
    /* A much faster computer joins: propose, never act. */
    m[1] = machine("fast", 2 * whole.total_bytes, 5000);
    d = decide(m, 2, links, &model, NULL, 0, &running, 0);
    assert(d->candidates[d->chosen].plan.edge_node == 1 && d->incumbent >= 0 && d->incumbent != d->chosen);
    assert(lmb_speed_replan(d, why, sizeof why) == LMB_REPLAN_PROPOSE && strstr(why, "proposal"));
    free(d);
    /* An unmeasured newcomer is no reason to move a working model. */
    m[1] = machine("new", 4 * whole.total_bytes, 0);
    d = decide(m, 2, links, &model, NULL, 0, &running, 0);
    assert(lmb_speed_replan(d, why, sizeof why) == LMB_REPLAN_KEEP);
    free(d);
    /* The computer holding the model left. */
    m[0].node.ram_budget_bytes = 0; m[1] = machine("fast", 2 * whole.total_bytes, 5000);
    d = decide(m, 2, links, &model, NULL, 0, &running, 0);
    assert(d->incumbent < 0 && lmb_speed_replan(d, why, sizeof why) == LMB_REPLAN_BROKEN && strstr(why, "approval"));
    free(d);
}

static void two_budgets(void) {
    LmbMemoryBudget need = {100, 0}, offer = {100, 0};
    assert(lmb_budget_admit(&need, &offer) == LMB_ADMIT_OK);
    need.ram_bytes = 101; offer.vram_bytes = UINT64_C(1) << 40;
    assert(lmb_budget_admit(&need, &offer) == LMB_ADMIT_RAM_SHORT);   /* VRAM never pays for RAM */
    need = (LmbMemoryBudget){50, 10}; offer = (LmbMemoryBudget){UINT64_C(1) << 40, 9};
    assert(lmb_budget_admit(&need, &offer) == LMB_ADMIT_VRAM_SHORT);  /* nor RAM for VRAM */
    offer.vram_bytes = 10; assert(lmb_budget_admit(&need, &offer) == LMB_ADMIT_OK);
    need = (LmbMemoryBudget){0, 0}; assert(lmb_budget_admit(&need, &offer) == LMB_ADMIT_INVALID);
    need = (LmbMemoryBudget){UINT64_MAX, 0}; assert(lmb_budget_admit(&need, &offer) == LMB_ADMIT_INVALID);
    assert(lmb_budget_admit(NULL, &offer) == LMB_ADMIT_INVALID && !whole.vram_bytes);
    /* The planner sees a large inventoried VRAM and still refuses on RAM. */
    LmbSpeedMachine m[1] = {machine("gpu box", whole.total_bytes / 2, 1000)};
    m[0].node.vram_budget_bytes = UINT64_C(64) << 30; m[0].node.gpu_backends = LMB_GPU_CUDA;
    LmbSpeedModel model = {&shape, checkpoint, 256, 1, 0};
    LmbSpeedDecision *d = decide(m, 1, NULL, &model, NULL, 0, NULL, 1);
    assert(d->refusal == LMB_SPEED_NO_MEMORY);
    free(d);
}

static uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }

static void random_households(void) {
    unsigned chosen_single = 0, chosen_split = 0, refused = 0;
    for (unsigned round = 0; round < 600; round++) {
        uint32_t n = 1 + (uint32_t)(next() % 7);
        LmbSpeedMachine m[7]; LmbSpeedLink links[49];
        for (uint32_t i = 0; i < n; i++) {
            char name[16]; snprintf(name, sizeof name, "pc%u", i);
            uint64_t ram = (uint64_t)(200 + next() % 4200) * mib;
            m[i] = machine(name, next() % 11 ? ram : 0, next() % 5 ? 100 + next() % 6000 : 0);
            if (!(next() % 6)) m[i].layer_seconds = 0.001 + (double)(next() % 1000) / 2000.0;
        }
        for (uint32_t i = 0; i < n * n; i++)
            links[i] = next() % 7 ? (LmbSpeedLink){0.0002 + (double)(next() % 400) / 10000.0, 1e6 + (double)(next() % 100) * 1e6} :
                (LmbSpeedLink){0, 0};
        LmbSpeedModel model = {&shape, checkpoint, 256, 1 + (uint32_t)(next() % 3), (uint32_t)(next() % 2000)};
        LmbSpeedDecision *d = malloc(sizeof *d); assert(d);
        int rc = lmb_speed_decide(m, n, next() % 5 ? links : NULL, &model, NULL, 0, NULL, d);
        assert(rc == 0 || rc == 1);
        if (rc) { refused++; assert(d->chosen < 0 && d->refusal != LMB_SPEED_OK && d->reason[0]); }
        else {
            check_plan(m, n, &model, &d->candidates[d->chosen]);
            never_slower(d);
            /* Every computer that holds the model alone is among the candidates. */
            uint32_t singles = 0;
            for (uint32_t i = 0; i < d->count; i++) singles += d->candidates[i].machines == 1;
            assert(singles == d->fits_single);
            *(d->candidates[d->chosen].machines == 1 ? &chosen_single : &chosen_split) += 1;
        }
        free(d);
    }
    assert(chosen_single > 50 && chosen_split > 50 && refused > 10);
    printf("random households: %u one computer, %u split, %u refused\n", chosen_single, chosen_split, refused);
}

int main(void) {
    fixture();
    measured_small_model();
    does_not_fit_one();
    fast_small_computer();
    weak_chat_computer();
    refusals_and_sessions();
    observation_corrects_probe();
    replanning();
    two_budgets();
    random_households();
    puts("speed plan: ok");
    return 0;
}
