/* Execution policy is a resource contract, not a GPU hardware detector.
 * Household plans currently budget CPU RAM only. Do not let an upstream
 * adapter's automatic policy silently turn that approval into a GPU plan. */
#ifndef LUMABRI_BACKEND_POLICY_H
#define LUMABRI_BACKEND_POLICY_H

#include <stdint.h>
#include <string.h>
#include "lumabri_segment.h"

#define LMB_EXEC_BACKEND_MASK (LMB_SEG_CAP_CPU | LMB_SEG_CAP_CUDA | \
    LMB_SEG_CAP_HIP | LMB_SEG_CAP_METAL | LMB_SEG_CAP_VULKAN)

static inline int lmb_backend_request(const char *policy, uint64_t *mask) {
    if (!mask) return -1;
    *mask = 0;
    /* Preserve the low-level CLI's existing adapter-selected policy. */
    if (!policy || !strcmp(policy, "auto")) return 0;
    if (!strcmp(policy, "cpu")) { *mask = LMB_SEG_CAP_CPU; return 0; }
    /* GPU choices require verified adapter/VRAM contracts first. A typo or
     * unsupported policy must not silently fall back to automatic execution. */
    return -1;
}

/* The approval is a CPU RAM contract: every assigned weight is resident in
 * RAM. A Segment may additionally keep copies on a CUDA device and compute
 * there; that changes neither the reservation nor what a donor approved, so
 * it satisfies the CPU policy. Any other backend, or a GPU without the CPU
 * path behind it, does not. */
static inline int lmb_backend_matches(uint64_t requested, uint64_t flags) {
    if (!requested) return 1;
    uint64_t used = flags & LMB_EXEC_BACKEND_MASK;
    return requested == LMB_SEG_CAP_CPU &&
           (used == LMB_SEG_CAP_CPU || used == (LMB_SEG_CAP_CPU | LMB_SEG_CAP_CUDA));
}

/* What a Segment asks of its adapter. CUDA is added to an approved CPU policy
 * only in a build that links the CUDA tier, for the engine that implements
 * it, and when the launcher asked for it. */
static inline uint64_t lmb_backend_segment_request(uint64_t policy, const char *gpu,
                                                   const char *engine_id) {
#ifdef LMB_SEGMENT_CUDA
    if (policy == LMB_SEG_CAP_CPU && gpu && !strcmp(gpu, "cuda") &&
        engine_id && !strcmp(engine_id, "qwen36"))
        return policy | LMB_SEG_CAP_CUDA;
#else
    (void)gpu; (void)engine_id;
#endif
    return policy;
}

static inline const char *lmb_backend_built(void) {
#ifdef LMB_SEGMENT_CUDA
    return "cpu cuda";
#else
    return "cpu";
#endif
}

#endif
