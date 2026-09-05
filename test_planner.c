/* The planner answers "does it fit", never "how fast".
 *
 * Its three figures are different in kind and the catalogue depends on the
 * difference: resident is every weight of the range, working set is the
 * least that can run at all, and state is what context and sessions add.
 * Collapse any two of them and a whole state of the product disappears —
 * "runs from disk" exists only in the gap between the first two. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lumabri_planner.h"

static int bad;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); bad = 1; } } while (0)

/* Roughly DeepSeek V4 Flash, at the numbers we measured on the real one. */
static LmbModelShape v4(void) {
    LmbModelShape m; memset(&m, 0, sizeof m);
    snprintf(m.model_type, sizeof m.model_type, "deepseek_v4");
    snprintf(m.segment_id, sizeof m.segment_id, "deepseek_v4");
    m.layers = 43; m.hidden = 4096; m.intermediate = 11264;
    m.moe_intermediate = 1408; m.experts = 256; m.experts_per_tok = 6;
    m.heads = 32; m.kv_heads = 8; m.vocab = 129280; m.bits_per_weight = 4;
    m.sizing_verified = 1;
    return m;
}

int main(void) {
    LmbModelShape m = v4();

    /* An expert of this shape measured ~13.4 MB on the real checkpoint. The
     * estimate has to land near that or every figure built on it is wrong. */
    uint64_t e = lmb_expert_bytes_of(&m);
    CHECK(e > 9u * 1000 * 1000 && e < 25u * 1000 * 1000,
          "expert estimate %.1f MB is nowhere near the measured 13.4 MB",
          (double)e / 1e6);

    LmbRangeCost all = lmb_estimate_segment(&m, 0, m.layers, 4096, 1);
    CHECK(all.ok, "a whole-model range could not be estimated");
    /* 11008 experts of ~13 MB is ~150 GB; anything outside 100-250 GB means
     * the arithmetic drifted. */
    CHECK(all.resident_bytes > 100ull * 1000 * 1000 * 1000 &&
          all.resident_bytes < 250ull * 1000 * 1000 * 1000,
          "whole model resident %.0f GB, expected roughly 150",
          (double)all.resident_bytes / 1e9);

    /* THE distinction. The working set of the whole model is a top-k cache,
     * so it must be a small fraction of resident — that gap is disk mode. */
    CHECK(all.working_set_bytes * 4 < all.resident_bytes,
          "working set %.0f GB is not meaningfully below resident %.0f GB: "
          "disk mode would be indistinguishable from resident",
          (double)all.working_set_bytes / 1e9, (double)all.resident_bytes / 1e9);

    /* Three states, on one 64 GB machine holding everything. */
    uint64_t budget = 64ull * 1000 * 1000 * 1000;
    m.disk_streaming = 0;
    CHECK(lmb_plan_state(&m, &all, budget) == LMB_PLAN_UNRUNNABLE,
          "a 150 GB model on 64 GB was not reported as unrunnable");
    m.disk_streaming = 1;
    CHECK(lmb_plan_state(&m, &all, budget) == LMB_PLAN_DISK,
          "an adapter that streams from disk was still called unrunnable");
    CHECK(lmb_plan_state(&m, &all, 400ull * 1000 * 1000 * 1000) == LMB_PLAN_RESIDENT,
          "a model that fits four times over was not called resident");

    /* Disk mode is a capability, not a consolation prize: an adapter that has
     * never demonstrated it does not get it, however well the floor fits. */
    m.disk_streaming = 0;
    LmbRangeCost slice = lmb_estimate_segment(&m, 0, 11, 4096, 1);
    uint64_t just_over = slice.working_set_bytes + slice.state_bytes +
                         slice.scratch_bytes + 1;
    CHECK(lmb_plan_state(&m, &slice, just_over) == LMB_PLAN_UNRUNNABLE,
          "a range was allowed to stream from disk without the capability");
    m.disk_streaming = 1;
    CHECK(lmb_plan_state(&m, &slice, just_over) == LMB_PLAN_DISK,
          "a range whose working set fits exactly was refused");

    /* Splitting divides the weights and nothing else: four ranges must cost
     * what one whole one does, or the placement arithmetic is unsound. */
    uint64_t sum = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t b = (uint32_t)i * m.layers / 4;
        uint32_t en = (uint32_t)(i + 1) * m.layers / 4;
        LmbRangeCost c = lmb_estimate_segment(&m, b, en, 4096, 1);
        CHECK(c.ok, "range %u:%u could not be estimated", b, en);
        sum += c.resident_bytes;
    }
    CHECK(sum == all.resident_bytes,
          "four ranges cost %.1f GB, the whole model %.1f GB",
          (double)sum / 1e9, (double)all.resident_bytes / 1e9);

    /* State scales with what state scales with, and weights do not move. */
    LmbRangeCost s1 = lmb_estimate_segment(&m, 0, 11, 4096, 1);
    LmbRangeCost s4 = lmb_estimate_segment(&m, 0, 11, 4096, 4);
    LmbRangeCost c8 = lmb_estimate_segment(&m, 0, 11, 8192, 1);
    CHECK(s4.state_bytes == s1.state_bytes * 4,
          "four sessions did not cost four KVs");
    CHECK(c8.state_bytes == s1.state_bytes * 2,
          "twice the context did not cost twice the KV");
    CHECK(s4.resident_bytes == s1.resident_bytes,
          "sessions changed the weight footprint");

    /* Edge is embedding and head, and it is not free — it is what makes a
     * chatter thin, so it has to be attributed to whoever runs it. */
    LmbRangeCost edge = lmb_estimate_edge(&m, 4096, 1);
    CHECK(edge.ok && edge.resident_bytes > 100u * 1000 * 1000,
          "Edge came out at %.0f MB, which cannot be right for a 129k vocab",
          (double)edge.resident_bytes / 1e6);

    /* A shape we cannot describe must say so instead of inventing a number:
     * a fabricated estimate is how a catalogue promises what it cannot do. */
    LmbModelShape empty; memset(&empty, 0, sizeof empty);
    CHECK(!lmb_estimate_segment(&empty, 0, 1, 64, 1).ok,
          "an empty shape produced an estimate");
    CHECK(!lmb_estimate_segment(&m, 5, 5, 64, 1).ok, "an empty range estimated");
    CHECK(!lmb_estimate_segment(&m, 0, 999, 64, 1).ok,
          "a range past the end of the model estimated");

    /* And the shape has to come out of a real checkpoint, not just a struct
     * literal — a reader that quietly returns zeros would make every
     * estimate above meaningless while every assertion still passed. */
    LmbModelShape fixture;
    if (!lmb_shape_from_config("tiny_olmoe", &fixture)) {
        CHECK(fixture.layers == 16, "fixture layers %u, expected 16", fixture.layers);
        CHECK(fixture.hidden > 0 && fixture.experts > 0,
              "fixture hidden %u experts %u: the reader found nothing useful",
              fixture.hidden, fixture.experts);
        CHECK(!strcmp(fixture.model_type, "olmoe"),
              "fixture model_type came out as \"%s\"", fixture.model_type);
        CHECK(!strcmp(fixture.segment_id, "olmoe"),
              "fixture was not mapped to an adapter (got \"%s\")",
              fixture.segment_id);
        LmbRangeCost half = lmb_estimate_segment(&fixture, 0, 8, 64, 1);
        CHECK(half.ok && half.resident_bytes > 0,
              "a real checkpoint's range could not be estimated");
    } else {
        fprintf(stderr, "note: tiny_olmoe not present, config reader unexercised\n");
    }

    /* A directory with no config.json must fail rather than return zeros. */
    LmbModelShape none;
    CHECK(lmb_shape_from_config("/nonexistent", &none) != 0,
          "a missing checkpoint produced a shape");

    /* A structurally plausible config for a family we do not support must not
     * become a plan merely because it contains familiar transformer keys. */
    char tmp[] = "/tmp/lmb-plan-XXXXXX";
    char *td = mkdtemp(tmp);
    if (td) {
        char path[256]; snprintf(path, sizeof path, "%s/config.json", td);
        FILE *fp = fopen(path, "w");
        if (fp) {
            fputs("{\"model_type\":\"not_a_colibri_adapter\","
                  "\"num_hidden_layers\":4,\"hidden_size\":64}", fp);
            fclose(fp);
            CHECK(lmb_shape_from_config(td, &none) != 0,
                  "an unsupported model_type produced a shape");
            unlink(path);
        }
        rmdir(td);
    }

    printf("PLANNER: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
