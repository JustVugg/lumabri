/* Household plan transaction. An inventory report grants no execution rights.
 * A donor accepts one immutable allocation; COMMIT cannot change its scope. */
#ifndef LUMABRI_HOME_H
#define LUMABRI_HOME_H
#include "lumabri_inventory.h"
#include "lumabri_families.h"
#include "lumabri_home_hybrid.h"

#define LMB_HOME_VERSION 1u
#define LMB_HOME_LEASE_MS 15000u
#define LMB_HOME_DECISION_MS 120000u

typedef enum {
    LMB_HOME_IDLE = 0, LMB_HOME_PENDING, LMB_HOME_ACCEPTED,
    LMB_HOME_LOADING, LMB_HOME_SEGMENT_READY, LMB_HOME_STARTING_HOST,
    LMB_HOME_READY, LMB_HOME_REJECTED, LMB_HOME_FAILED, LMB_HOME_CLOSED
} LmbHomePhase;

typedef struct {
    uint8_t id[32], requester[32], edge_peer[32], model_root[32];
    char model[64], model_type[64], tracker[256];
    uint32_t begin, end, layers, context, threads, max_new;
    uint64_t ram_bytes, edge_ram_bytes, disk_bytes, model_bytes;
    uint32_t runs_edge;
    uint32_t hybrid_role;
    LmbHybridRoutes hybrid;
} LmbHomeOffer;

typedef struct {
    LmbHomeOffer offer;
    LmbHomePhase phase;
    uint64_t last_seen_ms, decision_deadline_ms;
    int reservation_held;
    char reason[160];
} LmbHomeTransaction;

static LMB_MAYBE_UNUSED const char *lmb_home_phase_name(LmbHomePhase p) {
    switch (p) {
    case LMB_HOME_IDLE: return "Available";
    case LMB_HOME_PENDING: return "Waiting for your approval";
    case LMB_HOME_ACCEPTED: return "Accepted; waiting for other computers";
    case LMB_HOME_LOADING: return "Loading the accepted layers";
    case LMB_HOME_SEGMENT_READY: return "Layers ready; waiting for the complete chain";
    case LMB_HOME_STARTING_HOST: return "Starting chat host";
    case LMB_HOME_READY: return "Ready for chat";
    case LMB_HOME_REJECTED: return "Declined";
    case LMB_HOME_FAILED: return "Failed";
    case LMB_HOME_CLOSED: return "Released";
    default: return "Invalid state";
    }
}

static LMB_MAYBE_UNUSED int lmb_home_nonzero(const uint8_t *p, size_t n) {
    uint8_t any = 0;
    while (n--) any |= *p++;
    return any != 0;
}

static LMB_MAYBE_UNUSED int lmb_home_offer_valid(const LmbHomeOffer *o) {
    if (o->hybrid_role > LMB_HYBRID_ACCELERATOR) return 0;
    if (o->hybrid_role) {
        const LmbModelFamily *f = lmb_family_for(o->model_type);
        if (!f || strcmp(f->segment_id, "olmoe")) return 0;
        if (o->hybrid_role == LMB_HYBRID_COORDINATOR) {
            if (!o->runs_edge || o->begin || o->end != o->layers ||
                !lmb_hybrid_routes_valid(&o->hybrid, 0) ||
                memcmp(o->hybrid.allocation, o->id, 32) || memcmp(o->hybrid.root, o->model_root, 32)) return 0;
            for (uint32_t i = 0; i < o->hybrid.count; i++)
                if (o->hybrid.peers[i].end > o->layers || !memcmp(o->hybrid.peers[i].key, o->edge_peer, 32)) return 0;
        } else if (o->runs_edge || o->hybrid.count) return 0;
    } else if (o->hybrid.count) return 0;
    if (!lmb_home_nonzero(o->id, 32) || !lmb_home_nonzero(o->requester, 32) ||
        !lmb_home_nonzero(o->edge_peer, 32) || !lmb_home_nonzero(o->model_root, 32) ||
        !o->model[0] || !o->tracker[0] || !lmb_family_for(o->model_type) ||
        !o->layers || o->layers > 1048576 || o->begin >= o->end || o->end > o->layers ||
        !o->context || o->context > 1048576 || !o->threads || o->threads > 256 ||
        !o->max_new || o->max_new > 4096 || o->runs_edge > 1 ||
        !o->ram_bytes || o->ram_bytes > (1ull << 50) ||
        o->edge_ram_bytes >= o->ram_bytes || o->ram_bytes - o->edge_ram_bytes < (32u << 20) ||
        (!!o->edge_ram_bytes != !!o->runs_edge) ||
        !o->disk_bytes || !o->model_bytes || o->model_bytes > (1ull << 50) ||
        o->disk_bytes > (1ull << 51)) return 0;
    /* Model names become cache namespaces, never paths supplied by a peer. */
    for (const unsigned char *p = (const unsigned char *)o->model; *p; p++)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
    return !lmb_inventory_text(o->tracker);
}

