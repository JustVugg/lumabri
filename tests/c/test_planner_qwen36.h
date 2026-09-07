/* Check the resident contract without loading weights or importing Colibri. */
static void qwen36_contract_test(void) {
    char dir[] = "/tmp/lmb-qwen-plan-XXXXXX", path[256];
    CHECK(mkdtemp(dir) != NULL, "cannot create Qwen contract fixture");
    snprintf(path, sizeof path, "%s/config.json", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "cannot create Qwen config");
    if (!f) return;
    fputs("{\"model_type\":\"qwen3_5_moe_text\",\"hidden_size\":64,"
          "\"num_hidden_layers\":2,\"vocab_size\":320}", f);
    fclose(f);
    LmbModelShape m;
    CHECK(!lmb_shape_from_config(dir, &m) && !m.sizing_verified,
          "Qwen without converted metadata was enabled");
    snprintf(path, sizeof path, "%s/qwen36_meta.json", dir);
    for (int grouped = 0; grouped < 2; grouped++) {
        f = fopen(path, "w");
        CHECK(f != NULL, "cannot create Qwen metadata");
        if (!f) break;
        fprintf(f, "{\"hidden\":64,\"n_layers\":2,\"num_experts\":8,\"topk\":2,"
          "\"moe_inter\":32,\"shared_inter\":32,\"q_heads\":4,\"kv_heads\":2,"
          "\"head_dim\":16,\"q_head_dim\":32,\"k_head_dim\":16,\"v_head_dim\":16,"
          "\"o_in\":64,\"expert_gs\":%u,\"dn_vheads\":8,\"dn_kheads\":4,"
          "\"dn_kdim\":8,\"dn_vdim\":8,\"dn_convk\":4,\"dn_conv_dim\":128,"
          "\"layer_types\":[\"linear_attention\",\"full_attention\"],\"ebits\":%u}",
          grouped ? 16 : 0, grouped ? 4 : 8);
        fclose(f);
        CHECK(!lmb_shape_from_config(dir, &m) && m.sizing_verified && m.memory_contract == 1,
              "valid Qwen metadata did not produce a memory contract");
        uint64_t scales = grouped ? 2u * 32 * 4 + 64 * 2 : 2u * 32 + 64;
        uint64_t common = 3u * 64 + 8u * 64 + 8 + 3u * 64 * 32 + 2u * 16;
        uint64_t attention = 4u * 32 * 64 + 2u * 32 * 64 + 64u * 64;
        CHECK(m.memory[1].resident_bytes == (3u * 64 * 32 + scales * 4) * 8 +
              (common + attention) * 4,
              "Qwen CPU cache did not include unpacked int8 and all group scales");
        LmbRangeCost linear = lmb_estimate_segment(&m, 0, 1, 64, 1);
        LmbRangeCost full = lmb_estimate_segment(&m, 1, 2, 64, 1);
        LmbRangeCost all = lmb_estimate_segment(&m, 0, 2, 64, 1);
        CHECK(linear.ok && full.ok && all.ok, "Qwen ranges could not be estimated");
        CHECK(linear.state_bytes == (8u * 8 * 8 + 128u * 3) * 4,
              "DeltaNet recurrent and convolution state mismatch");
        CHECK(full.state_bytes == 2u * 16 * 64 * 8, "attention KV size mismatch");
        CHECK(all.state_bytes == linear.state_bytes + full.state_bytes,
              "hybrid state did not sum by actual layer kind");
        CHECK(lmb_estimate_segment(&m, 0, 1, 128, 1).state_bytes == linear.state_bytes,
              "context incorrectly expanded fixed recurrent state");
        CHECK(lmb_estimate_segment(&m, 1, 2, 128, 1).state_bytes == full.state_bytes * 2,
              "context did not expand attention KV state");
        CHECK(lmb_estimate_segment(&m, 0, 2, 64, 3).state_bytes == all.state_bytes * 3,
              "sessions did not receive separate state");
        CHECK(linear.resident_bytes + full.resident_bytes == all.resident_bytes + m.segment_fixed_bytes,
              "split lost or double-counted its per-process structures");
        CHECK(!m.disk_streaming && all.working_set_bytes == all.resident_bytes,
              "a resident-only contract advertised unverified disk execution");
        CHECK(!lmb_estimate_segment(&m, 0, 2, 262145, 1).ok &&
              !lmb_estimate_edge(&m, 262145, 1).ok,
              "context beyond the adapter limit was accepted");
        CHECK(lmb_estimate_edge(&m, 64, 1).resident_bytes == (2u * 320 * 64 + 64) * 4,
              "Edge did not reserve its float32 boundaries");
        m.memory[1].state_token_bytes = UINT64_MAX;
        CHECK(!lmb_estimate_segment(&m, 0, 2, 64, 1).ok,
              "invalid explicit layer state wrapped into a valid budget");
    }
    uint32_t n = 0;
    CHECK(!lmb_plan_u32("{\"x\":4}", "x", 1, 8, &n) && n == 4,
          "strict metadata integer reader rejected a valid value");
    const char *invalid[] = {"{\"x\":-1}", "{\"x\":04}", "{\"x\":4.5}",
        "{\"x\":4e1}", "{\"x\":4294967296}", "{\"x\":\"4\"}",
        "{\"x\":18446744073709551616000}"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        CHECK(lmb_plan_u32(invalid[i], "x", 1, 8, &n) != 0,
              "invalid metadata number accepted: %s", invalid[i]);
    f = fopen(path, "w");
    if (f) { fputs("{}", f); fclose(f); }
    CHECK(!lmb_shape_from_config(dir, &m) && !m.sizing_verified,
          "incomplete Qwen metadata enabled an estimate");
    unlink(path);
    snprintf(path, sizeof path, "%s/config.json", dir);
    unlink(path); rmdir(dir);
}
