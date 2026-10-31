/* Authenticated checkpoint inventory shared by admission and native weight
 * preparation. Endpoint discovery never replaces the approved signing key. */
#ifndef LMB_SIGNED_CHECKPOINT_H
#define LMB_SIGNED_CHECKPOINT_H
#include "lumabri_proto.h"
#include "lumabri_secure.h"
#include "lumabri_sha.h"
#include <limits.h>

#define LMB_CHECKPOINT_FILES 4096u
#define LMB_CHECKPOINT_PEERS 8u
#define LMB_CHECKPOINT_HASH_BYTES (64u<<20)
typedef struct { uint16_t count; char address[LMB_CHECKPOINT_PEERS][256]; } LmbCheckpointSources;
typedef struct {
    LmbModelItem *items;
    LmbCheckpointSources *sources;
    uint32_t count;
    uint8_t content[32];
} LmbSignedCheckpoint;

static void lmb_signed_checkpoint_free(LmbSignedCheckpoint *s) {
    if (!s) return;
    if (s->items) for (uint32_t i=0;i<s->count;i++) {
        free((void *)s->items[i].path);free((void *)s->items[i].hashes);
    }
    free(s->items);free(s->sources);memset(s,0,sizeof *s);
}

static int lmb_signed_checkpoint_fetch(const char *tracker,const char *model,
    const uint8_t signer[32],const uint8_t expected[32],uint64_t bytes,
    uint32_t timeout_ms,LmbSignedCheckpoint *out) {
    if (!out) return -1;
    memset(out,0,sizeof *out);
    if (!tracker || !*tracker || !model || !*model || strlen(model)>=64 ||
        !signer || !expected || !bytes || bytes>(UINT64_C(1)<<50) ||
        !timeout_ms || timeout_ms>INT_MAX || lmb_secure_init() || !lmb_secure_enabled()) return -1;
    uint64_t deadline=lmb_io_monotonic_ms()+timeout_ms;
    uint64_t previous_deadline=lmb_read_deadline_ms;
    if (!previous_deadline || deadline<previous_deadline) lmb_read_deadline_ms=deadline;
    LmbSocket fd=lmb_connect_ms_io(tracker,timeout_ms<1000 ? (int)timeout_ms : 1000,(int)timeout_ms);
    if (fd!=LMB_INVALID_SOCKET) lmb_set_io_timeout(fd,(int)timeout_ms);
    LmbBuf request={0};LmbMsg placement={0};int bad=1;
    uint32_t count=0;uint64_t total=0;size_t hashes=0;
    if (fd==LMB_INVALID_SOCKET || lmb_auth(fd) || lmb_buf_str(&request,model) ||
        lmb_send(fd,LMB_PLACEMENT,request.p,(uint32_t)request.len,NULL,0) ||
        lmb_recv_bounded(fd,&placement,8u<<20,timeout_ms) ||
        placement.op!=LMB_PLACEMENT_R || placement.pay_len) goto done;
    LmbCur cursor={placement.body,placement.body_len,0};
    if (lmb_cur_u32(&cursor,&count) || !count || count>LMB_CHECKPOINT_FILES) goto done;
    out->items=calloc(count,sizeof *out->items);out->sources=calloc(count,sizeof *out->sources);
    if (!out->items || !out->sources) goto done;
    out->count=count;
    for (uint32_t i=0;i<count;i++) {
        LmbModelItem *item=&out->items[i];char path[LMB_PATH_MAX],peer[256];uint16_t peers;
        if (lmb_cur_str(&cursor,path,sizeof path) || !lmb_rel_ok(path) ||
            lmb_cur_u64(&cursor,&item->size) || item->size>bytes || total>bytes-item->size ||
            lmb_cur_u16(&cursor,&peers)) goto done;
        total+=item->size;
        for (uint32_t j=0;j<i;j++) if (!strcmp(out->items[j].path,path)) goto done;
        item->path=strdup(path);if (!item->path) goto done;
        for (uint16_t j=0;j<peers;j++) {
            if (lmb_cur_str(&cursor,peer,sizeof peer)) goto done;
            if (j<LMB_CHECKPOINT_PEERS) {
                strcpy(out->sources[i].address[j],peer);out->sources[i].count++;
            }
        }
    }
    if (cursor.off!=cursor.len || total!=bytes) goto done;
    for (uint32_t i=0;i<count;i++) {
        uint64_t now=lmb_io_monotonic_ms();if (now>=deadline) goto done;
        lmb_set_io_timeout(fd,(int)(deadline-now));
        LmbModelItem *item=&out->items[i];LmbMsg reply={0};request.len=0;
        int fail=lmb_buf_str(&request,model) || lmb_buf_str(&request,item->path) ||
            lmb_send(fd,LMB_HASHES,request.p,(uint32_t)request.len,NULL,0) ||
            lmb_recv_bounded(fd,&reply,LMB_CHECKPOINT_HASH_BYTES+4096,(uint32_t)(deadline-now)) ||
            reply.op!=LMB_HASHES_R;
        LmbCur truth={reply.body,reply.body_len,0};char declared[64];uint32_t chunk,nh,signed_;uint64_t size;uint8_t sig[64];
        if (!fail) fail=lmb_cur_str(&truth,declared,sizeof declared) || strcmp(declared,model) ||
            lmb_cur_u32(&truth,&chunk) || chunk!=LMB_HASH_CHUNK || lmb_cur_u32(&truth,&nh) ||
            lmb_cur_u64(&truth,&size) || size!=item->size ||
            nh!=size/LMB_HASH_CHUNK+(size%LMB_HASH_CHUNK!=0) || nh>LMB_CHECKPOINT_HASH_BYTES/32 ||
            lmb_cur_u32(&truth,&signed_) || signed_!=1 || lmb_cur_bytes(&truth,sig,64) || truth.off!=truth.len ||
            reply.pay_len!=(uint64_t)nh*32 || reply.pay_len>LMB_CHECKPOINT_HASH_BYTES-hashes;
        if (!fail) {
            size_t length=0;uint8_t *message=lmb_truth_msg(model,item->path,chunk,size,reply.pay,nh,&length);
            fail=!message || lmb_sign_verify(sig,message,length,signer);free(message);
        }
        if (!fail) {item->nh=nh;hashes+=reply.pay_len;item->hashes=lmb_msg_take_pay(&reply);}
        lmb_msg_free(&reply);if (fail) goto done;
    }
    uint8_t root[32];
    bad=lmb_model_root(model,out->items,count,root) || memcmp(root,expected,32) ||
        lmb_model_root("lumabri-calibration-content-v1",out->items,count,out->content);
done:
    free(request.p);lmb_msg_free(&placement);if (fd!=LMB_INVALID_SOCKET) lmb_close(fd);
    lmb_read_deadline_ms=previous_deadline;
    if (bad) lmb_signed_checkpoint_free(out);
    return bad ? -1 : 0;
}
#endif
