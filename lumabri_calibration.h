/* lumabri_calibration.h — a measurement, and the exact conditions under
 * which it may be shown again.
 *
 * The catalogue's rule is that no speed appears without a calibration. That
 * rule is worth nothing unless a calibration also knows when it has STOPPED
 * being true, because a stale number is worse than no number: no number
 * prompts a measurement, a stale one prevents it.
 *
 * So a measurement carries the conditions that produced it, and a lookup is
 * an exact match on all of them. Change a machine, a backend, the ranges,
 * resident-versus-disk, the thread count, the context, the session count,
 * either commit, or the compiler flags — and the value is not adjusted, not
 * interpolated, not "close enough". It is stale, and the screen says so.
 *
 * The build id is in there because two binaries from the same commit can
 * perform differently: this Makefile alone gives DeepSeek its own DS_CFLAGS,
 * pin-slot and ramp settings, so "same commit" is not "same engine". */
#ifndef LUMABRI_CALIBRATION_H
#define LUMABRI_CALIBRATION_H

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lumabri_planner.h"

#define LMB_CAL_NODES_MAX 32

typedef struct {
    char model_root[65];        /* content identity, checked against the signed routing root */
    char adapter[32];
    uint32_t adapter_abi;
    char numeric_class[97];     /* Segment numeric class, including terminator */
    char commit_lumabri[41];    /* optional provenance; exact binary IDs remain required */
    char commit_colibri[41];
    char build_id[65];          /* compiler, flags, engine configuration */
    char plan_kind[16];         /* segment, expert or approved household hybrid */
    uint32_t goal;              /* LmbPlanGoal without including cluster.h */
    uint32_t nodes;
    uint32_t edge_node;         /* index in this key's ordered ranges */
    char node_id[LMB_CAL_NODES_MAX][65]; /* 32-byte identity in hex + NUL */
    char node_hardware_id[LMB_CAL_NODES_MAX][65];
    char node_build_id[LMB_CAL_NODES_MAX][65];
    char node_backend[LMB_CAL_NODES_MAX][16];
    uint32_t layer_begin[LMB_CAL_NODES_MAX];
    uint32_t layer_end[LMB_CAL_NODES_MAX];
    uint32_t threads[LMB_CAL_NODES_MAX];
    uint8_t from_disk[LMB_CAL_NODES_MAX];
    uint32_t context;
    uint32_t sessions;
} LmbCalKey;

typedef struct {
    LmbCalKey key;
    double decode_tok_s;
    double ttft_seconds;
    double measured_at;         /* wall clock, for the operator, not for matching */
    uint32_t samples;           /* completed turns represented by this record */
    uint32_t prompt_tokens;     /* observed workload, not the configured context limit */
    uint32_t generated_tokens;
    uint32_t stage_count;        /* zero on legacy/no per-range observations */
    double stage_decode_seconds[LMB_CAL_NODES_MAX]; /* seconds per RUN, includes transport */
} LmbCalibration;

/* Never compare unterminated fields or let two equally incomplete records
 * establish a match. These checks also bound data loaded from disk. */
