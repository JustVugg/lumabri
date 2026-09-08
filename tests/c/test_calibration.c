/* A measurement is only reusable under the conditions that produced it.
 *
 * The point of these assertions is not that matching works — it is that
 * every single field can invalidate a number on its own. A key that ignores
 * one field is a key that will one day show a GPU measurement for a CPU run,
 * or a two-machine number for a four-machine plan, and be believed. */
#include <stdio.h>
#include <string.h>
#include "lumabri_calibration_store.h"

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
    snprintf(k.plan_kind, sizeof k.plan_kind, "segment");
    k.goal = 0;
    k.nodes = 2; k.context = 4096; k.sessions = 1;
    for (int i = 0; i < 2; i++) {
        snprintf(k.node_id[i], sizeof k.node_id[i], "node%d", i);
        snprintf(k.node_hardware_id[i], sizeof k.node_hardware_id[i], "hw%d", i);
        snprintf(k.node_build_id[i], sizeof k.node_build_id[i], "build%d", i);
        snprintf(k.node_backend[i], sizeof k.node_backend[i], "cpu");
        k.layer_begin[i] = (uint32_t)i * 22;
        k.layer_end[i] = (uint32_t)(i + 1) * 22;
        k.threads[i] = 8;
        k.from_disk[i] = 0;
    }
    return k;
}

static void record_tests(void) {
    LmbCalibration r = { .key = base(), .decode_tok_s = 12.5,
        .ttft_seconds = 1.25, .measured_at = 1234567, .samples = 3 }, got;
    memset(r.key.model_root, 'a', 64); r.key.model_root[64] = 0;
    LmbBuf encoded = {0};
    CHECK(!lmb_cal_encode(&r, &encoded), "record encoding failed");
    if (!encoded.p) return;
    CHECK(!lmb_cal_decode(encoded.p, encoded.len, &got) && lmb_cal_matches(&r.key, &got.key) &&
          r.decode_tok_s == got.decode_tok_s && r.ttft_seconds == got.ttft_seconds &&
          r.measured_at == got.measured_at && r.samples == got.samples, "record round trip failed");
    for (size_t i = 0; i < encoded.len; i++) {
        got = r;
        CHECK(lmb_cal_decode(encoded.p, i, &got) && !got.samples, "truncated record accepted at %zu", i);
    }
    encoded.p[12] ^= 1; got = r;
    CHECK(lmb_cal_decode(encoded.p, encoded.len, &got) && !got.samples, "corrupt record accepted");
    encoded.p[12] ^= 1;
    /* Exercise parsing, not just the checksum: re-sign truncated payloads
     * and malformed length/count fields so integrity does not hide bugs. */
    uint8_t *candidate = malloc(encoded.len);
    CHECK(candidate != NULL, "cannot allocate malformed record fixture");
    if (candidate) {
        for (size_t len = 40; len < encoded.len; len++) {
            memcpy(candidate, encoded.p, len);
            LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha, candidate, len - 32);
            lmb_sha_final(&sha, candidate + len - 32);
            got = r;
            CHECK(lmb_cal_decode(candidate, len, &got) && !got.samples,
                  "checksummed truncated payload accepted at %zu", len);
        }
        for (size_t i = 8; i < encoded.len - 32; i++) {
            memcpy(candidate, encoded.p, encoded.len); candidate[i] ^= 0x80;
            LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha, candidate, encoded.len - 32);
            lmb_sha_final(&sha, candidate + encoded.len - 32);
            got = r;
            int result = lmb_cal_decode(candidate, encoded.len, &got);
            CHECK(result ? !got.samples : lmb_cal_valid(&got),
                  "mutated record returned invalid data at %zu", i);
        }
        free(candidate);
    }
    char tmp[] = "/tmp/lumabri-cal-record-XXXXXX", directory[256], path[384], link_path[256];
    char *created = mkdtemp(tmp);
    CHECK(created != NULL, "cannot create private test directory");
    if (!created) { free(encoded.p); return; }
    snprintf(directory, sizeof directory, "%s/records", tmp);
    snprintf(path, sizeof path, "%s/%s.cal", directory, r.key.model_root);
    snprintf(link_path, sizeof link_path, "%s/link", tmp);
    CHECK(!lmb_cal_store(directory, &r), "record store failed");
    CHECK(!lmb_cal_load(directory, r.key.model_root, &got) && lmb_cal_matches(&r.key, &got.key), "record load failed");
    r.decode_tok_s = 7.25;
    CHECK(!lmb_cal_store(directory, &r) && !lmb_cal_load(directory, r.key.model_root, &got) &&
          got.decode_tok_s == 7.25, "atomic replacement failed");
    struct stat st;
    CHECK(!stat(path, &st) && !(st.st_mode & 077), "record is not private");
    CHECK(lmb_cal_load(directory, "../../elsewhere", &got) && !got.samples, "unsafe filename accepted");
    CHECK(!symlink(directory, link_path), "cannot prepare directory symlink test");
    CHECK(lmb_cal_load(link_path, r.key.model_root, &got) && !got.samples, "directory symlink accepted");
    CHECK(!unlink(link_path), "cannot remove test directory symlink");
    CHECK(!chmod(path, 0644), "cannot prepare exposed record test");
    CHECK(lmb_cal_load(directory, r.key.model_root, &got) && !got.samples, "non-private record accepted");
    CHECK(!unlink(path) && !symlink("missing", path), "cannot prepare record symlink test");
    CHECK(lmb_cal_load(directory, r.key.model_root, &got) && !got.samples, "record symlink accepted");
    CHECK(!unlink(path) && !mkfifo(path, 0600), "cannot prepare FIFO test");
    CHECK(lmb_cal_load(directory, r.key.model_root, &got) && !got.samples, "FIFO accepted or blocked");
    CHECK(!unlink(path), "cannot remove test FIFO");
    CHECK(!lmb_cal_store(directory, &r), "cannot prepare truncated disk record");
    char linked[384]; snprintf(linked, sizeof linked, "%s/linked", directory);
    CHECK(!link(path, linked), "cannot prepare hardlink test");
    CHECK(lmb_cal_load(directory, r.key.model_root, &got) && !got.samples, "hardlinked record accepted");
    CHECK(!unlink(linked), "cannot remove test hardlink");
    CHECK(!truncate(path, 20), "cannot truncate test record");
    CHECK(lmb_cal_load(directory, r.key.model_root, &got) && !got.samples, "truncated disk record accepted");
    CHECK(!unlink(path) && !rmdir(directory) && !rmdir(tmp), "test record cleanup failed");
    free(encoded.p);
}

