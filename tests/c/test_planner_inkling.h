/* Metadata-only fixtures test admission arithmetic, not model numerics.
 * Real float/int4 inference and split oracles are separate integration gates. */
static void plan_tensor_write(const char *path, const char *json, uint64_t payload) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "cannot create tensor fixture");
    if (!f) return;
    uint64_t n = strlen(json);
    unsigned char prefix[8];
    for (unsigned i = 0; i < 8; i++) prefix[i] = (unsigned char)(n >> (8 * i));
    CHECK(fwrite(prefix, 1, 8, f) == 8 && fwrite(json, 1, n, f) == n,
          "cannot write tensor header");
    if (payload) {
        CHECK(!fseeko(f, (off_t)(payload - 1), SEEK_CUR) && fputc(0, f) != EOF,
              "cannot size sparse tensor payload");
    }
    fclose(f);
}

static int plan_tensor_count(const LmbPlanTensor *t, void *data) {
    unsigned *count = data;
    CHECK(t->elements == 2 && t->bytes == 8 && !strcmp(t->dtype, "F32"),
          "tensor inventory changed shape or storage width");
    (*count)++;
    return 0;
}

static void tensor_header_test(void) {
    char dir[] = "/tmp/lmb-tensor-plan-XXXXXX", path[256];
    CHECK(mkdtemp(dir) != NULL, "cannot create tensor directory");
    snprintf(path, sizeof path, "%s/model.safetensors", dir);
    const char *valid = "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,8]}}";
    plan_tensor_write(path, valid, 8);
    unsigned count = 0;
    CHECK(!lmb_plan_tensors(dir, plan_tensor_count, &count) && count == 1,
          "valid tensor header rejected");
    uint64_t budget = 2;
    CHECK(lmb_plan_tensor_file(path, &budget, plan_tensor_count, &count) != 0,
          "header byte budget was ignored");
    const char *invalid[] = {
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,9]}}",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[3],\"data_offsets\":[0,8]}}",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[18446744073709551615,4],\"data_offsets\":[0,8]}}",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[2.0],\"data_offsets\":[0,8]}}",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[8,0]}}",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,8]}]",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,8]},}",
        "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,8]}} extra",
        "{\"w\":{\"dtype\":\"F64\",\"shape\":[1],\"data_offsets\":[0,8]}}",
        "{\"__metadata__\":{\"format\":\"pt\"}}"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        plan_tensor_write(path, invalid[i], 8);
        CHECK(lmb_plan_tensors(dir, plan_tensor_count, &count) != 0,
              "invalid tensor header %zu accepted", i);
    }
    plan_tensor_write(path, valid, 7);
    CHECK(lmb_plan_tensors(dir, plan_tensor_count, &count) != 0,
          "truncated checkpoint accepted");
    unlink(path); rmdir(dir);
}

static void inkling_contract_test(void) {
    tensor_header_test();
    LmbModelShape m = { .layers = 2, .hidden = 4, .vocab = 8 };
    LmbInklingInventory v = { .model = &m, .hidden = 4, .inter = 2, .experts = 2 };
    LmbPlanTensor t = { .elements = 32, .bytes = 64 };
    snprintf(t.name, sizeof t.name, "model.embed_tokens.weight");
    snprintf(t.dtype, sizeof t.dtype, "BF16");
    CHECK(!lmb_inkling_tensor(&t, &v) && m.edge_resident_bytes == 64,
          "BF16 Edge boundary was expanded despite native retention");
    CHECK(lmb_inkling_tensor(&t, &v) != 0, "duplicate boundary accepted");
    snprintf(t.name, sizeof t.name, "lm_head.weight");
    snprintf(t.dtype, sizeof t.dtype, "F16");
    CHECK(!lmb_inkling_tensor(&t, &v) && m.edge_resident_bytes == 192,
          "F16 Edge boundary did not reserve native float32 expansion");
    snprintf(t.name, sizeof t.name, "model.layers.0.mlp.experts.gate_up_proj");
    snprintf(t.dtype, sizeof t.dtype, "U8"); t.elements = t.bytes = 16;
    CHECK(!lmb_inkling_tensor(&t, &v) && m.memory[0].resident_bytes == 16 &&
          v.encoding[0][0] == 4, "Inkling packed int4 was not retained at its real size");
    snprintf(t.name, sizeof t.name, "model.layers.1.mlp.experts.gate_up_proj");
    t.bytes = 15;
    CHECK(lmb_inkling_tensor(&t, &v) != 0, "invalid packed expert length accepted");
    snprintf(t.name, sizeof t.name, "model.layers.0.mlp.experts.gate_up_proj.qs");
    snprintf(t.dtype, sizeof t.dtype, "F32"); t.elements = 7; t.bytes = 28;
    CHECK(lmb_inkling_tensor(&t, &v) != 0, "missing expert scale row accepted");

    m.sizing_verified = m.memory_contract = 1; m.max_context = 128;
    m.memory[0].state_fixed_bytes = 96; m.memory[0].state_token_bytes = 32;
    m.memory[0].state_context_limit = 8;
    m.memory[1].state_fixed_bytes = 64; m.memory[1].state_token_bytes = 16;
    LmbRangeCost a = lmb_estimate_segment(&m, 0, 2, 16, 1);
    LmbRangeCost b = lmb_estimate_segment(&m, 0, 2, 32, 1);
    CHECK(a.ok && b.ok && a.state_bytes == 96 + 32 * 8 + 64 + 16 * 16 &&
          b.state_bytes - a.state_bytes == 16 * 16,
          "sliding KV grew beyond its ring or full KV failed to grow");
    CHECK(lmb_estimate_segment(&m, 0, 2, 32, 3).state_bytes == b.state_bytes * 3,
          "Inkling session state was shared between clients");
    CHECK(!m.disk_streaming && a.working_set_bytes == a.resident_bytes,
          "unverified Inkling disk support was advertised");
    CHECK(!lmb_estimate_segment(&m, 0, 2, 129, 1).ok,
          "Inkling context admission ceiling ignored");
}
