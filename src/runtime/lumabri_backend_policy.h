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

static inline int lmb_backend_matches(uint64_t requested, uint64_t flags) {
    if (!requested) return 1;
    return requested == LMB_SEG_CAP_CPU &&
           (flags & LMB_EXEC_BACKEND_MASK) == LMB_SEG_CAP_CPU;
}

#endif
