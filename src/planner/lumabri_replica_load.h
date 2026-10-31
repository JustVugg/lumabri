/* Observed work on a replica's participating machines. The FIFO reports
 * compute turns, not requests, tokens, queueing time or a capacity promise. */
#ifndef LMB_REPLICA_LOAD_H
#define LMB_REPLICA_LOAD_H
#include "lumabri_inventory.h"

#define LMB_REPLICA_LOAD_MAX_AGE_MS (2u * LMB_INVENTORY_HEARTBEAT_MS)
typedef struct {
    uint32_t nodes, max_pending, total_pending, max_age_ms;
} LmbReplicaLoad;

/* The caller supplies each distinct machine once, after matching its runtime
 * and approved allocation. Any failed participant invalidates the whole load
 * observation; a missing report must never look like an idle computer. */
static inline int lmb_replica_load_add(LmbReplicaLoad *load, const LmbWorkloadFacts *work,
    uint64_t total_ram, uint64_t reserved_bytes, uint32_t age_ms) {
    if (!load || !work || !work->known || !work->allocations ||
        work->compute_policy!=LMB_COMPUTE_LOCAL_FIFO || !lmb_workload_valid(work,total_ram) ||
        !reserved_bytes || work->reserved_bytes<reserved_bytes ||
        age_ms>LMB_REPLICA_LOAD_MAX_AGE_MS || load->nodes>=LMB_INVENTORY_MAX) return -1;
    uint32_t pending=work->active+work->queued;
    load->nodes++;
    if (pending>load->max_pending) load->max_pending=pending;
    load->total_pending+=pending;
    if (age_ms>load->max_age_ms) load->max_age_ms=age_ms;
    return 0;
}
static inline int lmb_replica_load_valid(const LmbReplicaLoad *load) {
    return load->nodes && load->nodes<=LMB_INVENTORY_MAX &&
        load->max_pending<=33 && load->total_pending>=load->max_pending &&
        load->total_pending<=load->max_pending*load->nodes &&
        load->max_age_ms<=LMB_REPLICA_LOAD_MAX_AGE_MS;
}
static inline int lmb_replica_load_compare(const LmbReplicaLoad *a, const LmbReplicaLoad *b) {
    if (a->max_pending!=b->max_pending) return a->max_pending<b->max_pending ? -1 : 1;
    if (a->total_pending!=b->total_pending) return a->total_pending<b->total_pending ? -1 : 1;
    return 0;
}
#endif
