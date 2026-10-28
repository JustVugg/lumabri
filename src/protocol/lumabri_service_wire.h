/* Common local-service records and fences, independent of OS transport.
 * Journals describe observations; they never authorize replaying an offer. */
#ifndef LUMABRI_SERVICE_WIRE_H
#define LUMABRI_SERVICE_WIRE_H
#include "lumabri_home.h"

#define HOME_SVC_VERSION 1u
#define HOME_SVC_RECORD_VERSION 5u
#define HOME_SVC_MAX 65536u
#define HOME_PREPARATION_RESULTS 8u
enum { HOME_SVC_STATUS = 0, HOME_SVC_STOP, HOME_SVC_ACCEPT, HOME_SVC_DECLINE,
       HOME_SVC_UNLOAD, HOME_SVC_CANCEL, HOME_SVC_NEXT_MODEL,
       HOME_SVC_ALLOW_REPEAT, HOME_SVC_REVOKE_REPEAT };
enum { HOME_SVC_RUNNING = 1, HOME_SVC_DONE, HOME_SVC_FAILED, HOME_SVC_STOPPED };

/* Historical outcomes, not live readiness or permission to load again. An
 * allocation ID is durably assigned before the first OFFER. A controller can
 * reconcile that exact ID after interruption without guessing from names or
 * adopting someone else's newly visible allocation. */
typedef struct {
    uint8_t allocation[32];
    char catalogue_name[64];
    uint32_t ready_observed;
} HomePreparationResult;

typedef struct {
    uint8_t instance[32];
    uint32_t state, phase, retained, threads;
    uint64_t pid, segment_pid, host_pid, ram, revision;
    char role[16], name[64], tracker[256], detail[512];
    LmbHomeOffer offer;
    int has_offer;
    uint32_t model_count;
    uint64_t reserved_total;
    uint32_t compute_enabled, compute_active, compute_queued;
    uint64_t compute_grants;
    uint8_t allocation_set[32];
    uint32_t preparation_results_known, preparation_result_count;
    HomePreparationResult preparation_results[HOME_PREPARATION_RESULTS];
} HomeServiceSnapshot;

static int home_service_role(const char *role) {
    return !strcmp(role, "manager") || !strcmp(role, "donor") ||
           !strcmp(role, "tracker") || !strcmp(role, "prepare") || !strcmp(role, "api");
}

static int home_service_pack(LmbBuf *b, const HomeServiceSnapshot *s) {
    if (lmb_inventory_text(s->role) || lmb_inventory_text(s->name) ||
        lmb_inventory_text(s->tracker) || lmb_inventory_text(s->detail)) return -1;
    if (!home_service_role(s->role) || !s->state || s->state > HOME_SVC_STOPPED ||
        s->phase > LMB_HOME_CLOSED || s->retained > 1 ||
        (s->has_offer && !lmb_home_offer_valid(&s->offer))) return -1;
    if (s->model_count > 4 || s->reserved_total > s->ram) return -1;
    if (s->compute_enabled > 1 || s->compute_active > 1 || s->compute_queued > 32 ||
        (!s->compute_enabled && (s->compute_active || s->compute_queued))) return -1;
    if (s->preparation_results_known > 1 || s->preparation_result_count > HOME_PREPARATION_RESULTS ||
        (s->preparation_result_count && !s->preparation_results_known) ||
        (s->preparation_results_known && strcmp(s->role,"prepare"))) return -1;
    for (uint32_t i=0;i<s->preparation_result_count;i++) {
        const HomePreparationResult *r=&s->preparation_results[i]; unsigned nonzero=0;
        for (unsigned j=0;j<32;j++) nonzero|=r->allocation[j];
        if (!nonzero || r->ready_observed>1 || !r->catalogue_name[0] ||
            strnlen(r->catalogue_name,sizeof r->catalogue_name)==sizeof r->catalogue_name ||
            lmb_inventory_text(r->catalogue_name)) return -1;
        for (uint32_t j=0;j<i;j++)
            if (!memcmp(r->allocation,s->preparation_results[j].allocation,32)) return -1;
    }
    if (lmb_buf_u32(b, HOME_SVC_RECORD_VERSION) || lmb_buf_bytes(b, s->instance, 32) ||
        lmb_buf_u32(b, s->state) || lmb_buf_u32(b, s->phase) ||
        lmb_buf_u32(b, s->retained) || lmb_buf_u32(b, s->threads) ||
        lmb_buf_u64(b, s->pid) || lmb_buf_u64(b, s->segment_pid) ||
        lmb_buf_u64(b, s->host_pid) || lmb_buf_u64(b, s->ram) || lmb_buf_u64(b, s->revision) ||
        lmb_buf_str(b, s->role) || lmb_buf_str(b, s->name) ||
        lmb_buf_str(b, s->tracker) || lmb_buf_str(b, s->detail) ||
        lmb_buf_u32(b, (uint32_t)s->has_offer)) return -1;
    LmbBuf offer = {0};
    int rc = s->has_offer ? lmb_home_offer_pack(&offer, &s->offer) : 0;
    if (!rc) rc = lmb_buf_u32(b, (uint32_t)offer.len) || (offer.len && lmb_buf_bytes(b, offer.p, offer.len));
    free(offer.p);
    if (!rc) rc = lmb_buf_u32(b, s->model_count) || lmb_buf_u64(b, s->reserved_total);
    if (!rc) rc = lmb_buf_u32(b, s->compute_enabled) || lmb_buf_u32(b, s->compute_active) ||
        lmb_buf_u32(b, s->compute_queued) || lmb_buf_u64(b, s->compute_grants);
    if (!rc) rc = lmb_buf_bytes(b, s->allocation_set, 32);
    if (!rc) rc = lmb_buf_u32(b,s->preparation_results_known) || lmb_buf_u32(b,s->preparation_result_count);
    for (uint32_t i=0;i<s->preparation_result_count && !rc;i++) {
        const HomePreparationResult *r=&s->preparation_results[i];
        rc=lmb_buf_bytes(b,r->allocation,32) || lmb_buf_str(b,r->catalogue_name) || lmb_buf_u32(b,r->ready_observed);
    }
    return rc || b->len > HOME_SVC_MAX ? -1 : 0;
}

