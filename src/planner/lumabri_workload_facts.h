/* Leased workload observations, not reservations or performance promises. */
#ifndef LUMABRI_WORKLOAD_FACTS_H
#define LUMABRI_WORKLOAD_FACTS_H
#include <stdint.h>
#include <string.h>

enum { LMB_COMPUTE_UNKNOWN = 0, LMB_COMPUTE_LOCAL_FIFO = 1 };
typedef struct {
    uint32_t known, allocations, compute_policy, active, queued;
    uint64_t reserved_bytes;
    uint8_t allocation_set[32]; /* canonical set digest; never conversation data */
} LmbWorkloadFacts;

static inline int lmb_workload_valid(const LmbWorkloadFacts *f, uint64_t total_ram) {
    if (!f || f->known > 1 || f->allocations > 4 || f->compute_policy > LMB_COMPUTE_LOCAL_FIFO ||
        f->active > 1 || f->queued > 32 || f->reserved_bytes > total_ram ||
        (!f->allocations && (f->reserved_bytes || f->active || f->queued)) ||
        (!f->compute_policy && (f->active || f->queued))) return 0;
    uint8_t digest = 0;
    for (unsigned i = 0; i < 32; i++) digest |= f->allocation_set[i];
    if (!f->known) return !f->allocations && !f->compute_policy && !f->active &&
        !f->queued && !f->reserved_bytes && !digest;
    return digest != 0;
}
#endif
