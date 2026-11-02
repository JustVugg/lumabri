/* Bounded text turn over the shared Hosted and Serve2 codecs. */
#ifndef LUMABRI_HOST_TURN_H
#define LUMABRI_HOST_TURN_H
#include "src/runtime/lumabri_host_client.h"
#include "src/runtime/lumabri_reply_stream.h"
#define LMB_CLIENT_TEXT_LIMIT (1u << 20)

static int lmb_host_turn_payload(Cap *out,const LmbHostGreeting *g,const char *history,
        const char *prompt) {
    if (!history || !prompt || strlen(history)>LMB_CLIENT_TEXT_LIMIT || strlen(prompt)>65536 ||
        serve2_prefix(out,g->kind) || cap_str(out,history) ||
        serve2_turn(out,g->kind,!*history,prompt,NULL) || out->len>LMB_CLIENT_TEXT_LIMIT ||
        (g->max_frame && out->len>g->max_frame)) return -1;
    return 0;
}
static int lmb_host_turn_send(LmbSocket fd,const LmbHostGreeting *g,const char *id,
        const Cap *payload,unsigned max_new) {
    if (g->codec!=2 || !lmb_reply_id_valid(id,strlen(id)) || !max_new ||
        max_new>1048576 || (g->max_new && max_new>g->max_new) ||
        payload->len>LMB_CLIENT_TEXT_LIMIT || (g->max_frame && payload->len>g->max_frame)) return -1;
    char header[160];int n=snprintf(header,sizeof header,"SUBMIT %s 0 %zu %u %.1f 0.95\n",
        id,payload->len,max_new,(g->sampling&1) ? 0.0 : 0.7);
    if (n<0 || (size_t)n>=sizeof header || lmb_send(fd,LMB_HOST_STREAM,NULL,0,header,(uint32_t)n)) return -1;
    for (size_t off=0;off<payload->len;) {
        size_t take=payload->len-off;if(take>16384) take=16384;
        if (lmb_send(fd,LMB_HOST_STREAM,NULL,0,payload->p+off,(uint32_t)take)) return -1;
        off+=take;
    }
    return lmb_send(fd,LMB_HOST_STREAM,NULL,0,"\n",1);
}
static int lmb_host_turn_read(LmbSocket fd,LmbReplyStream *stream,uint32_t timeout_ms) {
    LmbMsg m={0};size_t consumed=0;
    int bad=lmb_recv_bounded(fd,&m,1u<<20,timeout_ms) || m.op!=LMB_HOST_STREAM ||
        m.body_len || !m.pay_len;
    if (!bad) {
        lmb_reply_feed(stream,m.pay,m.pay_len,&consumed);
        bad=consumed!=m.pay_len || stream->status<0;
    }
    lmb_msg_free(&m);
    return bad ? -1 : 0;
}
#endif
