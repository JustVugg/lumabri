/* lumabri_segment_discovery.h — tracker control plane for Segment v2.
 *
 * Segment execution is stateful, so discovery returns an immutable placement
 * generation plus a fenced lease for every eligible peer.  Model-specific
 * state remains opaque: the tracker only matches the schema and numeric class
 * published by the Colibri adapter.
 */
#ifndef LUMABRI_SEGMENT_DISCOVERY_H
#define LUMABRI_SEGMENT_DISCOVERY_H

#include "lumabri_segment.h"

#include <stddef.h>
#include <stdint.h>

#define LMB_SEG_DISCOVERY_MAGIC       0x31444753u /* "SGD1" */
#define LMB_SEG_DISCOVERY_VERSION     1u
#define LMB_SEG_MODEL_MAX             64u
#define LMB_SEG_PEER_NAME_MAX         64u
#define LMB_SEG_ADDR_MAX              64u
#define LMB_SEG_ROUTE_MAX             64u

enum {
    LMB_SEG_ADVERT_DRAINING = 1u << 0,
    /* An origin may keep complete coverage online so a new swarm works from
     * minute zero.  Placement prefers ordinary donated ranges and consumes a
     * fallback range only where no non-fallback exact chain is available. */
    LMB_SEG_ADVERT_FALLBACK = 1u << 1,
};

enum {
    LMB_SEG_TRANSPORT_DIRECT = 1u << 0,
    /* Reserved until the tracker forwards Segment frames; an SREG control
     * connection alone is not evidence that the Segment data plane relays. */
    LMB_SEG_TRANSPORT_RELAY = 1u << 1,
};

typedef struct {
    char peer_name[LMB_SEG_PEER_NAME_MAX];
    char addr[LMB_SEG_ADDR_MAX];
    char model[LMB_SEG_MODEL_MAX];
    uint8_t model_root[LMB_SEG_ROOT_BYTES];
    uint8_t tokenizer_root[LMB_SEG_ROOT_BYTES];
    uint32_t layer_begin;
    uint32_t layer_end;
    uint32_t max_context;
    uint32_t max_rows;
    uint32_t state_dtype;
    uint32_t state_width;
    uint32_t max_sessions;
    uint32_t active_sessions;
    uint32_t queue_depth;
    uint32_t inflight;
    uint32_t flags;
    uint64_t capabilities;
    uint64_t resident_ram_bytes;
    uint64_t resident_vram_bytes;
    char engine_id[LMB_SEG_ENGINE_MAX];
    char state_schema[LMB_SEG_SCHEMA_MAX];
    char numeric_class[LMB_SEG_NUMERIC_CLASS_MAX];
} LmbSegAdvert;

typedef struct {
    char model[LMB_SEG_MODEL_MAX];
    uint8_t model_root[LMB_SEG_ROOT_BYTES];
    uint8_t tokenizer_root[LMB_SEG_ROOT_BYTES];
    uint32_t layer_begin;
    uint32_t layer_end;
    uint32_t context_tokens;
    uint32_t rows;
    uint32_t state_dtype;
    uint32_t state_width;
    uint64_t required_capabilities;
    char engine_id[LMB_SEG_ENGINE_MAX];
    char state_schema[LMB_SEG_SCHEMA_MAX];
    char numeric_class[LMB_SEG_NUMERIC_CLASS_MAX];
} LmbSegQuery;

typedef struct {
    LmbSegAdvert advert;
    LmbSegOwner owner;
    uint32_t transport;
} LmbSegRouteEntry;

/* Fixed-size by design: copying this value produces an immutable routing
 * snapshot with no shared allocation and no tracker access on the inference
 * path. Entries include replicas; complete says at least one exact-boundary
 * executor chain covers the complete requested range. */
typedef struct {
    uint64_t route_generation;
    uint64_t fetched_at_ms;
    uint32_t complete;
    uint32_t count;
    LmbSegRouteEntry entries[LMB_SEG_ROUTE_MAX];
} LmbSegRouteSnapshot;

int lmb_seg_advert_valid(const LmbSegAdvert *advert);
int lmb_seg_query_valid(const LmbSegQuery *query);
int lmb_seg_advert_compatible(const LmbSegAdvert *advert,
                              const LmbSegQuery *query);
int lmb_seg_route_complete(const LmbSegRouteSnapshot *snapshot,
                           const LmbSegQuery *query);

int lmb_seg_advert_encode(const LmbSegAdvert *advert,
                          uint8_t **body, uint32_t *body_len);
int lmb_seg_advert_decode(const void *body, size_t body_len,
                          LmbSegAdvert *advert);
int lmb_seg_query_encode(const LmbSegQuery *query,
                         uint8_t **body, uint32_t *body_len);
int lmb_seg_query_decode(const void *body, size_t body_len,
                         LmbSegQuery *query);
int lmb_seg_route_encode(const LmbSegRouteSnapshot *snapshot,
                         uint8_t **body, uint32_t *body_len);
int lmb_seg_route_decode(const void *body, size_t body_len,
                         LmbSegRouteSnapshot *snapshot);
int lmb_seg_registration_reply_encode(const LmbSegOwner *owner,
                                      uint8_t **body, uint32_t *body_len);
int lmb_seg_registration_reply_decode(const void *body, size_t body_len,
                                      LmbSegOwner *owner);

/* One synchronous fetch for startup/tests. Production chat uses the worker
 * below so inference never contacts the tracker. */
int lmb_seg_routes_fetch(const char *tracker, const LmbSegQuery *query,
                         LmbSegRouteSnapshot *snapshot);

typedef struct LmbSegDiscovery LmbSegDiscovery;

LmbSegDiscovery *lmb_seg_discovery_start(const char *tracker,
                                         const LmbSegQuery *query,
                                         uint32_t refresh_ms);
/* Copies the current immutable value. Returns 1 when a snapshot has been
 * published, 0 before the first successful tracker response, or -1. */
int lmb_seg_discovery_snapshot(LmbSegDiscovery *discovery,
                               LmbSegRouteSnapshot *snapshot);
void lmb_seg_discovery_stop(LmbSegDiscovery *discovery);

#endif /* LUMABRI_SEGMENT_DISCOVERY_H */
