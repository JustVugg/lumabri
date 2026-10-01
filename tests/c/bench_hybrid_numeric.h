/* Explicit opt-in for the fixed public benchmark prompts only. Never linked
 * into the household runtime. Capture preserves the first rejected sample;
 * replay uses the same already-approved resident expert, not new allocations. */
#ifndef BENCH_HYBRID_NUMERIC_H
#define BENCH_HYBRID_NUMERIC_H
static struct { int layer, expert, dim; float *data; } numeric_sample;

/* Socket helpers use send(MSG_NOSIGNAL), and cannot write a regular file. */
static int bench_numeric_write(int fd, const void *bytes, size_t count) {
    const unsigned char *cursor = bytes;
    while (count) {
        ssize_t n = write(fd, cursor, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        cursor += n; count -= (size_t)n;
    }
    return 0;
}

static int bench_numeric_save(const char *dir, int inter) {
    char path[4096];
    if (!dir || !numeric_sample.data || snprintf(path, sizeof path, "%s/rejected-expert.bin", dir) >= (int)sizeof path)
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    uint32_t header[6] = {0x4c4e554d, 1, (uint32_t)numeric_sample.layer,
        (uint32_t)numeric_sample.expert, (uint32_t)numeric_sample.dim, (uint32_t)inter};
    int rc = bench_numeric_write(fd, header, sizeof header) ||
        bench_numeric_write(fd, numeric_sample.data, (size_t)numeric_sample.dim*3*sizeof(float));
    if (close(fd)) rc = -1;
    return rc;
}

static void bench_numeric_capture(int layer, int expert, const float *x, int dim,
                                  const float *local, const float *remote) {
    if (!getenv("LMB_BENCH_NUMERIC_DIR") || numeric_sample.data || dim < 1 || dim > 65536) return;
    float *data = malloc((size_t)dim * 3 * sizeof(float));
    if (!data) return;
    memcpy(data, x, (size_t)dim * sizeof(float));
    memcpy(data + dim, local, (size_t)dim * sizeof(float));
    memcpy(data + 2*dim, remote, (size_t)dim * sizeof(float));
    numeric_sample.layer = layer; numeric_sample.expert = expert;
    numeric_sample.dim = dim; numeric_sample.data = data;
}

static void bench_numeric_difference(const char *name, const float *reference, const float *got, int dim) {
    double max_abs = 0, max_ratio = 0, square_error = 0, square_ref = 0;
    unsigned outside = 0; int worst = 0;
    for (int i = 0; i < dim; i++) {
        double diff = fabs((double)reference[i] - got[i]);
        double limit = LMB_HYBRID_FP32_ATOL + LMB_HYBRID_FP32_RTOL * fabs((double)reference[i]);
        double ratio = diff / limit;
        if (!isfinite(got[i]) || ratio > 1) outside++;
        if (diff > max_abs) max_abs = diff;
        if (ratio > max_ratio) { max_ratio = ratio; worst = i; }
        square_error += diff * diff; square_ref += (double)reference[i]*reference[i];
    }
    printf("NUMERIC_COMPARE name=%s bit_exact=%d outside=%u max_abs=%.12g "
        "max_envelope_ratio=%.12g relative_l2=%.12g worst_index=%d reference=%.12g got=%.12g\n",
        name, !memcmp(reference, got, (size_t)dim*sizeof(float)), outside, max_abs, max_ratio,
        sqrt(square_error / fmax(square_ref, 1e-300)), worst, (double)reference[worst], (double)got[worst]);
    fflush(stdout);
}

/* Mathematical reference for the SAME quantized weight representation.
 * Long-double accumulation and exp do not reproduce FP32 rounding. This
 * diagnoses error sources, not the accuracy of the original full model. */
static void bench_numeric_precise(Slot *e, const float *x, int dim, int inter, float *out) {
    long double *activation = calloc((size_t)inter, sizeof *activation);
    if (!activation) { fprintf(stderr, "NUMERIC_REFERENCE allocation failed\n"); exit(5); }
    for (int j = 0; j < inter; j++) {
        long double gate = 0, up = 0;
        for (int i = 0; i < dim; i++) {
            gate += (long double)x[i] * e->g[(size_t)j*dim+i];
            up += (long double)x[i] * e->u[(size_t)j*dim+i];
        }
        gate *= e->gs[j]; up *= e->us[j];
        activation[j] = gate / (1 + expl(-gate)) * up;
    }
    for (int i = 0; i < dim; i++) {
        long double down = 0;
        for (int j = 0; j < inter; j++) down += activation[j] * e->d[(size_t)i*inter+j];
        out[i] = (float)(down * e->ds[i]);
    }
    free(activation);
}

static void bench_numeric_replay(Model *m) {
    int layer = numeric_sample.layer, expert = numeric_sample.expert, dim = numeric_sample.dim;
    float *x = numeric_sample.data, *local = x + dim, *remote = x + 2*dim;
    printf("NUMERIC_SAMPLE layer=%d expert=%d dim=%d inter=%d IDOT=%s\n",
        layer, expert, dim, m->c.inter, getenv("IDOT") ? getenv("IDOT") : "unset");
    bench_numeric_difference("captured_local_vs_remote", local, remote, dim);
    /* Little-endian diagnostic ABI: header, then input/local/remote FP32. */
    int saved = !bench_numeric_save(getenv("LMB_BENCH_NUMERIC_DIR"), m->c.inter);
    printf("NUMERIC_CAPTURE saved=%d\n", saved);
    float *got = falloc(dim);
    LmbOlmoeLocal ctx = {m, falloc(m->c.inter), falloc(m->c.inter)};
    int original_threads = omp_get_max_threads();
    const int teams[] = {1, 4, original_threads};
    for (unsigned team = 0; team < sizeof teams/sizeof *teams; team++) {
        omp_set_num_threads(teams[team]);
        for (int run = 0; run < 3; run++) {
            char name[80]; snprintf(name, sizeof name, "local_threads%d_repeat%d", teams[team], run);
            if (!lmb_olmoe_local(&ctx, layer, expert, x, dim, got))
                bench_numeric_difference(name, local, got, dim);
        }
    }
    omp_set_num_threads(original_threads);
    for (int run = 0; run < 3; run++) {
        uint32_t tried = 0; LumiPeer *from = NULL;
        float *again = lumi_exec_retry(layer, expert, x, dim, 1, NULL, &tried, &from);
        if (!again) { printf("NUMERIC_REMOTE_REPLAY failed=%d\n", run); continue; }
        char name[80]; snprintf(name, sizeof name, "remote_repeat%d", run);
        bench_numeric_difference(name, remote, again, dim);
        free(again);
    }
    Slot *slot; expert_get(m, layer, expert, &slot);
    bench_numeric_precise(slot, x, dim, m->c.inter, got);
    bench_numeric_difference("precise_vs_local", got, local, dim);
    bench_numeric_difference("precise_vs_remote", got, remote, dim);
    free(got); free(ctx.g); free(ctx.u); free(numeric_sample.data); numeric_sample.data = NULL;
}

static int bench_numeric_capture_selftest(const char *dir) {
    float data[] = {1,2,3,4,5,6};
    numeric_sample.layer = 14; numeric_sample.expert = 8;
    numeric_sample.dim = 2; numeric_sample.data = data;
    if (bench_numeric_save(dir, 3)) return 5;
    char path[4096]; snprintf(path, sizeof path, "%s/rejected-expert.bin", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    uint32_t h[6]; float restored[6]; struct stat st;
    int bad = fd < 0 || fstat(fd, &st) || (st.st_mode & 077) || st.st_size != 48 ||
        lmb_read_full(fd, h, sizeof h) || lmb_read_full(fd, restored, sizeof restored) ||
        h[0] != 0x4c4e554d || h[1] != 1 || h[2] != 14 || h[3] != 8 || h[4] != 2 || h[5] != 3 ||
        memcmp(data, restored, sizeof data);
    if (fd >= 0) close(fd);
    /* A second capture must not overwrite the first diagnostic. */
    if (!bench_numeric_save(dir, 3)) bad = 1;
    numeric_sample.data = NULL;
    printf("NUMERIC_CAPTURE_SELFTEST %s\n", bad ? "FAIL" : "PASS");
    return bad ? 5 : 0;
}
static int bench_numeric_reference_selftest(void) {
    int8_t g[] = {1,-1}, u[] = {2,1}, d[] = {3,-2};
    float gs[] = {1}, us[] = {.5f}, ds[] = {1,2}, x[] = {2,1}, out[2];
    Slot slot = {.eid=0, .g=g, .u=u, .d=d, .gs=gs, .us=us, .ds=ds};
    int map[] = {0};
    LCache cache = {.slots=&slot, .slot_by_expert=map, .n=1, .cap=1};
    Model m = {0}; m.c.hidden=2; m.c.inter=1; m.c.n_layers=1; m.c.n_experts=1;
    m.quant_bits=8; m.cache=&cache;
    LmbOlmoeLocal ctx = {.m=&m};
    unsetenv("IDOT"); g_pilot=0; g_fused3=0;
    assert(!lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    long double a = 2.5L/(1+expl(-1));
    assert(out[0] == (float)(3*a) && out[1] == (float)(-4*a));
    map[0]=-1; /* Missing resident expert: MUST NOT touch the absent shards. */
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    map[0]=0; slot.eid=1;
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    slot.eid=0; m.quant_bits=4;
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    m.quant_bits=8; setenv("IDOT", "1", 1);
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    unsetenv("IDOT"); g_fused3=1;
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    g_fused3=0; g_pilot=1;
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 2, out));
    g_pilot=0;
    assert(lmb_olmoe_reference(&ctx, 1, 0, x, 2, out));
    assert(lmb_olmoe_reference(&ctx, 0, 1, x, 2, out));
    assert(lmb_olmoe_reference(&ctx, 0, 0, x, 3, out));
    assert(m.miss == 0 && m.hits == 0 && m.clock == 0);
    puts("NUMERIC_REFERENCE_SELFTEST PASS: resident-only, correct shape/kernel, no cache mutation");
    return 0;
}
#endif
