/* Experimental greedy speculation with a causal, zero-weight n-gram proposer.
 * No remote execution and no future/reference tokens are available to the
 * proposer. This tests acceptance and rollback, not a deployed feature. */
static int causal_propose(const int *history, int count, int *out, int cap) {
    if (!history || !out || count < 2 || cap <= 0) return 0;
    for (int width = count < 8 ? count - 1 : 8; width >= 2; width--) {
        for (int begin = count - width - 1; begin >= 0; begin--) {
            if (memcmp(history + begin, history + count - width, width * sizeof(int))) continue;
            int n = count - begin - width;
            if (n > cap) n = cap;
            memcpy(out, history + begin + width, n * sizeof(int));
            return n;
        }
    }
    return 0;
}

/* Each row predicts the next token. Keep the matching proposed prefix and
 * its correction/bonus token; never publish the rejected suffix. */
static int causal_keep(const int *inputs, const int *predictions, int rows) {
    if (!inputs || !predictions || rows < 1) return 0;
    int kept = 1;
    while (kept < rows && inputs[kept] == predictions[kept - 1]) kept++;
    return kept;
}

static void causal_contract_test(void) {
    const int history[] = {1, 2, 3, 4, 1, 2}; int proposed[8];
    assert(causal_propose(history, 6, proposed, 8) == 4);
    assert(proposed[0] == 3 && proposed[1] == 4 && proposed[2] == 1 && proposed[3] == 2);
    assert(!causal_propose(history, 1, proposed, 8));
    const int in[] = {7, 8, 9, 10};
    const int good[] = {8, 9, 10, 11}, first[] = {2, 9, 10, 11}, middle[] = {8, 2, 10, 11};
    assert(causal_keep(in, good, 4) == 4);
    assert(causal_keep(in, first, 4) == 1);
    assert(causal_keep(in, middle, 4) == 2);
}

