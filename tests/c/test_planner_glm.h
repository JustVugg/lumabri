static void glm_contract_test(void) {
    LmbModelShape m = { .layers = 2, .hidden = 32, .vocab = 8 };
    uint8_t seen[4] = {0};
    LmbGlmInventory v = { .model = &m, .first = 1, .inter = 32,
        .experts = 2, .expert_seen = seen };
    LmbPlanTensor t = { .rank = 2, .shape = {32, 32}, .elements = 1024, .bytes = 2048 };
    snprintf(t.dtype, sizeof t.dtype, "BF16");
    snprintf(t.name, sizeof t.name, "model.layers.1.mlp.experts.0.gate_proj.weight");
    CHECK(!lmb_glm_tensor(&t, &v) && seen[2] == 1 && m.memory[1].resident_bytes == 5120 &&
          v.largest_expert == 4096,
          "GLM float source did not cover f32 runtime weights and scales");
    CHECK(lmb_glm_tensor(&t, &v) != 0, "duplicate GLM expert accepted");
    snprintf(t.name, sizeof t.name, "model.layers.1.mlp.experts.0.up_proj.weight");
    snprintf(t.dtype, sizeof t.dtype, "U8");
    CHECK(lmb_glm_tensor(&t, &v) != 0, "unverified prepared GLM encoding enabled");
    snprintf(t.name, sizeof t.name, "model.layers.1.mlp.experts.2.gate_proj.weight");
    snprintf(t.dtype, sizeof t.dtype, "F32"); t.bytes = 4096;
    CHECK(lmb_glm_tensor(&t, &v) != 0, "GLM expert ID out of range accepted");
    snprintf(t.name, sizeof t.name, "model.embed_tokens.weight");
    t.elements = 256; t.bytes = 1024;
    CHECK(!lmb_glm_tensor(&t, &v) && m.edge_resident_bytes == 1056 &&
          m.edge_scratch_fixed_bytes == 1024, "GLM Edge load temporary not reserved");
    snprintf(t.name, sizeof t.name, "model.embed_tokens.weight.qs");
    CHECK(lmb_glm_tensor(&t, &v) != 0,
          "sidecar silently changed an admitted float source into a quantized container");
}