static LMB_UNUSED int lmb_cal_text(const char *s, size_t cap) {
    const char *end = memchr(s, 0, cap);
    if (!end || end == s) return 0;
    for (; s != end; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return 0;
    return 1;
}

static LMB_UNUSED int lmb_cal_key_valid(const LmbCalKey *k) {
    if (!k || !k->nodes || k->nodes > LMB_CAL_NODES_MAX || k->edge_node >= k->nodes ||
        !k->adapter_abi || !k->context || !k->sessions || k->goal > 1) return 0;
#define CAL_TEXT(f) if (!lmb_cal_text(k->f, sizeof k->f)) return 0
    CAL_TEXT(model_root); CAL_TEXT(adapter); CAL_TEXT(numeric_class);
    if (!memchr(k->commit_lumabri, 0, sizeof k->commit_lumabri) ||
        !memchr(k->commit_colibri, 0, sizeof k->commit_colibri)) return 0;
    if (k->commit_lumabri[0]) { CAL_TEXT(commit_lumabri); }
    if (k->commit_colibri[0]) { CAL_TEXT(commit_colibri); }
    CAL_TEXT(build_id); CAL_TEXT(plan_kind);
    if (strcmp(k->plan_kind, "segment") && strcmp(k->plan_kind, "expert") && strcmp(k->plan_kind, "hybrid")) return 0;
    for (uint32_t i = 0; i < k->nodes; i++) {
        CAL_TEXT(node_id[i]); CAL_TEXT(node_hardware_id[i]);
        CAL_TEXT(node_build_id[i]); CAL_TEXT(node_backend[i]);
        if (!k->threads[i] || k->layer_begin[i] >= k->layer_end[i] ||
            k->from_disk[i] > 1) return 0;
    }
#undef CAL_TEXT
    return 1;
}

static LMB_UNUSED int lmb_cal_valid(const LmbCalibration *c) {
    if (!c || (c->stage_count && c->stage_count != c->key.nodes) || c->stage_count > LMB_CAL_NODES_MAX) return 0;
    for (uint32_t i = 0; i < c->stage_count; i++)
        if (!isfinite(c->stage_decode_seconds[i]) || c->stage_decode_seconds[i] <= 0 ||
            c->stage_decode_seconds[i] > 1e9) return 0;
    return c && lmb_cal_key_valid(&c->key) && c->samples &&
           c->prompt_tokens && c->prompt_tokens <= c->key.context &&
           c->generated_tokens > 1 && c->generated_tokens <= 1048576 &&
           isfinite(c->decode_tok_s) && c->decode_tok_s > 0 &&
           isfinite(c->ttft_seconds) && c->ttft_seconds >= 0 &&
           isfinite(c->measured_at) && c->measured_at > 0;
}

/* Everything in the key, in order, and nothing outside it. Deliberately not
 * a hash: a mismatch has to be able to say WHICH field moved, because "your
 * measurement is stale" without a reason is how people learn to ignore it. */
static LMB_UNUSED const char *lmb_cal_mismatch(const LmbCalKey *a,
                                               const LmbCalKey *b) {
    if (!lmb_cal_key_valid(a) || !lmb_cal_key_valid(b)) return "incomplete measurement conditions";
    if (strcmp(a->model_root, b->model_root))       return "the checkpoint";
    if (strcmp(a->adapter, b->adapter))             return "the adapter";
    if (a->adapter_abi != b->adapter_abi)           return "the adapter ABI";
    if (strcmp(a->numeric_class, b->numeric_class)) return "the numeric class";
    if (strcmp(a->commit_lumabri, b->commit_lumabri)) return "the Lumabri build";
    if (strcmp(a->commit_colibri, b->commit_colibri)) return "the Colibri build";
    if (strcmp(a->build_id, b->build_id))           return "the compiler flags";
    if (strcmp(a->plan_kind, b->plan_kind))         return "the execution plan";
    if (a->goal != b->goal)                         return "the planning goal";
    if (a->context != b->context)                   return "the context length";
    if (a->sessions != b->sessions)                 return "the session count";
    if (a->nodes != b->nodes)                       return "the number of machines";
    if (a->edge_node != b->edge_node)               return "the Edge host";
    if (a->nodes > LMB_CAL_NODES_MAX || b->nodes > LMB_CAL_NODES_MAX)
        return "an invalid machine count";
    for (uint32_t i = 0; i < a->nodes; i++) {
        if (strcmp(a->node_id[i], b->node_id[i]))   return "which machines";
        if (strcmp(a->node_hardware_id[i], b->node_hardware_id[i]))
            return "the machine hardware";
        if (strcmp(a->node_build_id[i], b->node_build_id[i]))
            return "a node build";
        if (strcmp(a->node_backend[i], b->node_backend[i]))
            return "a node backend";
        if (a->layer_begin[i] != b->layer_begin[i] ||
            a->layer_end[i] != b->layer_end[i])     return "the assigned ranges";
        if (a->threads[i] != b->threads[i])         return "the thread counts";
        if (a->from_disk[i] != b->from_disk[i])
            return "whether a range is resident or streamed";
    }
    return NULL;                /* every condition still holds */
}

static LMB_UNUSED int lmb_cal_matches(const LmbCalKey *a, const LmbCalKey *b) {
    return lmb_cal_mismatch(a, b) == NULL;
}

/* What the catalogue prints in the speed column.
 *
 * Three outcomes and they are not interchangeable. A match is a number, and
 * it is the only case where one appears. A mismatch is not a worse number —
 * it is the absence of one, plus the reason, so the operator knows what to
 * put back or what to re-measure. No record at all is simply "not
 * calibrated", which is the honest state of a plan nobody has run. */
static LMB_UNUSED void lmb_cal_speed_text(const LmbCalibration *have,
                                          const LmbCalKey *want,
                                          char *out, size_t cap) {
    if (!have) { snprintf(out, cap, "not calibrated"); return; }
    if (!lmb_cal_valid(have)) { snprintf(out, cap, "invalid measurement — recalibrate"); return; }
    const char *moved = lmb_cal_mismatch(&have->key, want);
    if (moved) {
        snprintf(out, cap, "stale (%.20s changed) — recalibrate", moved);
        return;
    }
    snprintf(out, cap, "%.2f tok/s (last)", have->decode_tok_s);
}

/* Compose the build id from what actually varies between two binaries of the
 * same commit: the compiler and the flags the engine was built with. */
static LMB_UNUSED void lmb_cal_build_id(char *out, size_t cap,
                                        const char *cc, const char *cflags) {
    /* FNV-1a over both, so it is short enough to print and specific enough
     * to differ when a profile does. */
    uint64_t h = 1469598103934665603ull;
    const char *parts[2] = { cc ? cc : "", cflags ? cflags : "" };
    for (int p = 0; p < 2; p++) {
        for (const char *c = parts[p]; *c; c++) {
            h ^= (unsigned char)*c;
            h *= 1099511628211ull;
        }
        /* Separate ("ab", "c") from ("a", "bc"). */
        h ^= 0xffu;
        h *= 1099511628211ull;
    }
    snprintf(out, cap, "%016llx", (unsigned long long)h);
}

#endif /* LUMABRI_CALIBRATION_H */
