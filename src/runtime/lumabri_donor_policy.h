/* Explicit owner authorization for FUTURE offers, not a replayed approval.
 * This scope describes one resident Segment placement on this donor.
 * limit.model_root and offers passed here carry the VERIFIED canonical
 * content identity, never a peer assertion or the allocation routing root. */
#ifndef LMB_DONOR_POLICY_H
#define LMB_DONOR_POLICY_H
#include "lumabri_home.h"
#include "lumabri_sha.h"
#define LMB_DONOR_POLICY_MAX 64u
#define LMB_DONOR_POLICY_BYTES 8192u
typedef struct {
    uint8_t id[32], donor_instance[32];
    uint32_t revision, enabled, allocations;
    uint64_t issued, expires;
    LmbHomeOffer limit;
} LmbDonorPolicy;
static inline void lmb_donor_policy_digest(const void *p,size_t n,uint8_t out[32]) {
    LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,p,n); lmb_sha_final(&sha,out);
}
static inline int lmb_donor_policy_scope(const LmbHomeOffer *o,uint8_t out[32]) {
    LmbBuf b={0};
    int bad=!lmb_home_offer_valid(o) || o->hybrid_role ||
        lmb_buf_bytes(&b,o->requester,32) || lmb_buf_bytes(&b,o->model_root,32) ||
        lmb_buf_bytes(&b,o->edge_peer,32) || lmb_buf_str(&b,o->tracker) || lmb_buf_str(&b,o->model_type) ||
        lmb_buf_u32(&b,o->layers) || lmb_buf_u32(&b,o->begin) || lmb_buf_u32(&b,o->end) || lmb_buf_u32(&b,o->runs_edge);
    if (!bad) lmb_donor_policy_digest(b.p,b.len,out);
    free(b.p); return bad ? -1 : 0;
}
static inline int lmb_donor_policy_valid(const LmbDonorPolicy *p) {
    uint8_t scope[32];
    return p->revision && p->enabled<=1 && p->allocations && p->allocations<=4 && p->issued &&
        lmb_home_nonzero(p->donor_instance,32) &&
        p->expires>p->issued && p->expires-p->issued<=604800 &&
        !lmb_donor_policy_scope(&p->limit,scope) && !memcmp(scope,p->id,32);
}
static inline int lmb_donor_policy_allows(const LmbDonorPolicy *p,const LmbHomeOffer *o,
    const uint8_t donor_instance[32],uint64_t now,uint32_t allocated,uint64_t used_ram) {
    uint8_t scope[32];
    if (!lmb_donor_policy_valid(p) || !p->enabled || memcmp(p->donor_instance,donor_instance,32) || now<p->issued || now>=p->expires ||
        lmb_donor_policy_scope(o,scope) || memcmp(scope,p->id,32) || allocated>=p->allocations) return 0;
    const LmbHomeOffer *limit=&p->limit;
    return o->ram_bytes<=limit->ram_bytes && o->edge_ram_bytes<=limit->edge_ram_bytes &&
        o->disk_bytes<=limit->disk_bytes && o->model_bytes<=limit->model_bytes &&
        o->context<=limit->context && o->threads<=limit->threads && o->max_new<=limit->max_new &&
        (o->sessions ? o->sessions : 1)<=(limit->sessions ? limit->sessions : 1) &&
        used_ram<=limit->ram_bytes*p->allocations && o->ram_bytes<=limit->ram_bytes*p->allocations-used_ram;
}
static inline int lmb_donor_policy_encode(const LmbDonorPolicy *p,LmbBuf *b) {
    if (!lmb_donor_policy_valid(p) || !b || b->len) return -1;
    int bad=lmb_buf_bytes(b,"LMBDPOL1",8) || lmb_buf_bytes(b,p->id,32) || lmb_buf_bytes(b,p->donor_instance,32) ||
        lmb_buf_u32(b,p->revision) || lmb_buf_u32(b,p->enabled) || lmb_buf_u32(b,p->allocations) ||
        lmb_buf_u64(b,p->issued) || lmb_buf_u64(b,p->expires) || lmb_home_offer_pack(b,&p->limit);
    uint8_t hash[32];
    if (!bad) { lmb_donor_policy_digest(b->p,b->len,hash); bad=lmb_buf_bytes(b,hash,32); }
    return bad || b->len>LMB_DONOR_POLICY_BYTES ? -1 : 0;
}
static inline int lmb_donor_policy_decode(const void *bytes,size_t n,LmbDonorPolicy *out) {
    memset(out,0,sizeof *out);
    if (!bytes || n<100 || n>LMB_DONOR_POLICY_BYTES || memcmp(bytes,"LMBDPOL1",8)) return -1;
    uint8_t hash[32]; lmb_donor_policy_digest(bytes,n-32,hash);
    if (memcmp(hash,(const uint8_t *)bytes+n-32,32)) return -1;
    LmbCur c={bytes,n-32,8}; LmbDonorPolicy p={0};
    if (lmb_cur_bytes(&c,p.id,32) || lmb_cur_bytes(&c,p.donor_instance,32) || lmb_cur_u32(&c,&p.revision) || lmb_cur_u32(&c,&p.enabled) ||
        lmb_cur_u32(&c,&p.allocations) || lmb_cur_u64(&c,&p.issued) || lmb_cur_u64(&c,&p.expires) ||
        lmb_home_offer_unpack(&c,&p.limit) || c.off!=c.len || !lmb_donor_policy_valid(&p)) return -1;
    *out=p; return 0;
}
#endif