static LMB_MAYBE_UNUSED int lmb_home_offer_pack(LmbBuf *b, const LmbHomeOffer *o) {
    if (!lmb_home_offer_valid(o) || lmb_buf_u32(b, o->hybrid_role ? 2u : LMB_HOME_VERSION)) return -1;
#define HOME_KEY(f) if (lmb_buf_bytes(b, o->f, sizeof o->f)) return -1
    HOME_KEY(id); HOME_KEY(requester); HOME_KEY(edge_peer); HOME_KEY(model_root);
#undef HOME_KEY
#define HOME_STR(f) if (lmb_buf_str(b, o->f)) return -1
    HOME_STR(model); HOME_STR(model_type); HOME_STR(tracker);
#undef HOME_STR
#define HOME_U32(f) if (lmb_buf_u32(b, o->f)) return -1
    HOME_U32(begin); HOME_U32(end); HOME_U32(layers); HOME_U32(context);
    HOME_U32(threads); HOME_U32(max_new); HOME_U32(runs_edge);
#undef HOME_U32
#define HOME_U64(f) if (lmb_buf_u64(b, o->f)) return -1
    HOME_U64(ram_bytes); HOME_U64(edge_ram_bytes); HOME_U64(disk_bytes); HOME_U64(model_bytes);
#undef HOME_U64
    if (o->hybrid_role && (lmb_buf_u32(b, o->hybrid_role) ||
        (o->hybrid_role == LMB_HYBRID_COORDINATOR && lmb_hybrid_routes_pack(b, &o->hybrid, 0)))) return -1;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_home_offer_unpack(LmbCur *c, LmbHomeOffer *o) {
    memset(o, 0, sizeof *o);
    uint32_t version;
    if (lmb_cur_u32(c, &version) || (version != LMB_HOME_VERSION && version != 2u)) return -1;
#define HOME_KEY(f) do { if (c->off > c->len || sizeof o->f > c->len - c->off) return -1; \
    memcpy(o->f, c->p + c->off, sizeof o->f); c->off += sizeof o->f; } while (0)
    HOME_KEY(id); HOME_KEY(requester); HOME_KEY(edge_peer); HOME_KEY(model_root);
#undef HOME_KEY
#define HOME_STR(f) if (lmb_inventory_string(c, o->f, sizeof o->f)) return -1
    HOME_STR(model); HOME_STR(model_type); HOME_STR(tracker);
#undef HOME_STR
#define HOME_U32(f) if (lmb_cur_u32(c, &o->f)) return -1
    HOME_U32(begin); HOME_U32(end); HOME_U32(layers); HOME_U32(context);
    HOME_U32(threads); HOME_U32(max_new); HOME_U32(runs_edge);
#undef HOME_U32
#define HOME_U64(f) if (lmb_cur_u64(c, &o->f)) return -1
    HOME_U64(ram_bytes); HOME_U64(edge_ram_bytes); HOME_U64(disk_bytes); HOME_U64(model_bytes);
#undef HOME_U64
    if (version == 2u && (lmb_cur_u32(c, &o->hybrid_role) || !o->hybrid_role ||
        (o->hybrid_role == LMB_HYBRID_COORDINATOR && lmb_hybrid_routes_unpack(c, &o->hybrid, 0)))) return -1;
    return c->off == c->len && lmb_home_offer_valid(o) ? 0 : -1;
}

