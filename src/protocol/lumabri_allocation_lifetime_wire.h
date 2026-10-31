/* Allocation identity and exit evidence, shared by Unix and Windows. */
#ifndef LMB_ALLOCATION_LIFETIME_WIRE_H
#define LMB_ALLOCATION_LIFETIME_WIRE_H
#include "lumabri_proto.h"
#include "lumabri_sign.h"
#define LMB_LIFETIME_BYTES 188u
#define LMB_LIFETIME_MAX 4096u
typedef struct {
    uint8_t allocation[32], root[32], requester[32], offer_hash[64];
    uint64_t reserved_bytes;
    uint32_t edge, begin, end;
} LmbAllocationLifetime;

static int lmb_lifetime_valid(const LmbAllocationLifetime *r) {
    uint8_t id=0,root=0,owner=0,hash=0;
    for (unsigned i=0;i<32;i++) { id|=r->allocation[i]; root|=r->root[i]; owner|=r->requester[i]; }
    for (unsigned i=0;i<64;i++) hash|=r->offer_hash[i];
    return id && root && owner && hash && r->reserved_bytes && r->reserved_bytes<=(UINT64_C(1)<<50) &&
        r->edge<=1 && r->begin<r->end;
}
static int lmb_lifetime_encode(const LmbAllocationLifetime *r,uint8_t b[LMB_LIFETIME_BYTES]) {
    if (!lmb_lifetime_valid(r)) return -1;
    memcpy(b,"LMBLIF1\0",8); memcpy(b+8,r->allocation,32); memcpy(b+40,r->root,32);
    memcpy(b+72,r->requester,32); memcpy(b+104,r->offer_hash,64);
    lmb_put32(b+168,(uint32_t)(r->reserved_bytes>>32)); lmb_put32(b+172,(uint32_t)r->reserved_bytes);
    lmb_put32(b+176,r->edge); lmb_put32(b+180,r->begin); lmb_put32(b+184,r->end); return 0;
}
static int lmb_lifetime_decode(const uint8_t *b,size_t size,LmbAllocationLifetime *r) {
    memset(r,0,sizeof *r);
    if (size!=LMB_LIFETIME_BYTES || memcmp(b,"LMBLIF1\0",8)) return -1;
    memcpy(r->allocation,b+8,32); memcpy(r->root,b+40,32); memcpy(r->requester,b+72,32);
    memcpy(r->offer_hash,b+104,64); r->reserved_bytes=((uint64_t)lmb_get32(b+168)<<32)|lmb_get32(b+172);
    r->edge=lmb_get32(b+176); r->begin=lmb_get32(b+180); r->end=lmb_get32(b+184);
    return lmb_lifetime_valid(r) ? 0 : -1;
}
static void lmb_lifetime_name(const uint8_t id[32],char name[80]) {
    char hex[65]; lmb_hex(hex,id,32); snprintf(name,80,"%s.lifetime",hex);
}
#endif
