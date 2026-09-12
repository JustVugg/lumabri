#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "lumabri_cluster.h"
#include "lumabri_checkpoint_inventory.h"
#include "src/planner/lumabri_stage_placement.h"

static void timed_plans(void) {
    const uint64_t mib = UINT64_C(1) << 20;
    LmbModelShape shape = {0}; strcpy(shape.model_type, "olmoe");
    shape.layers = 16; shape.hidden = 64; shape.vocab = 128;
    shape.sizing_verified = shape.memory_contract = 1; shape.max_context = 4096;
    shape.edge_resident_bytes = 8 * mib;
    for (uint32_t i = 0; i < shape.layers; i++) shape.memory[i].resident_bytes = 100 * mib;
    LmbClusterNode nodes[2] = {{.ram_budget_bytes = 2200 * mib}, {.ram_budget_bytes = 1000 * mib}};
    LmbClusterPlan seed, got, before;
    assert(!lmb_home_plan_source(&shape, 1, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &seed));
    assert(seed.state == LMB_PLAN_RESIDENT && seed.nslices == 2);
    double costs[2] = {.01, .08};
    assert(!lmb_home_plan_selected(&shape, 1, nodes, 2, 128, &seed, costs, &got));
    assert(got.nslices == 2 && got.slices[0].layer_end == 15 && got.slices[1].layer_begin == 15);
    nodes[0].ram_budget_bytes = 1500 * mib;
    assert(!lmb_home_plan_selected(&shape, 1, nodes, 2, 128, &seed, costs, &got));
    assert(got.slices[0].layer_end == 14 && got.slices[1].layer_begin == 14);
    nodes[0].ram_budget_bytes = 2200 * mib;
    seed.goal = LMB_GOAL_THROUGHPUT;
    costs[0] = .04;
    assert(!lmb_home_plan_selected(&shape, 1, nodes, 2, 128, &seed, costs, &got));
    double best = 1e9; unsigned split = 0;
    for (unsigned s = 1; s < 16; s++) {
        double a = costs[0] * s, b = costs[1] * (16 - s), value = a > b ? a : b;
        if (value < best) { best = value; split = s; }
    }
    assert(got.slices[0].layer_end == split);
    before = got; costs[0] = NAN;
    assert(lmb_home_plan_selected(&shape, 1, nodes, 2, 128, &seed, costs, &got));
    assert(!memcmp(&got, &before, sizeof got));
    costs[0] = .01; nodes[1].ram_budget_bytes = 100 * mib;
    assert(lmb_home_plan_selected(&shape, 1, nodes, 2, 128, &seed, costs, &got));
    assert(!memcmp(&got, &before, sizeof got));
    shape.layers = 1;
    assert(lmb_home_plan_selected(&shape, 1, nodes, 2, 128, &seed, NULL, &got));
}

static void sized_file(const char *path, off_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !ftruncate(fd, size));
    assert(!close(fd));
}

