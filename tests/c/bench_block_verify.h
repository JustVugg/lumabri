/* Diagnostic perfect-draft bound, not a speculative runtime. No remote
 * routes, model downloads or upstream changes. Every position is verified;
 * step(S) alone would expose only the last logit row and give a false speedup.
 * Requires the OLMoE implementation already included by bench_home_hybrid.c. */
static void block_predictions(Model *m, const int *ids, int rows, int pos,
                              int batched_head, int *predictions) {
    int d = m->c.hidden, v = m->c.vocab;
    float *x = falloc((int64_t)rows * d), *n = falloc((int64_t)rows * d);
    float *logits = falloc((int64_t)rows * v);
    for (int r = 0; r < rows; r++)
        memcpy(x + (int64_t)r * d, m->embed + (int64_t)ids[r] * d, d * sizeof(float));
    layers_forward_range(m, x, rows, pos, 0, m->c.n_layers, 1);
    for (int r = 0; r < rows; r++)
        rmsnorm_row(n + (int64_t)r * d, x + (int64_t)r * d, m->final_norm, d, m->c.eps);
    if (batched_head) matmul(logits, n, m->lm_head, rows, d, v);
    else for (int r = 0; r < rows; r++)
        matmul(logits + (int64_t)r * v, n + (int64_t)r * d, m->lm_head, 1, d, v);
    for (int r = 0; r < rows; r++) predictions[r] = pick_tok(logits + (int64_t)r * v, v, -1);
    m->kv_len = pos + rows; m->token_count += rows; m->freq_token_count += rows;
    free(logits); free(n); free(x);
}

static int block_oracle(Model *m, const char *directory) {
    const char *prompts[] = {"Descrivi i cappelletti in due frasi.",
        "What is 17 plus 25? Explain briefly.",
        "Write a short Python function that returns the larger of two numbers."};
    const int widths[] = {1, 1, 2, 4, 8, 8};
    const char *names[] = {"native", "verify-1", "verify-2", "verify-4", "verify-8", "verify-8-batched-head"};
    enum {TOKENS = 16, REPEATS = 3, MODES = 6};
    double totals[MODES][REPEATS] = {{0}};
    Tok tokenizer; char path[4096];
    if (snprintf(path, sizeof path, "%s/tokenizer.json", directory) >= (int)sizeof path) return 2;
    tok_load(&tokenizer, path); g_temp = 0; g_pilot = 0; L.on = 0;
    m->max_t = 512; m->K = calloc(m->c.n_layers, sizeof *m->K); m->V = calloc(m->c.n_layers, sizeof *m->V);
    if (!m->K || !m->V) return 2;
    for (int i = 0; i < m->c.n_layers; i++) {
        m->K[i] = falloc((int64_t)m->c.n_heads * m->max_t * m->c.head_dim);
        m->V[i] = falloc((int64_t)m->c.n_heads * m->max_t * m->c.head_dim);
    }
    uint64_t misses = m->miss; unsigned checked = 0;
    puts("PERFECT DRAFT: all-row verification, 3 prompts, 16 positions each, 3 rotated repeats; "
         "prefill excluded; no drafting/network/rejection costs. Final mode batches only the head experimentally.");
    for (int prompt = 0; prompt < 3; prompt++) {
        char text[2048]; int ids[2048], gold[TOKENS + 1];
        int length = fmt_user_turn(text, sizeof text, prompts[prompt], 1);
        int np = length > 0 ? tok_encode(&tokenizer, text, length, ids, 2048) : -1;
        if (np < 1 || np + TOKENS > m->max_t) return 2;
        m->kv_len = 0; float *logits = step(m, ids, np, 0);
        for (int k = 0; k <= TOKENS; k++) {
            gold[k] = pick_tok(logits, m->c.vocab, -1); free(logits);
            if (k < TOKENS) logits = step(m, &gold[k], 1, np + k);
        }
        for (int repeat = 0; repeat < REPEATS; repeat++) for (int j = 0; j < MODES; j++) {
            int mode = (j + repeat + prompt) % MODES, width = widths[mode];
            m->kv_len = 0; free(step(m, ids, np, 0));
            int actual[TOKENS]; double start = lumi_now();
            for (int k = 0; k < TOKENS; k += width) {
                if (!mode) {
                    logits = step(m, gold + k, 1, np + k);
                    actual[k] = pick_tok(logits, m->c.vocab, -1); free(logits);
                } else block_predictions(m, gold + k, width, np + k, mode == 5, actual + k);
            }
            double seconds = lumi_now() - start;
            for (int k = 0; k < TOKENS; k++) if (actual[k] != gold[k + 1]) {
                fprintf(stderr, "BLOCK FAIL prompt=%d mode=%s position=%d expected=%d actual=%d\n",
                    prompt, names[mode], k, gold[k + 1], actual[k]); return 4;
            }
            if (m->miss != misses) { fprintf(stderr, "BLOCK FAIL: late expert loads\n"); return 4; }
            totals[mode][repeat] += seconds; checked += TOKENS;
            printf("block prompt=%d repeat=%d mode=%s positions=%d seconds=%.6f ms/position=%.3f\n",
                prompt, repeat, names[mode], TOKENS, seconds, 1000 * seconds / TOKENS); fflush(stdout);
        }
    }
    double median[MODES];
    for (int mode = 0; mode < MODES; mode++) {
        double a = totals[mode][0], b = totals[mode][1], c = totals[mode][2];
        median[mode] = a + b + c - fmin(a, fmin(b, c)) - fmax(a, fmax(b, c));
        printf("SUMMARY mode=%s median_seconds=%.6f ideal_positions/s=%.3f relative_to_native=%.3f\n",
            names[mode], median[mode], 3 * TOKENS / median[mode], median[0] / median[mode]);
    }
    printf("BLOCK PASS: %u greedy predictions match; zero late expert loads; no remote/draft model. "
           "These are optimistic bounds for this verifier, NOT delivered chat rates.\n", checked);
    tok_free(&tokenizer); return 0;
}
