/* Independent oracle: compile against the original Colibri checkout, never
 * the generated adapter sources. Exercise its monolithic step() directly. */
#define OLMOE_NO_MAIN
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "olmoe.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *out = fopen(argv[2], "w");
    if (!out) return 2;
    Model model;
    g_olm_kv16 = 0;
    model_init(&model, argv[1], 4096, 8);
    model.max_t = 128;
    model.K = calloc((size_t)model.c.n_layers, sizeof(float *));
    model.V = calloc((size_t)model.c.n_layers, sizeof(float *));
    for (int i = 0; i < model.c.n_layers; i++) {
        model.K[i] = falloc((int64_t)model.c.hidden * model.max_t);
        model.V[i] = falloc((int64_t)model.c.hidden * model.max_t);
    }
    int ids[34] = {1, 2};
    for (int pos = 2; pos < 34; pos++) {
        float *logits = step(&model, pos == 2 ? ids : &ids[pos - 1],
                             pos == 2 ? 2 : 1, pos == 2 ? 0 : pos - 1);
        if (!logits) return 1;
        int best = 0;
        for (int t = 0; t < model.c.vocab; t++) {
            if (!isfinite(logits[t])) return 1;
            if (logits[t] > logits[best]) best = t;
        }
        ids[pos] = best;
        free(logits);
    }
    fprintf(out, "{\"prompt_ids\":[1,2],\"full_ids\":[");
    for (int i = 0; i < 34; i++) fprintf(out, "%s%d", i ? "," : "", ids[i]);
    fprintf(out, "]}\n");
    return fclose(out) ? 1 : 0;
}