static void feasible_plans(void) {
    const uint64_t mib = UINT64_C(1) << 20;
    LmbModelShape m = {0};
    strcpy(m.model_type, "olmoe");
    m.layers = 4; m.hidden = 64; m.vocab = 128;
    m.sizing_verified = m.memory_contract = 1; m.max_context = 4096;
    m.edge_resident_bytes = 8 * mib;
    for (uint32_t i = 0; i < m.layers; i++) m.memory[i].resident_bytes = 100 * mib;
    LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES] = {0};
    nodes[0].ram_budget_bytes = 500 * mib;
    nodes[1].ram_budget_bytes = 100 * mib;
    LmbClusterPlan p, old;
    assert(!lmb_plan_cluster_source(&m, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &old));
    assert(!lmb_home_plan_budgets(&m, 1, nodes, 2, 128, &old));
    assert(old.state == LMB_PLAN_UNRUNNABLE);
    assert(!lmb_home_plan_source(&m, 1, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(p.state == LMB_PLAN_RESIDENT && p.nslices == 1 && p.edge_node == 0);
    assert(p.slices[0].node == 0 && p.slices[0].layer_end == 4);
    assert(p.fetch_bytes == 408 * mib && !p.ready_known);
    nodes[0].lan_bps = mib;
    assert(!lmb_home_plan_source(&m, 1, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(p.ready_known && p.ready_seconds == 408);
    nodes[0].has_checkpoint = 1;
    assert(!lmb_home_plan_source(&m, 1, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 0, &p));
    assert(!p.fetch_bytes && p.ready_known && !p.ready_seconds);
    nodes[0].has_checkpoint = 0;

    /* Successful legacy placements (and thus calibration keys) are stable. */
    nodes[1].ram_budget_bytes = 500 * mib;
    assert(!lmb_plan_cluster_source(&m, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &old));
    assert(!lmb_home_plan_budgets(&m, 1, nodes, 2, 128, &old));
    assert(old.state == LMB_PLAN_RESIDENT && old.nslices == 2);
    assert(!lmb_home_plan_source(&m, 1, nodes, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(!memcmp(&p, &old, sizeof p));

    /* More selected donors than layers used to strand Edge without a slice.
     * Neither donor can hold both layers with Edge; two are genuinely needed. */
    m.layers = 2;
    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++) nodes[i].ram_budget_bytes = 250 * mib;
    assert(!lmb_plan_cluster_source(&m, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 1, &old));
    assert(lmb_home_plan_budgets(&m, 1, nodes, 8, 128, &old));
    assert(!lmb_home_plan_source(&m, 1, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(p.state == LMB_PLAN_RESIDENT && p.nslices == 2);
    assert(p.edge_node == p.slices[0].node && p.slices[0].layer_end == 1);
    assert(!lmb_home_plan_budgets(&m, 1, nodes, 8, 128, &p));

    /* No sources, extra sessions, invalid context/geometry and overflow
     * must never become approved plans through the alternative search. */
    assert(lmb_home_plan_source(&m, 1, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 0, &p));
    assert(lmb_home_plan_source(&m, 1, nodes, 8, 128, 2, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(lmb_home_plan_source(&m, 1, nodes, 8, 8192, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(lmb_home_plan_source(&m, 0, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(lmb_home_plan_source(NULL, 1, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(lmb_home_plan_source(&m, 1, nodes, LMB_CLUSTER_MAX_NODES + 1,
        128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    m.layers = 0;
    assert(lmb_home_plan_source(&m, 1, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    m.layers = 4;
    for (uint32_t i = 0; i < 8; i++) {
        nodes[i].ram_budget_bytes = 100 * mib;
        nodes[i].vram_budget_bytes = UINT64_C(100) << 30;
    }
    int rc = lmb_home_plan_source(&m, 1, nodes, 8, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p);
    assert(rc || p.state == LMB_PLAN_UNRUNNABLE); /* No RAM/VRAM pooling. */

    /* Deterministic heterogeneous sweeps: every accepted interval is checked
     * independently against the same budget launch will actually enforce. */
    for (uint32_t trial = 0; trial < 500; trial++) {
        uint32_t count = 1 + trial % 8;
        m.layers = 1 + trial % 11;
        for (uint32_t i = 0; i < m.layers; i++)
            m.memory[i].resident_bytes = (10 + (trial * 7 + i * 53) % 200) * mib;
        for (uint32_t i = 0; i < count; i++)
            nodes[i].ram_budget_bytes = (40 + (trial * 47 + i * 71) % 700) * mib;
        rc = lmb_home_plan_source(&m, 1234567, nodes, count, 128, 1,
            LMB_GOAL_ONE_SESSION, 1, &p);
        if (rc || p.state != LMB_PLAN_RESIDENT) continue;
        uint32_t next = 0, edge = 0, seen = 0;
        for (uint32_t i = 0; i < p.nslices; i++) {
            const LmbSlice *s = &p.slices[i];
            assert(s->node < count && !(seen & (1u << s->node)));
            seen |= 1u << s->node;
            assert(s->layer_begin == next && s->layer_end > next);
            next = s->layer_end; edge += s->node == p.edge_node;
            LmbHomeReservation r;
            assert(!lmb_home_reservation(&m, 1234567, s->layer_begin,
                s->layer_end, 128, s->node == p.edge_node, &r));
            assert(r.total_bytes == s->bytes_resident && r.total_bytes <= nodes[s->node].ram_budget_bytes);
        }
        assert(next == m.layers && edge == 1);
    }
}

int main(void) {
    timed_plans();
    feasible_plans();
    LmbModelShape m = {0};
    strcpy(m.model_type, "olmoe");
    m.layers = 4; m.hidden = 64; m.intermediate = 32;
    m.moe_intermediate = 32; m.experts = 8; m.experts_per_tok = 2;
    m.heads = 4; m.kv_heads = 4; m.vocab = 128;
    m.bits_per_weight = 16; m.sizing_verified = 1;
    const uint64_t mib = UINT64_C(1) << 20, bytes = 620001;
    LmbHomeReservation r;
    assert(!lmb_home_reservation(&m, bytes, 0, 4, 128, 1, &r));
    assert(r.segment_bytes >= 128 * mib && r.edge_bytes >= 64 * mib);
    assert(!(r.segment_bytes % mib) && !(r.edge_bytes % mib));
    assert(r.total_bytes == r.segment_bytes + r.edge_bytes);
    assert(lmb_checkpoint_floor(101, 4, 1, 4) == 75 + 5);
    assert(lmb_checkpoint_floor(1, 0, 0, 1) == UINT64_MAX);
    assert(lmb_budget_add(UINT64_MAX, 1) == UINT64_MAX);
    assert(lmb_budget_mib(UINT64_MAX - 1) == UINT64_MAX);
    assert(lmb_budget_mib(mib) == mib && lmb_budget_mib(mib + 1) == 2 * mib);
    LmbHomeReservation invalid;
    assert(lmb_home_reservation(&m, UINT64_MAX, 0, 4, 128, 1, &invalid));
    assert(!invalid.total_bytes);
    assert(lmb_home_reservation(&m, bytes, 2, 1, 128, 1, &invalid));
    assert(lmb_home_reservation(&m, bytes, 0, 4, 128, 2, &invalid));

    LmbClusterNode node = {0};
    node.ram_budget_bytes = r.total_bytes;
    LmbClusterPlan p;
    assert(!lmb_plan_cluster_source(&m, &node, 1, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(!lmb_home_plan_budgets(&m, bytes, &node, 1, 128, &p));
    assert(p.state == LMB_PLAN_RESIDENT && p.slices[0].bytes_resident == r.total_bytes);
    node.ram_budget_bytes--;
    assert(!lmb_home_plan_budgets(&m, bytes, &node, 1, 128, &p));
    assert(p.state == LMB_PLAN_UNRUNNABLE && p.missing_bytes == 1);
    node.vram_budget_bytes = UINT64_C(100) << 30;
    assert(!lmb_home_plan_budgets(&m, bytes, &node, 1, 128, &p));
    assert(p.state == LMB_PLAN_UNRUNNABLE); /* VRAM is not CPU RAM. */
    p.slices[0].layer_begin = 1;
    LmbClusterPlan before = p;
    assert(lmb_home_plan_budgets(&m, bytes, &node, 1, 128, &p));
    assert(!memcmp(&p, &before, sizeof p));
    p.slices[0].layer_begin = 0; p.data_available = 0;
    assert(lmb_home_plan_budgets(&m, bytes, &node, 1, 128, &p));
    p.data_available = 1; p.sessions = 2;
    assert(lmb_home_plan_budgets(&m, bytes, &node, 1, 128, &p));

    LmbClusterNode pair[2];
    memset(pair, 0, sizeof pair);
    pair[0].ram_budget_bytes = pair[1].ram_budget_bytes = 1u << 30;
    assert(!lmb_plan_cluster_source(&m, pair, 2, 128, 1, LMB_GOAL_ONE_SESSION, 1, &p));
    assert(p.nslices == 2);
    assert(!lmb_home_plan_budgets(&m, bytes, pair, 2, 128, &p));
    for (uint32_t i = 0; i < p.nslices; i++) {
        LmbSlice *s = &p.slices[i];
        assert(!lmb_home_reservation(&m, bytes, s->layer_begin, s->layer_end,
                                     128, s->node == p.edge_node, &r));
        assert(s->bytes_resident == r.total_bytes);
    }
    p.slices[1].node = p.slices[0].node;
    before = p;
    assert(lmb_home_plan_budgets(&m, bytes, pair, 2, 128, &p));
    assert(!memcmp(&p, &before, sizeof p));
    p.slices[1].node = 2;
    assert(lmb_home_plan_budgets(&m, bytes, pair, 2, 128, &p));

    char temp[] = "/tmp/lumabri-inventory-test-XXXXXX";
    assert(mkdtemp(temp));
    char config[512], weights[512], runtime[512], link[512];
    snprintf(config, sizeof config, "%s/config.json", temp);
    snprintf(weights, sizeof weights, "%s/model.safetensors", temp);
    snprintf(runtime, sizeof runtime, "%s/.coli_kv", temp);
    snprintf(link, sizeof link, "%s/alias.bin", temp);
    sized_file(config, 100); sized_file(weights, 8192); sized_file(runtime, 65536);
    LmbCheckpointInventory inv;
    assert(!lmb_checkpoint_inventory(temp, &inv));
    assert(inv.bytes == 8292 && inv.files == 2 && inv.has_weights);
    assert(!symlink("model.safetensors", link));
    assert(!lmb_checkpoint_inventory(temp, &inv));
    assert(inv.bytes == 16484 && inv.files == 3 && inv.has_weights);
    assert(!unlink(link) && !symlink(".", link));
    assert(lmb_checkpoint_inventory(temp, &inv));
    assert(!inv.bytes && !inv.files && !inv.has_weights);
    assert(!unlink(link) && !symlink("missing", link));
    assert(lmb_checkpoint_inventory(temp, &inv));
    assert(!unlink(link));
    assert(!unlink(config) && !unlink(weights) && !unlink(runtime) && !rmdir(temp));
    assert(lmb_checkpoint_inventory(temp, &inv));
    puts("HOUSEHOLD MEMORY BUDGET: PASS");
    return 0;
}
