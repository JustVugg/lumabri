/* A plan says what fits and how it would be split. It must never say how
 * fast, and it must never say "runs" when one machine in the chain cannot
 * hold its range. */
#include <stdio.h>
#include <string.h>
#include "lumabri_cluster.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); bad = 1; } } while (0)

static LmbModelShape v4(void) {
    LmbModelShape m; memset(&m, 0, sizeof m);
    snprintf(m.model_type, sizeof m.model_type, "deepseek_v4");
    m.layers = 43; m.hidden = 4096; m.intermediate = 11264;
    m.moe_intermediate = 1408; m.experts = 256; m.experts_per_tok = 6;
    m.heads = 32; m.kv_heads = 8; m.vocab = 129280; m.bits_per_weight = 4;
    return m;
}

static LmbClusterNode node(const char *name, double gb, uint32_t gpu) {
    LmbClusterNode n; memset(&n, 0, sizeof n);
    snprintf(n.name, sizeof n.name, "%s", name);
    n.ram_budget_bytes = (uint64_t)(gb * 1e9);
    n.gpu_backends = gpu; n.threads = 8; n.lan_bps = 100u * 1000 * 1000;
    return n;
}

int main(void) {
    LmbModelShape m = v4();
    LmbClusterPlan p;

    /* A realistic house: four computers, 96 GB between them, against a model
     * that needs about 150. It does not fit, and the plan has to SAY it does
     * not fit rather than produce a split that would fail on first use. */
    LmbClusterNode house[4] = { node("desk", 32, 0), node("study", 24, 0),
                                node("bedroom", 24, 0), node("server", 16, 0) };
    CHECK(!lmb_plan_cluster(&m, house, 4, 4096, 1, LMB_GOAL_ONE_SESSION, &p),
          "a four-machine house could not be planned at all");
    CHECK(p.state == LMB_PLAN_UNRUNNABLE,
          "96 GB was reported as able to hold a ~150 GB model (%s)",
          lmb_plan_state_name(p.state));
    CHECK(p.missing_bytes > 0, "nothing was reported as missing");
    CHECK(p.missing_nodes > 0,
          "the shortfall was not expressed in machines, which is the only "
          "form a person can act on");

    /* Coverage is not optional: every layer belongs to exactly one node, in
     * order, with no gap and no overlap. A plan that leaves layer 30
     * unassigned is not a slower plan, it is not a plan. */
    uint32_t next = 0;
    for (uint32_t i = 0; i < p.nslices; i++) {
        CHECK(p.slices[i].layer_begin == next,
              "slice %u starts at %u, expected %u — a gap or an overlap",
              i, p.slices[i].layer_begin, next);
        next = p.slices[i].layer_end;
    }
    CHECK(next == m.layers, "the slices cover %u of %u layers", next, m.layers);

    /* Shares follow budgets. Equal splits are the obvious thing and they are
     * wrong on the hardware a house has, where one machine is always bigger. */
    CHECK(p.nslices >= 2, "a four-node cluster produced %u slices", p.nslices);
    uint32_t big = p.slices[0].layer_end - p.slices[0].layer_begin;
    uint32_t small = p.slices[p.nslices - 1].layer_end -
                     p.slices[p.nslices - 1].layer_begin;
    CHECK(big >= small, "the 32 GB machine took %u layers, the 16 GB one %u",
          big, small);

    /* Enough machines, and it fits: same model, eight big boxes. */
    LmbClusterNode big8[8];
    for (int i = 0; i < 8; i++) big8[i] = node("big", 40, 0);
    CHECK(!lmb_plan_cluster(&m, big8, 8, 4096, 1, LMB_GOAL_ONE_SESSION, &p) &&
          p.state == LMB_PLAN_RESIDENT,
          "320 GB across eight machines did not hold a ~150 GB model (%s)",
          lmb_plan_state_name(p.state));
    CHECK(p.missing_bytes == 0, "a plan that fits still reported %.1f GB missing",
          (double)p.missing_bytes / 1e9);

    /* Edge goes to a machine whose ENGINE can use its card, not to whichever
     * machine happens to have one fitted. */
    LmbClusterNode mixed[3] = { node("cpu-big", 64, 0), node("gpu", 40, LMB_GPU_CUDA),
                                node("cpu", 40, 0) };
    CHECK(!lmb_plan_cluster(&m, mixed, 3, 4096, 1, LMB_GOAL_ONE_SESSION, &p),
          "a mixed cluster could not be planned");
    CHECK(p.edge_node == 1,
          "Edge went to node %u; the machine whose engine can use a GPU is 1",
          p.edge_node);

    /* Ready-in is bytes over MEASURED bandwidth. With none measured it is
     * unknown, and unknown has to be visible — a fabricated minute is how a
     * catalogue starts lying. */
    LmbClusterNode blind[8];
    for (int i = 0; i < 8; i++) { blind[i] = node("big", 40, 0); blind[i].lan_bps = 0; }
    CHECK(!lmb_plan_cluster(&m, blind, 8, 4096, 1, LMB_GOAL_ONE_SESSION, &p),
          "an unmeasured cluster could not be planned");
    CHECK(!p.ready_known, "ready-in was claimed with no bandwidth measured");
    for (int i = 0; i < 8; i++) blind[i].has_checkpoint = 1;
    CHECK(!lmb_plan_cluster(&m, blind, 8, 4096, 1, LMB_GOAL_ONE_SESSION, &p),
          "a cluster that already holds the weights could not be planned");
    CHECK(p.fetch_bytes == 0 && p.ready_known,
          "machines that already hold the checkpoint were told to fetch %.1f GB",
          (double)p.fetch_bytes / 1e9);

    /* The three answers about a new machine stay three. Sized so that four
     * of these do not hold the model and five do, which is the case the
     * distinction exists for. */
    LmbClusterNode grow[5];
    for (int i = 0; i < 5; i++) grow[i] = node("box", 32, 0);
    LmbNodeEffect e = lmb_node_effect(&m, grow, 2, 4096, 1, LMB_GOAL_ONE_SESSION);
    CHECK(!e.makes_runnable && e.missing_delta >= 0,
          "a machine that leaves the model unrunnable claimed to fix it");
    e = lmb_node_effect(&m, grow, 5, 4096, 1, LMB_GOAL_ONE_SESSION);
    CHECK(e.makes_runnable && e.enters_critical_path,
          "the machine that completes the coverage was kept out of the chain "
          "because it does not accelerate anything");
    CHECK(e.reason && e.reason[0], "no reason was given for the decision");

    /* And a machine added to a cluster that already runs the model does NOT
     * join the chain on a hunch: without a measurement there is nothing to
     * justify it, and inventing one is the promise we refuse to make. */
    LmbClusterNode plenty[9];
    for (int i = 0; i < 9; i++) plenty[i] = node("big", 60, 0);
    e = lmb_node_effect(&m, plenty, 9, 4096, 1, LMB_GOAL_ONE_SESSION);
    CHECK(!e.enters_critical_path,
          "a ninth machine joined the chain of an already-running model with "
          "no measurement to justify it");

    /* Sessions cost state on every node, so more of them can turn a plan
     * that fits into one that does not. */
    LmbClusterNode tight8[8];
    for (int i = 0; i < 8; i++) tight8[i] = node("snug", 21, 0);
    LmbClusterPlan one, many;
    int ok1 = !lmb_plan_cluster(&m, tight8, 8, 8192, 1, LMB_GOAL_ONE_SESSION, &one);
    int ok8 = !lmb_plan_cluster(&m, tight8, 8, 8192, 16, LMB_GOAL_THROUGHPUT, &many);
    CHECK(ok1 && ok8, "a snug cluster could not be planned");
    CHECK(many.state != LMB_PLAN_RESIDENT || one.state == LMB_PLAN_RESIDENT,
          "sixteen sessions fitted where one did not");

    printf("CLUSTER PLAN: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
