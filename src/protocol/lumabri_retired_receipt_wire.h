/* Terminal receipt format shared by both household keepers. */
#ifndef LMB_RETIRED_RECEIPT_WIRE_H
#define LMB_RETIRED_RECEIPT_WIRE_H
#include "lumabri_proto.h"
#include "src/runtime/lumabri_node_control.h"
#define LMB_RETIRED_RECEIPTS_MAX 256u
#define LMB_RETIRED_RECEIPT_BYTES (108u+LMB_NODE_CONTROL_REPLY_BYTES)
enum { LMB_RECEIPT_PREPARED=1, LMB_RECEIPT_DONE=2 };
typedef struct {
    uint8_t allocation[32], root[32], requester[32];
    uint32_t phase;
    LmbNodeControl seal;
} LmbRetiredReceipt;

static int lmb_retired_valid(const LmbRetiredReceipt *r) {
    uint8_t id=0,root=0,owner=0;
    for (unsigned i=0;i<32;i++) { id|=r->allocation[i]; root|=r->root[i]; owner|=r->requester[i]; }
    uint8_t bytes[LMB_NODE_CONTROL_REPLY_BYTES]; LmbNodeControl decoded;
    lmb_node_control_reply(bytes,0,&r->seal);
    return id && root && owner && (r->phase==LMB_RECEIPT_PREPARED || r->phase==LMB_RECEIPT_DONE) &&
        !lmb_node_control_decode(bytes,sizeof bytes,&decoded) &&
        r->seal.draining==LMB_NODE_CONTROL_RETIRED && !r->seal.sessions && !r->seal.experts;
}
static int lmb_retired_encode(const LmbRetiredReceipt *r, uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES]) {
    if (!lmb_retired_valid(r)) return -1;
    memcpy(bytes,"LMBREL1\0",8); memcpy(bytes+8,r->allocation,32);
    memcpy(bytes+40,r->root,32); memcpy(bytes+72,r->requester,32);
    lmb_put32(bytes+104,r->phase); lmb_node_control_reply(bytes+108,0,&r->seal); return 0;
}
static int lmb_retired_decode(const uint8_t *bytes, size_t size, LmbRetiredReceipt *r) {
    memset(r,0,sizeof *r);
    if (size!=LMB_RETIRED_RECEIPT_BYTES || memcmp(bytes,"LMBREL1\0",8)) return -1;
    memcpy(r->allocation,bytes+8,32); memcpy(r->root,bytes+40,32); memcpy(r->requester,bytes+72,32);
    r->phase=lmb_get32(bytes+104);
    return lmb_node_control_decode(bytes+108,LMB_NODE_CONTROL_REPLY_BYTES,&r->seal) ||
        !lmb_retired_valid(r) ? -1 : 0;
}
#endif
