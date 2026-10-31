/* Test-only signed lease issuer and local Colibri oracle. The worker and its
 * encrypted data plane are the production executable, in a separate process.
 * Full tracker interoperability is also exercised by the PC/Linux lab gate. */
#include "lumabri_sign.h"
#include "lumabri_inventory.h"
#include "lumabri_secure.h"
#include "lumabri_segment_discovery.h"
#include "edge_runtime.h"
#include "edge_adapters.h"
#include "segment_runtime.h"
#include "segment_adapters.h"
#include <math.h>
#include "checkpoint_fixture_windows.h"

typedef struct {
    LmbSocket listener;
    HANDLE registered;
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_int stop;
    LmbSegAdvert advert,adverts[4];
    unsigned nadverts,machine_reports;
    LmbMachineReport machine_report;
    LmbSegOwner owner;
    CheckpointFixture checkpoint;
} WorkerFixture;

static void *fixture_tracker(void *opaque) {
    WorkerFixture *f=opaque;
    LmbSocket clients[8]; uint8_t nonce[8][32]; int authed[8]={0},challenged[8]={0};
    for (unsigned i=0;i<8;i++) clients[i]=LMB_INVALID_SOCKET;
    while (!atomic_load(&f->stop)) {
        if (lmb_socket_wait(f->listener,0,10)==1) {
            LmbSocket fd=lmb_socket_accept(f->listener,NULL,NULL); assert(fd!=LMB_INVALID_SOCKET);
            assert(!lmb_socket_timeout(fd,2000) && !lmb_secure_server(fd));
            unsigned i=0; while (i<8 && clients[i]!=LMB_INVALID_SOCKET) i++;
            assert(i<8); clients[i]=fd;authed[i]=challenged[i]=0;
        }
        for (unsigned i=0;i<8;i++) {
            LmbSocket fd=clients[i];
            if (fd==LMB_INVALID_SOCKET || lmb_socket_wait(fd,0,0)!=1) continue;
            LmbMsg m={0};
            if (lmb_recv(fd,&m)) {lmb_close(fd);clients[i]=LMB_INVALID_SOCKET;continue;}
            assert(!m.pay_len);
            if (m.op==LMB_AUTH) {
                char token[64];LmbCur c={m.body,m.body_len,0};
                assert(!lmb_cur_str(&c,token,sizeof token) && c.off==c.len);
                assert(!strcmp(token,"native-worker-test"));authed[i]=1;
                assert(!lmb_send(fd,LMB_OK,NULL,0,NULL,0));
            } else {
                assert(authed[i]);
                if (f->checkpoint.count && checkpoint_fixture_reply(&f->checkpoint,fd,&m)) {
                    /* Manifest and bytes come from this test-only source. */
                } else if (m.op==LMB_CHALLENGE) {
                    lmb_random(nonce[i],32);challenged[i]=1;
                    assert(!lmb_send(fd,LMB_CHALLENGE_R,nonce[i],32,NULL,0));
                } else if (m.op==LMB_MACHINE_REPORT) {
                    assert(challenged[i] && m.body_len>64);
                    LmbCur c={m.body,m.body_len-64,0};LmbMachineReport report;
                    assert(!lmb_inventory_unpack(&c,&report) && c.off==c.len && lmb_secure_peer_matches(fd,report.identity));
                    LmbBuf transcript={0};assert(!lmb_buf_str(&transcript,"lumabri.machine.v1") &&
                        !lmb_buf_bytes(&transcript,nonce[i],32) && !lmb_buf_bytes(&transcript,m.body,c.len));
                    assert(!lmb_sign_verify(m.body+c.len,transcript.p,transcript.len,report.identity));free(transcript.p);
                    pthread_mutex_lock(&f->lock);f->machine_report=report;f->machine_reports++;pthread_mutex_unlock(&f->lock);
                    assert(!lmb_send(fd,LMB_OK,NULL,0,NULL,0));
                } else if (m.op==LMB_SEG_REGISTER) {
                    assert(challenged[i] && m.body_len>100);
                    size_t n=m.body_len-100;LmbSegAdvert advert;
                    assert(lmb_get32(m.body+n)==LMB_PEER_AUTH_MAGIC);
                    assert(!lmb_seg_advert_decode(m.body,n,&advert));
                    uint8_t transcript[512];
                    size_t len=lmb_peer_auth_msg(nonce[i],advert.peer_name,advert.model,
                                                advert.addr,transcript,sizeof transcript);
                    assert(len && !lmb_sign_verify(m.body+n+36,transcript,len,m.body+n+4));
                    assert(lmb_secure_peer_matches(fd,m.body+n+4));
                    pthread_mutex_lock(&f->lock);f->advert=advert;
                    unsigned slot=0;while (slot<f->nadverts && strcmp(f->adverts[slot].peer_name,advert.peer_name)) slot++;
                    assert(slot<4);f->adverts[slot]=advert;if (slot==f->nadverts) f->nadverts++;
                    pthread_mutex_unlock(&f->lock);
                    uint8_t *body=NULL;uint32_t size=0;
                    assert(!lmb_seg_registration_reply_encode(&f->owner,&body,&size));
                    assert(!lmb_send(fd,LMB_SEG_REGISTER_R,body,size,NULL,0));free(body);
                    SetEvent(f->registered);
                } else {
                    /* Optional telemetry probe is deliberately absent here. */
                    assert(m.op==LMB_SEG_COMPUTE_GET);
                    assert(!lmb_send(fd,LMB_ERR,NULL,0,NULL,0));
                }
            }
            lmb_msg_free(&m);
        }
    }
    for (unsigned i=0;i<8;i++) if (clients[i]!=LMB_INVALID_SOCKET) lmb_close(clients[i]);
    return NULL;
}

