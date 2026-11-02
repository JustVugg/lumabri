/* Observed work on a replica's participating machines. The FIFO reports
 * compute turns, not requests, tokens, queueing time or a capacity promise. */
#ifndef LMB_REPLICA_LOAD_H
#define LMB_REPLICA_LOAD_H
#include "lumabri_inventory.h"

#define LMB_REPLICA_LOAD_MAX_AGE_MS (2u * LMB_INVENTORY_HEARTBEAT_MS)
typedef struct {
    uint32_t nodes, max_pending, total_pending, max_age_ms;
} LmbReplicaLoad;

/* In-flight requests through this operator's gateway are a separate signal
 * from the workers' FIFO compute turns. Never add the two or call this a
 * cluster-wide count: other gateways and direct clients are not observed. */
typedef struct {
    uint32_t known, max_active, total_active;
} LmbReplicaGatewayLoad;

static inline int lmb_replica_gateway_overlap(uint32_t *counts,const uint8_t *nodes,uint32_t count,
    const uint8_t *active_nodes,uint32_t active_count) {
    if (!counts || !nodes || !active_nodes || !count || !active_count ||
        count>LMB_INVENTORY_MAX || active_count>LMB_INVENTORY_MAX) return -1;
    for (uint32_t i=0;i<count;i++) {
        int duplicate=0;
        for (uint32_t k=0;k<i;k++) if (!memcmp(nodes+i*32,nodes+k*32,32)) duplicate=1;
        if (duplicate) continue;
        for (uint32_t j=0;j<active_count;j++) if (!memcmp(nodes+i*32,active_nodes+j*32,32)) {
            if (counts[i]==UINT32_MAX) return -1;
            counts[i]++;break;
        }
    }
    return 0;
}

static inline int lmb_replica_gateway_compare(const LmbReplicaGatewayLoad *a,const LmbReplicaGatewayLoad *b) {
    if (a->max_active!=b->max_active) return a->max_active<b->max_active ? -1 : 1;
    if (a->total_active!=b->total_active) return a->total_active<b->total_active ? -1 : 1;
    return 0;
}

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
