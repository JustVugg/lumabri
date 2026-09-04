/* A measurement is only reusable under the conditions that produced it.
 *
 * The point of these assertions is not that matching works — it is that
 * every single field can invalidate a number on its own. A key that ignores
 * one field is a key that will one day show a GPU measurement for a CPU run,
 * or a two-machine number for a four-machine plan, and be believed. */
#include <stdio.h>
#include <string.h>
#include "lumabri_calibration.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); bad = 1; } } while (0)

static LmbCalKey base(void) {
    LmbCalKey k; memset(&k, 0, sizeof k);
    snprintf(k.model_root, sizeof k.model_root, "abc123");
    snprintf(k.adapter, sizeof k.adapter, "deepseek_v4");
    k.adapter_abi = 1;
    snprintf(k.numeric_class, sizeof k.numeric_class, "fp4/bf16-acc");
    snprintf(k.commit_lumabri, sizeof k.commit_lumabri, "deadbeef");
    snprintf(k.commit_colibri, sizeof k.commit_colibri, "cafebabe");
    snprintf(k.build_id, sizeof k.build_id, "0123456789abcdef");
    snprintf(k.backend, sizeof k.backend, "cpu");
    k.nodes = 2; k.context = 4096; k.sessions = 1;
    for (int i = 0; i < 2; i++) {
        snprintf(k.node_id[i], sizeof k.node_id[i], "node%d", i);
        k.layer_begin[i] = (uint32_t)i * 22;
        k.layer_end[i] = (uint32_t)(i + 1) * 22;
        k.threads[i] = 8;
        k.from_disk[i] = 0;
    }
    return k;
}

int main(void) {
    LmbCalKey a = base(), b = base();
    CHECK(lmb_cal_matches(&a, &b), "two identical keys did not match");

    /* Every field, one at a time. A key that lets any of these through will
     * eventually show somebody the wrong number with full confidence. */
    struct { const char *what; LmbCalKey k; } cases[16];
    int n = 0;
#define VARY(label, mutation) do { LmbCalKey v = base(); mutation; \
    cases[n].what = label; cases[n].k = v; n++; } while (0)
    VARY("checkpoint",      snprintf(v.model_root, sizeof v.model_root, "other"));
    VARY("adapter",         snprintf(v.adapter, sizeof v.adapter, "glm53"));
    VARY("adapter ABI",     v.adapter_abi = 2);
    VARY("numeric class",   snprintf(v.numeric_class, sizeof v.numeric_class, "f32"));
    VARY("lumabri commit",  snprintf(v.commit_lumabri, sizeof v.commit_lumabri, "0000"));
    VARY("colibri commit",  snprintf(v.commit_colibri, sizeof v.commit_colibri, "1111"));
    VARY("build id",        snprintf(v.build_id, sizeof v.build_id, "ffff"));
    VARY("backend",         snprintf(v.backend, sizeof v.backend, "cuda"));
    VARY("context",         v.context = 8192);
    VARY("sessions",        v.sessions = 4);
    VARY("machine count",   v.nodes = 1);
    VARY("which machine",   snprintf(v.node_id[1], sizeof v.node_id[1], "elsewhere"));
    VARY("ranges",          v.layer_begin[1] = 20; v.layer_end[0] = 20);
    VARY("threads",         v.threads[0] = 4);
    VARY("resident/disk",   v.from_disk[1] = 1);
#undef VARY

    for (int i = 0; i < n; i++) {
        const char *moved = lmb_cal_mismatch(&a, &cases[i].k);
        CHECK(moved != NULL,
              "changing %s did not invalidate the measurement — a number "
              "taken under other conditions would be shown as current",
              cases[i].what);
        CHECK(!moved || moved[0],
              "changing %s invalidated the key but named no reason",
              cases[i].what);
    }

    /* What the screen actually prints, which is where this either helps or
     * misleads. */
    LmbCalibration have; memset(&have, 0, sizeof have);
    have.key = base(); have.decode_tok_s = 12.5; have.samples = 3;
    char text[128];

    lmb_cal_speed_text(NULL, &a, text, sizeof text);
    CHECK(!strcmp(text, "not calibrated"),
          "with no record the column said \"%s\"", text);

    lmb_cal_speed_text(&have, &a, text, sizeof text);
    CHECK(strstr(text, "12.5") != NULL,
          "a matching calibration did not print its number: \"%s\"", text);

    LmbCalKey gpu = base();
    snprintf(gpu.backend, sizeof gpu.backend, "cuda");
    lmb_cal_speed_text(&have, &gpu, text, sizeof text);
    CHECK(strstr(text, "stale") != NULL,
          "a CPU measurement was shown for a CUDA plan: \"%s\"", text);
    CHECK(strstr(text, "backend") != NULL,
          "the staleness did not say what changed: \"%s\"", text);
    CHECK(strstr(text, "12.5") == NULL,
          "a stale entry still printed its number: \"%s\"", text);

    /* Two builds of one commit are two builds: the Makefile gives DeepSeek
     * its own flags, so "same commit" is not "same engine". */
    char id1[65], id2[65], id3[65];
    lmb_cal_build_id(id1, sizeof id1, "cc", "-O2 -fopenmp");
    lmb_cal_build_id(id2, sizeof id2, "cc", "-O2 -fopenmp -DCOLI_V4_GPU_TIER");
    lmb_cal_build_id(id3, sizeof id3, "cc", "-O2 -fopenmp");
    CHECK(strcmp(id1, id2), "two different flag sets produced the same build id");
    CHECK(!strcmp(id1, id3), "the same flags produced different build ids");

    printf("CALIBRATION KEY: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