static void fixture_start(WorkerFixture *f,char *address,size_t cap,const char *checkpoint) {
    memset(f,0,sizeof *f);atomic_init(&f->stop,0);pthread_mutex_init(&f->lock,NULL);
    f->registered=CreateEventW(NULL,TRUE,FALSE,NULL);assert(f->registered);
    f->owner.lease_id.bytes[0]=7;f->owner.fencing_epoch=3;f->owner.route_generation=5;
    f->listener=lmb_listen(0);assert(f->listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in bound;LmbSocketLength size=sizeof bound;
    assert(!getsockname(f->listener,(struct sockaddr *)&bound,&size));
    snprintf(address,cap,"127.0.0.1:%u",ntohs(bound.sin_port));
    if (checkpoint) {
        checkpoint_fixture_init(&f->checkpoint,"native-worker-olmoe");
        strcpy(f->checkpoint.address,address);checkpoint_fixture_load(&f->checkpoint,checkpoint);
    }
    assert(!pthread_create(&f->thread,NULL,fixture_tracker,f));
}

static LmbSegReply fixture_exchange(LmbSocket fd,uint32_t op,const void *body,uint32_t size,
                                    const void *input,uint32_t bytes,void *output) {
    assert(!lmb_send(fd,op,body,size,input,bytes));
    LmbMsg m={0};LmbSegReply reply;
    assert(!lmb_recv(fd,&m) && m.op==op+1 && !lmb_seg_reply_decode(m.body,m.body_len,&reply));
    if (reply.status==LMB_SEG_STATUS_OK && bytes) {
        assert(output && m.pay_len==bytes);memcpy(output,m.pay,bytes);
    } else assert(!m.pay_len);
    lmb_msg_free(&m);return reply;
}

static void fixture_chat_with_advert(WorkerFixture *f,const char *model,int reject_open,const LmbSegAdvert *selected) {
    assert(WaitForSingleObject(f->registered,10000)==WAIT_OBJECT_0);
    LmbSegAdvert a;pthread_mutex_lock(&f->lock);a=selected ? *selected : f->advert;pthread_mutex_unlock(&f->lock);
    LmbSegOpen open={.owner=f->owner,.layer_begin=a.layer_begin,.layer_end=a.layer_end,
        .context_tokens=64,.max_rows=1,.state_dtype=a.state_dtype,.state_width=a.state_width,
        .ttl_ms=60000,.capabilities=a.capabilities};
    lmb_random(open.session_id.bytes,16);lmb_random(open.request_id.bytes,16);
    memcpy(open.model_root,a.model_root,32);memcpy(open.tokenizer_root,a.tokenizer_root,32);
    strcpy(open.engine_id,a.engine_id);strcpy(open.state_schema,a.state_schema);strcpy(open.numeric_class,a.numeric_class);
    LmbSocket fd=lmb_connect_ms_io(a.addr,2000,4000);assert(fd!=LMB_INVALID_SOCKET);
    uint8_t *body=NULL;uint32_t size=0;
    assert(!lmb_seg_open_encode(&open,&body,&size));
    LmbSegReply reply={0};
    for (unsigned attempt=0;attempt<100;attempt++) {
        reply=fixture_exchange(fd,LMB_SEG_OPEN,body,size,NULL,0,NULL);
        if (reply.status!=LMB_SEG_STATUS_STALE_OWNER) break;
        Sleep(10); /* READY and the first lease reply can cross. */
    }
    free(body);
    if (reject_open) {assert(reply.status==LMB_SEG_STATUS_QUOTA);lmb_close(fd);return;}
    assert(reply.status==LMB_SEG_STATUS_OK);
    char error[256];ColiEdgeEngine *edge=NULL;ColiSegmentEngine *reference=NULL;
    ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=model,.backend_mask=COLI_EDGE_CAP_CPU};
    ColiSegmentEngineOptions so={.struct_size=sizeof so,.model_dir=model,.layer_begin=a.layer_begin,
        .layer_end=a.layer_end,.context_tokens=64,.backend_mask=COLI_SEGMENT_CAP_CPU};
    assert(!coli_edge_engine_open("olmoe",&eo,&edge,error,sizeof error));
    assert(!coli_segment_engine_open("olmoe",&so,&reference,error,sizeof error));
    ColiSegmentSession *session=NULL;ColiSegmentSessionOptions options={.struct_size=sizeof options,.context_tokens=64};
    assert(!coli_segment_session_create(reference,&options,&session,error,sizeof error));
    assert(a.state_dtype==LMB_SEG_DTYPE_F32);
    size_t bytes=a.state_width*sizeof(float);
    float *input=malloc(bytes),*output=malloc(bytes),*expected=malloc(bytes);assert(input && output && expected);
    int32_t token=1;
    for (unsigned position=0;position<24;position++) {
        ColiEdgeEmbedRequest embed={.struct_size=sizeof embed,.rows=1,.token_ids=&token,.token_count=1,
            .output=input,.output_bytes=bytes};
        assert(!coli_edge_embed(edge,&embed,error,sizeof error));
        ColiSegmentRunRequest local={.struct_size=sizeof local,.rows=1,.position=position,.token_ids=&token,
            .token_count=1,.input=input,.input_bytes=bytes,.output=expected,.output_bytes=bytes};
        assert(!coli_segment_run(session,&local,error,sizeof error));
        LmbSegRun run={.session_id=open.session_id,.owner=open.owner,.sequence=reply.next_sequence,
            .position=position,.rows=1,.token_count=1,.token_ids=&token};
        lmb_random(run.request_id.bytes,16);
        assert(!lmb_seg_run_encode(&run,&body,&size));
        reply=fixture_exchange(fd,LMB_SEG_RUN,body,size,input,(uint32_t)bytes,output);free(body);
        assert(reply.status==LMB_SEG_STATUS_OK && reply.next_position==position+1);
        for (unsigned i=0;i<a.state_width;i++)
            assert(isfinite(output[i]) && fabsf(output[i]-expected[i])<=2e-5f*fmaxf(1,fabsf(expected[i])));
        int32_t selected=-1,oracle=-1;
        ColiEdgeSelectRequest pick={.struct_size=sizeof pick,.rows=1,.input=output,.input_bytes=bytes,
            .token_ids=&selected,.token_capacity=1};
        assert(!coli_edge_select(edge,&pick,error,sizeof error));
        pick.input=expected;pick.token_ids=&oracle;
        assert(!coli_edge_select(edge,&pick,error,sizeof error) && selected==oracle);token=selected;
    }
    LmbSegControl close={.session_id=open.session_id,.owner=open.owner,.sequence=reply.next_sequence};
    lmb_random(close.request_id.bytes,16);assert(!lmb_seg_control_encode(&close,&body,&size));
    reply=fixture_exchange(fd,LMB_SEG_CLOSE,body,size,NULL,0,NULL);free(body);
    assert(reply.status==LMB_SEG_STATUS_OK);lmb_close(fd);
    free(input);free(output);free(expected);coli_segment_session_destroy(session);
    assert(!coli_segment_engine_close(reference,error,sizeof error));coli_edge_engine_close(edge);
}

static void fixture_chat(WorkerFixture *f,const char *model,int reject_open) {
    fixture_chat_with_advert(f,model,reject_open,NULL);
}
