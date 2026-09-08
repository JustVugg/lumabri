static void kimi_contract_test(void) {
    LmbModelShape m = { .layers = 2, .hidden = 32, .vocab = 8 };
    uint8_t experts_seen[4] = {0};
    LmbKimiInventory v = { .model = &m, .expert_seen = experts_seen,
        .first = 1, .latent = 32, .inter = 32, .experts = 2 };
    LmbPlanTensor t = { .bytes = 512, .elements = 512, .rank = 2, .shape = {32, 16} };
    snprintf(t.dtype, sizeof t.dtype, "U8");
    snprintf(t.name, sizeof t.name, "model.layers.1.block_sparse_moe.experts.0.w1.weight_packed");
    CHECK(!lmb_kimi_tensor(&t, &v) && m.memory[1].resident_bytes == 512 && experts_seen[2] == 1,
          "native MXFP4 payload did not retain its packed size");
    CHECK(lmb_kimi_tensor(&t, &v) != 0, "duplicate Kimi expert tensor accepted");
    snprintf(t.name, sizeof t.name, "model.layers.1.block_sparse_moe.experts.0.w1.weight_scale");
    t.bytes = t.elements = 31;
    CHECK(lmb_kimi_tensor(&t, &v) != 0, "short e8m0 scales accepted");
    t.bytes = t.elements = 32;
    CHECK(!lmb_kimi_tensor(&t, &v) && experts_seen[2] == 3,
          "valid MXFP4 exponent scales rejected");
    snprintf(t.name, sizeof t.name, "model.layers.0.block_sparse_moe.experts.0.w1.weight_packed");
    t.bytes = t.elements = 512;
    CHECK(lmb_kimi_tensor(&t, &v) != 0, "expert bank accepted on a dense layer");
    snprintf(t.name, sizeof t.name, "model.layers.1.block_sparse_moe.experts.2.w1.weight_packed");
    CHECK(lmb_kimi_tensor(&t, &v) != 0, "expert ID beyond the configured bank accepted");
    snprintf(t.name, sizeof t.name, "language_model.model.embed_tokens.weight");
    snprintf(t.dtype, sizeof t.dtype, "F32"); t.elements = 256; t.bytes = 1024;
    CHECK(lmb_kimi_tensor(&t, &v) != 0, "mixed text namespaces were accepted");
    uint8_t ids[3] = {0};
    CHECK(!lmb_plan_indices("{\"layers\":[1,3]}", "layers", ids, 3, 1) && ids[0] && !ids[1] && ids[2],
          "Kimi's one-based KDA layer list was shifted");
    CHECK(lmb_plan_indices("{\"layers\":[0]}", "layers", ids, 3, 1) != 0 &&
          lmb_plan_indices("{\"layers\":[4]}", "layers", ids, 3, 1) != 0,
          "out-of-range KDA layer accepted");
    m.memory_contract = m.sizing_verified = 1; m.max_context = 128;
    m.boundary_width = 96; m.edge_scratch_fixed_bytes = 4096;
    LmbRangeCost edge = lmb_estimate_edge(&m, 64, 2);
    CHECK(edge.ok && edge.state_bytes == 64u * 96 * 4 * 2,
          "AttnRes boundary state was budgeted as ordinary hidden state");
    CHECK(edge.scratch_bytes == 4096 + 8 * 16, "Edge load workspace disappeared");
    m.boundary_width = 0;
    CHECK(lmb_estimate_edge(&m, 64, 2).state_bytes == 64u * 32 * 4 * 2,
          "ordinary adapter's default boundary width changed");
}