static int causal_oracle(Model *m, const char *directory) {
    const char *prompts[] = {"Descrivi i cappelletti in due frasi.",
        "What is 17 plus 25? Explain briefly.",
        "Write a short Python function that returns the larger of two numbers.",
        "Repeat this sentence exactly four times: The red boat crosses the quiet lake."};
    enum {COUNT = 32, REPEATS = 3};
    Tok tokenizer; char path[4096];
    if (snprintf(path, sizeof path, "%s/tokenizer.json", directory) >= (int)sizeof path) return 2;
    tok_load(&tokenizer, path); g_temp = 0; g_pilot = 0; L.on = 0;
    stops_arm_tok(&m->c, tok_id_of(&tokenizer, "|||IP_ADDRESS|||"), &tokenizer);
    m->max_t = 512; m->K = calloc(m->c.n_layers, sizeof *m->K); m->V = calloc(m->c.n_layers, sizeof *m->V);
    if (!m->K || !m->V) return 2;
    for (int i = 0; i < m->c.n_layers; i++) {
        m->K[i] = falloc((int64_t)m->c.n_heads * m->max_t * m->c.head_dim);
        m->V[i] = falloc((int64_t)m->c.n_heads * m->max_t * m->c.head_dim);
    }
    causal_contract_test(); uint64_t misses = m->miss;
    double totals[2][REPEATS] = {{0}};
    unsigned positions[2][REPEATS] = {{0}};
    unsigned matched = 0, all_proposed = 0, all_accepted = 0, all_rejected = 0;
    puts("CAUSAL SPECULATION: n-gram proposals from committed history only; all-row batched head; "
         "up to 32 generated positions, stop tokens honored; fresh KV; prefill separate. No donor/network."); fflush(stdout);
    for (int p = 0; p < 4; p++) {
        char text[2048]; int prompt[2048];
        int len = fmt_user_turn(text, sizeof text, prompts[p], 1);
        int np = len > 0 ? tok_encode(&tokenizer, text, len, prompt, 2048) : -1;
        if (np < 1 || np + COUNT + 8 > m->max_t) return 2;
        int gold[COUNT], gold_count = 0;
        for (int repeat = 0; repeat < REPEATS; repeat++) for (int j = 0; j < 2; j++) {
            /* Establish oracle once, then alternate execution order. */
            int mode = repeat ? (j + repeat + p) % 2 : j;
            int history[512]; memcpy(history, prompt, np * sizeof(int));
            m->kv_len = 0; double prefill_start = lumi_now();
            float *logits = step(m, prompt, np, 0);
            history[np] = pick_tok(logits, m->c.vocab, -1); free(logits);
            double prefill = lumi_now() - prefill_start, start = lumi_now();
            int generated = 1; unsigned proposals = 0, accepted = 0, rejects = 0, rounds = 0;
            while (generated < COUNT && !is_stop(history[np + generated - 1])) {
                int count = np + generated, pos = count - 1;
                int inputs[8], predicted[8], kept = 1, n = 0;
                inputs[0] = history[pos];
                if (mode) n = causal_propose(history, count, inputs + 1, 3);
                if (n + 1 > COUNT - generated) n = COUNT - generated - 1;
                if (n) {
                    block_predictions(m, inputs, n + 1, pos, 1, predicted);
                    kept = causal_keep(inputs, predicted, n + 1);
                } else {
                    logits = step(m, inputs, 1, pos);
                    predicted[0] = pick_tok(logits, m->c.vocab, -1); free(logits);
                }
                for (int k = 0; k < kept; k++) if (is_stop(predicted[k])) { kept = k + 1; break; }
                if (n) { proposals += n; accepted += kept - 1; if (kept < n + 1) rejects++; }
                /* OLMoE causal KV only: stale suffix is overwritten before
                 * reuse. This is NOT valid for recurrent compressed state. */
                m->kv_len = pos + kept;
                memcpy(history + count, predicted, kept * sizeof(int)); generated += kept; rounds++;
            }
            double seconds = lumi_now() - start;
            if (!repeat && !mode) { gold_count = generated; memcpy(gold, history + np, generated * sizeof(int)); }
            if (generated != gold_count || memcmp(gold, history + np, generated * sizeof(int))) {
                fprintf(stderr, "CAUSAL FAIL: prompt=%d repeat=%d mode=%d\n", p, repeat, mode); return 4;
            }
            if (m->miss != misses) { fprintf(stderr, "CAUSAL FAIL: late expert loads\n"); return 4; }
            matched += generated; totals[mode][repeat] += seconds;
            positions[mode][repeat] += generated - 1;
            if (mode) { all_proposed += proposals; all_accepted += accepted; all_rejected += rejects; }
            printf("causal prompt=%d repeat=%d mode=%s prefill=%.3f decode=%.6f generated=%d rounds=%u proposed=%u accepted=%u rejected_blocks=%u\n",
                p, repeat, mode ? "ngram" : "native", prefill, seconds, generated, rounds, proposals, accepted, rejects);
            fflush(stdout);
        }
    }
    double median[2];
    for (int mode = 0; mode < 2; mode++) {
        double a = totals[mode][0], b = totals[mode][1], c = totals[mode][2];
        median[mode] = a + b + c - fmin(a, fmin(b, c)) - fmax(a, fmax(b, c));
        printf("CAUSAL SUMMARY mode=%s median_decode_seconds=%.6f decode_positions/s=%.3f relative_to_native=%.3f\n",
            mode ? "ngram" : "native", median[mode], positions[mode][0] / median[mode], median[0] / median[mode]);
    }
    printf("CAUSAL PASS: %u matching positions; proposed=%u accepted=%u rejected_blocks=%u; "
        "zero late expert loads. Local prototype only, not PC+Mac acceleration.\n",
        matched, all_proposed, all_accepted, all_rejected);
    tok_free(&tokenizer); return 0;
}
