/* Routing roots include an allocation's name. Bounded repeat permissions
 * instead use the canonical checkpoint identity, reconstructed from signed
 * chunk hashes. No weight bytes, paths on disk or new wire opcode are needed. */
#ifndef LMB_CONTENT_IDENTITY_H
#define LMB_CONTENT_IDENTITY_H
#include "lumabri_home.h"
#include "src/runtime/lumabri_signed_checkpoint.h"
static int home_offer_content(const LmbHomeOffer *o,uint8_t content[32]) {
    memset(content,0,32);
    if (!lmb_home_offer_valid(o)) return -1;
    LmbSignedCheckpoint manifest;
    if (lmb_signed_checkpoint_fetch(o->tracker,o->model,o->requester,o->model_root,
                                   o->model_bytes,2000,&manifest)) return -1;
    memcpy(content,manifest.content,32);lmb_signed_checkpoint_free(&manifest);return 0;
}
#endif
