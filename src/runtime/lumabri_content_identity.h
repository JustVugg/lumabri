/* Routing roots include an allocation's name. Bounded repeat permissions
 * instead use the canonical checkpoint identity, reconstructed from signed
 * chunk hashes. No weight bytes, paths on disk or new wire opcode are needed. */
#ifndef LMB_CONTENT_IDENTITY_H
#define LMB_CONTENT_IDENTITY_H
#include "lumabri_home.h"
#include "lumabri_sha.h"
static int home_offer_content(const LmbHomeOffer *o,uint8_t content[32]) {
    memset(content,0,32);
    if (!lmb_home_offer_valid(o)) return -1;
    int fd=lmb_connect_ms_io(o->tracker,1000,1000),bad=1; double deadline=nowd()+2;
    LmbBuf request={0}; LmbMsg placement={0}; LmbModelItem *items=NULL;
    uint32_t count=0; size_t hashes=0; uint64_t total=0;
    if (fd<0 || lmb_auth(fd) || lmb_buf_str(&request,o->model) ||
        lmb_send(fd,LMB_PLACEMENT,request.p,(uint32_t)request.len,NULL,0) || lmb_recv(fd,&placement) ||
        placement.op!=LMB_PLACEMENT_R || placement.pay_len) goto done;
    LmbCur c={placement.body,placement.body_len,0};
    if (lmb_cur_u32(&c,&count) || !count || count>4096) { count=0; goto done; }
    items=calloc(count,sizeof *items); if (!items) goto done;
    for (uint32_t i=0;i<count;i++) {
        char path[LMB_PATH_MAX],peer[256]; uint16_t peers;
        if (lmb_cur_str(&c,path,sizeof path) || !lmb_rel_ok(path) ||
            lmb_cur_u64(&c,&items[i].size) || items[i].size>o->model_bytes ||
            total>o->model_bytes-items[i].size || lmb_cur_u16(&c,&peers)) goto done;
        total+=items[i].size;
        for (uint32_t j=0;j<i;j++) if (!strcmp(items[j].path,path)) goto done;
        items[i].path=strdup(path); if (!items[i].path) goto done;
        for (uint16_t j=0;j<peers;j++) if (lmb_cur_str(&c,peer,sizeof peer)) goto done;
    }
    if (c.off!=c.len || total!=o->model_bytes) goto done;
    for (uint32_t i=0;i<count;i++) {
        int remaining=(int)((deadline-nowd())*1000); if (remaining<=0) goto done;
        lmb_set_io_timeout(fd,remaining);
        LmbMsg reply={0}; request.len=0;
        int fail=lmb_buf_str(&request,o->model) || lmb_buf_str(&request,items[i].path) ||
            lmb_send(fd,LMB_HASHES,request.p,(uint32_t)request.len,NULL,0) || lmb_recv(fd,&reply) || reply.op!=LMB_HASHES_R;
        LmbCur truth={reply.body,reply.body_len,0}; char model[64]; uint32_t chunk,nh,signed_; uint64_t size; uint8_t sig[64];
        if (!fail) fail=lmb_cur_str(&truth,model,sizeof model) || strcmp(model,o->model) ||
            lmb_cur_u32(&truth,&chunk) || chunk!=LMB_HASH_CHUNK || lmb_cur_u32(&truth,&nh) ||
            lmb_cur_u64(&truth,&size) || size!=items[i].size ||
            nh!=(size+LMB_HASH_CHUNK-1)/LMB_HASH_CHUNK || nh>(64u<<20)/32 ||
            lmb_cur_u32(&truth,&signed_) || signed_!=1 || lmb_cur_bytes(&truth,sig,64) || truth.off!=truth.len ||
            reply.pay_len!=(uint64_t)nh*32 || reply.pay_len>(64u<<20)-hashes;
        if (!fail) {
            size_t length=0; uint8_t *message=lmb_truth_msg(model,items[i].path,chunk,size,reply.pay,nh,&length);
            fail=!message || lmb_sign_verify(sig,message,length,o->requester); free(message);
        }
        if (!fail) { items[i].nh=nh; hashes+=reply.pay_len; items[i].hashes=lmb_msg_take_pay(&reply); }
        lmb_msg_free(&reply); if (fail) goto done;
    }
    uint8_t routing[32];
    bad=lmb_model_root(o->model,items,count,routing) || memcmp(routing,o->model_root,32) ||
        lmb_model_root("lumabri-calibration-content-v1",items,count,content);
done:
    if (items) for (uint32_t i=0;i<count;i++) { free((void *)items[i].path); free((void *)items[i].hashes); }
    free(items); free(request.p); lmb_msg_free(&placement); if (fd>=0) lmb_close(fd);
    if (bad) memset(content,0,32);
    return bad ? -1 : 0;
}
#endif