int main(void) {
    LmbCalKey a = base(), b = base();
    CHECK(lmb_cal_matches(&a, &b), "two identical keys did not match");

    /* Every field, one at a time. A key that lets any of these through will
     * eventually show somebody the wrong number with full confidence. */
    struct { const char *what; LmbCalKey k; } cases[24];
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
    VARY("plan",            snprintf(v.plan_kind, sizeof v.plan_kind, "expert"));
    VARY("goal",            v.goal = 1);
    VARY("context",         v.context = 8192);
    VARY("sessions",        v.sessions = 4);
    VARY("machine count",   v.nodes = 1);
    VARY("which machine",   snprintf(v.node_id[1], sizeof v.node_id[1], "elsewhere"));
    VARY("node hardware",   snprintf(v.node_hardware_id[1], sizeof v.node_hardware_id[1], "other"));
    VARY("node build",      snprintf(v.node_build_id[1], sizeof v.node_build_id[1], "other"));
    VARY("node backend",    snprintf(v.node_backend[1], sizeof v.node_backend[1], "cuda"));
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
    have.key = base(); have.decode_tok_s = 12.5; have.samples = 3; have.measured_at = 1;
    char text[128];

    lmb_cal_speed_text(NULL, &a, text, sizeof text);
    CHECK(!strcmp(text, "not calibrated"),
          "with no record the column said \"%s\"", text);

    lmb_cal_speed_text(&have, &a, text, sizeof text);
    CHECK(strstr(text, "12.5") != NULL,
          "a matching calibration did not print its number: \"%s\"", text);

    LmbCalKey gpu = base();
    snprintf(gpu.node_backend[0], sizeof gpu.node_backend[0], "cuda");
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
    lmb_cal_build_id(id2, sizeof id2, "ab", "c");
    lmb_cal_build_id(id3, sizeof id3, "a", "bc");
    CHECK(strcmp(id2, id3), "build-id fields were concatenated ambiguously");

    LmbCalKey invalid = base(); invalid.nodes = LMB_CAL_NODES_MAX + 1;
    CHECK(!lmb_cal_matches(&invalid, &invalid),
          "an out-of-bounds machine count was accepted as a calibration key");

    LmbCalKey empty = {0};
    CHECK(!lmb_cal_matches(&empty, &empty), "two empty keys matched");
    CHECK(!lmb_cal_matches(NULL, &a) && !lmb_cal_matches(&a, NULL), "null keys matched");
    invalid = base(); memset(invalid.node_id[0], 'a', sizeof invalid.node_id[0]);
    CHECK(!lmb_cal_matches(&invalid, &invalid), "unterminated identity matched");
    invalid = base(); invalid.numeric_class[0] = '\033';
    CHECK(!lmb_cal_matches(&invalid, &invalid), "terminal controls accepted");
    invalid = base(); invalid.threads[0] = 0;
    CHECK(!lmb_cal_key_valid(&invalid), "unknown thread count accepted");
    invalid = base(); invalid.layer_end[0] = invalid.layer_begin[0];
    CHECK(!lmb_cal_key_valid(&invalid), "empty range accepted");
    invalid = base(); memset(invalid.node_id[0], 'a', 64); invalid.node_id[0][64] = 0;
    memset(invalid.numeric_class, 'x', 96); invalid.numeric_class[96] = 0;
    CHECK(lmb_cal_key_valid(&invalid), "full identity or numeric class did not fit");
    const double invalid_rates[] = {0, -1, NAN, INFINITY};
    for (size_t i = 0; i < sizeof invalid_rates / sizeof invalid_rates[0]; i++) {
        have.decode_tok_s = invalid_rates[i];
        lmb_cal_speed_text(&have, &a, text, sizeof text);
        CHECK(strstr(text, "invalid") && !strstr(text, "tok/s"), "invalid speed printed: %s", text);
    }
    have.decode_tok_s = 12.5; have.samples = 0;
    CHECK(!lmb_cal_valid(&have), "zero samples accepted");
    have.samples = 1; have.measured_at = NAN;
    CHECK(!lmb_cal_valid(&have), "invalid measurement date accepted");

    record_tests();

    printf("CALIBRATION KEY: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