static int home_service_unpack(const uint8_t *bytes, size_t n, HomeServiceSnapshot *s) {
    memset(s, 0, sizeof *s);
    LmbCur c = {bytes, n, 0}; uint32_t v, has, size;
    if (lmb_cur_u32(&c, &v) || v < 1 || v > HOME_SVC_RECORD_VERSION || c.len - c.off < 32) return -1;
    memcpy(s->instance, c.p + c.off, 32); c.off += 32;
    if (lmb_cur_u32(&c, &s->state) || lmb_cur_u32(&c, &s->phase) ||
        lmb_cur_u32(&c, &s->retained) || lmb_cur_u32(&c, &s->threads) ||
        lmb_cur_u64(&c, &s->pid) || lmb_cur_u64(&c, &s->segment_pid) ||
        lmb_cur_u64(&c, &s->host_pid) || lmb_cur_u64(&c, &s->ram) || lmb_cur_u64(&c, &s->revision) ||
        lmb_inventory_string(&c, s->role, sizeof s->role) ||
        lmb_inventory_string(&c, s->name, sizeof s->name) ||
        lmb_inventory_string(&c, s->tracker, sizeof s->tracker) ||
        lmb_inventory_string(&c, s->detail, sizeof s->detail) ||
        lmb_cur_u32(&c, &has) || has > 1 || lmb_cur_u32(&c, &size) || size > c.len - c.off) return -1;
    s->has_offer = (int)has;
    LmbCur offer = {c.p + c.off, size, 0};
    if ((has && lmb_home_offer_unpack(&offer, &s->offer)) || (!has && size)) return -1;
    c.off += size;
    if (v >= 2 && (lmb_cur_u32(&c, &s->model_count) || lmb_cur_u64(&c, &s->reserved_total))) return -1;
    if (v >= 3 && (lmb_cur_u32(&c, &s->compute_enabled) || lmb_cur_u32(&c, &s->compute_active) ||
        lmb_cur_u32(&c, &s->compute_queued) || lmb_cur_u64(&c, &s->compute_grants))) return -1;
    if (v >= 4) {
        if (c.off > c.len || c.len-c.off < 32) return -1;
        memcpy(s->allocation_set, c.p+c.off, 32); c.off += 32;
    }
    if (v >= 5) {
        if (lmb_cur_u32(&c,&s->preparation_results_known) || lmb_cur_u32(&c,&s->preparation_result_count) ||
            s->preparation_result_count>HOME_PREPARATION_RESULTS) return -1;
        for (uint32_t i=0;i<s->preparation_result_count;i++) {
            HomePreparationResult *r=&s->preparation_results[i];
            if (lmb_cur_bytes(&c,r->allocation,32) ||
                lmb_inventory_string(&c,r->catalogue_name,sizeof r->catalogue_name) ||
                lmb_cur_u32(&c,&r->ready_observed)) return -1;
        }
    }
    if (c.off != c.len) return -1;
    LmbBuf check = {0}; int rc = home_service_pack(&check, s); free(check.p); return rc;
}

/* One request format on Unix sockets and native Windows named pipes. */
static int home_service_request_pack(uint8_t request[80], uint32_t op,
        const HomeServiceSnapshot *expected) {
    if (op > HOME_SVC_REVOKE_REPEAT || (op && !expected)) return -1;
    memset(request, 0, 80);
    lmb_put32(request, HOME_SVC_VERSION); lmb_put32(request + 4, op);
    if (expected) {
        memcpy(request + 8, expected->instance, 32);
        memcpy(request + 40, expected->offer.id, 32);
        lmb_put32(request + 72, (uint32_t)expected->revision);
        lmb_put32(request + 76, (uint32_t)(expected->revision >> 32));
    }
    return 0;
}
static int home_service_request_operation(const uint8_t request[80],
        const HomeServiceSnapshot *current) {
    if (lmb_get32(request) != HOME_SVC_VERSION ||
        lmb_get32(request + 4) > HOME_SVC_REVOKE_REPEAT) return -1;
    uint32_t op = lmb_get32(request + 4);
    uint64_t revision = (uint64_t)lmb_get32(request + 72) |
                       ((uint64_t)lmb_get32(request + 76) << 32);
    if (op && (memcmp(request + 8, current->instance, 32) ||
               memcmp(request + 40, current->offer.id, 32) || revision != current->revision)) return -1;
    return (int)op;
}
#endif