static LMB_MAYBE_UNUSED int lmb_home_offer_begin(LmbHomeTransaction *t,
    const LmbHomeOffer *o, const char *household_tracker,
    uint64_t ram_limit, uint64_t disk_limit, uint64_t now_ms) {
    if (t->phase != LMB_HOME_IDLE && t->phase != LMB_HOME_REJECTED &&
        t->phase != LMB_HOME_FAILED && t->phase != LMB_HOME_CLOSED) return -1;
    if (t->reservation_held || !lmb_home_offer_valid(o) ||
        strcmp(o->tracker, household_tracker) || o->ram_bytes > ram_limit ||
        o->disk_bytes > disk_limit) return -1;
    memset(t, 0, sizeof *t);
    t->offer = *o;
    t->phase = LMB_HOME_PENDING;
    t->last_seen_ms = now_ms;
    t->decision_deadline_ms = now_ms + LMB_HOME_DECISION_MS;
    return 0;
}

/* Acquire the machine-wide resource lease BEFORE calling this function with
 * approve=1; release it on any error. This state alone is not a reservation. */
static LMB_MAYBE_UNUSED int lmb_home_decide(LmbHomeTransaction *t, int approve,
    uint64_t current_ram, uint64_t current_disk, uint64_t now_ms) {
    if (t->phase != LMB_HOME_PENDING || now_ms >= t->decision_deadline_ms) return -1;
    if (!approve) { t->phase = LMB_HOME_REJECTED; return 0; }
    if (t->offer.ram_bytes > current_ram || t->offer.disk_bytes > current_disk) return -1;
    t->reservation_held = 1;
    t->phase = LMB_HOME_ACCEPTED;
    return 0;
}

/* COMMIT contains only the immutable transaction ID. No caller can enlarge a
 * range, add GPU use or substitute a model after the donor has clicked Yes. */
static LMB_MAYBE_UNUSED int lmb_home_commit(LmbHomeTransaction *t,
                                           const uint8_t id[32]) {
    if (t->phase != LMB_HOME_ACCEPTED || !t->reservation_held ||
        memcmp(id, t->offer.id, 32)) return -1;
    t->phase = LMB_HOME_LOADING;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_home_mark_segment_ready(LmbHomeTransaction *t) {
    if (t->phase != LMB_HOME_LOADING || !t->reservation_held) return -1;
    t->phase = LMB_HOME_SEGMENT_READY;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_home_start_host(LmbHomeTransaction *t,
                                               const uint8_t id[32]) {
    if (t->phase != LMB_HOME_SEGMENT_READY || !t->offer.runs_edge ||
        memcmp(id, t->offer.id, 32)) return -1;
    t->phase = LMB_HOME_STARTING_HOST;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_home_expired(const LmbHomeTransaction *t,
                                            uint64_t now_ms) {
    if (t->phase == LMB_HOME_IDLE || t->phase >= LMB_HOME_REJECTED) return 0;
    return now_ms < t->last_seen_ms || now_ms - t->last_seen_ms >= LMB_HOME_LEASE_MS ||
        (t->phase == LMB_HOME_PENDING && now_ms >= t->decision_deadline_ms);
}

/* Caller stops and reaps all owned processes BEFORE releasing the lease and
 * clearing this flag. A dead control connection must not orphan an engine. */
static LMB_MAYBE_UNUSED void lmb_home_released(LmbHomeTransaction *t,
                                              LmbHomePhase final, const char *why) {
    t->reservation_held = 0;
    t->phase = final == LMB_HOME_REJECTED || final == LMB_HOME_FAILED ? final : LMB_HOME_CLOSED;
    snprintf(t->reason, sizeof t->reason, "%s", why ? why : "");
}
#endif
