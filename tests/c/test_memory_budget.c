#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "lumabri_cluster.h"
#include "lumabri_checkpoint_inventory.h"

static void sized_file(const char *path, off_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !ftruncate(fd, size));
    assert(!close(fd));
}

int main(void) {
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
