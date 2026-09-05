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
#include <stdio.h>
#include <string.h>
#include "lumabri_planner.h"

#define LMB_CAL_NODES_MAX 32

typedef struct {
    char model_root[65];        /* the checkpoint's signed identity */
    char adapter[32];
    uint32_t adapter_abi;
    char numeric_class[48];     /* what the nodes agreed to compute in */
    char commit_lumabri[41];
    char commit_colibri[41];
    char build_id[65];          /* compiler, flags, engine configuration */
    char plan_kind[16];         /* segment or expert */
    uint32_t goal;              /* LmbPlanGoal without including cluster.h */
    uint32_t nodes;
    char node_id[LMB_CAL_NODES_MAX][64];
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
    uint32_t samples;           /* how many runs the median came from */
} LmbCalibration;

/* Everything in the key, in order, and nothing outside it. Deliberately not
 * a hash: a mismatch has to be able to say WHICH field moved, because "your
 * measurement is stale" without a reason is how people learn to ignore it. */
static LMB_UNUSED const char *lmb_cal_mismatch(const LmbCalKey *a,
                                               const LmbCalKey *b) {
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
    const char *moved = lmb_cal_mismatch(&have->key, want);
    if (moved) {
        snprintf(out, cap, "stale (%.20s changed) — recalibrate", moved);
        return;
    }
    snprintf(out, cap, "%.2f tok/s", have->decode_tok_s);
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
