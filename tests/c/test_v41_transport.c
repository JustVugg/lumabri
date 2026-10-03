/* Encrypted, separate-process boundary laboratory, not the product donor.
 * Exercises the existing RUN codec/session fencing with real V4.1 arithmetic.
 * No TUI approvals, large-checkpoint or physical LAN claims follow from this.
 */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
static int weights_sealed;
static unsigned forbidden_reads;
static ssize_t guarded_pread(int fd, void *p, size_t n, off_t off) {
    if (weights_sealed) {forbidden_reads++; errno=EIO; return -1;}
    return pread(fd,p,n,off);
}
#define pread guarded_pread
#define main upstream_cli_main
#include "v41_range_core.c"
#undef main
#undef pread
#include "engine_patches/v41_boundary.h"
#include "engine_patches/v41_lifecycle.h"
#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "lumabri_segment.h"

enum { CHATS=2, STEPS=48 };
typedef struct {
    Model model;
    LmbSegOpen open;
    int counts[V41_MAX_LAYERS];
    float *last;
    unsigned runs;
} WorkerSession;

static void warm(Model *m) {
    for (int l=m->range_enabled ? m->range_begin : 0;
         l<(m->range_enabled ? m->range_end : m->c.n_layers);l++)
        for(int e=0;e<m->c.n_routed;e++) (void)expert_slot_at(m,&m->cache[l],"layers",l,e);
}
static size_t width(const Cfg *c) {return (size_t)c->hc_mult*(c->dim+1)+lmb_v41_aux_width(c);}
static size_t aux_offset(const Cfg *c) {return (size_t)c->hc_mult*(c->dim+1);}
static void identity(int id, uint8_t pk[32],uint8_t sk[64]) {
    uint8_t seed[32]={0}; seed[0]=(uint8_t)id; lmb_sign_keypair(pk,sk,seed);
}
static LmbSegOpen scope(int chat,int begin,int end,uint32_t w) {
    LmbSegOpen o={0}; o.session_id.bytes[0]=(uint8_t)(chat+1);o.request_id.bytes[0]=1;
    o.owner.lease_id.bytes[0]=(uint8_t)(chat+20);o.owner.fencing_epoch=7;o.owner.route_generation=2;
    o.model_root[0]=11;o.tokenizer_root[0]=12;
    o.layer_begin=(uint32_t)begin;o.layer_end=(uint32_t)end;o.context_tokens=128;o.max_rows=1;
    o.state_dtype=LMB_SEG_DTYPE_F32;o.state_width=w;o.ttl_ms=60000;
    o.capabilities=LMB_SEG_CAP_TOKEN_IDS|LMB_SEG_CAP_RANGE_NATIVE|LMB_SEG_CAP_CPU|LMB_SEG_CAP_MULTI_SESSION;
    snprintf(o.engine_id,sizeof o.engine_id,"deepseek_v41");
    snprintf(o.state_schema,sizeof o.state_schema,"lmb-v41-delta-lab-v1");
    snprintf(o.numeric_class,sizeof o.numeric_class,"v41-f32-single-row-cpu");
    assert(lmb_seg_open_valid(&o));return o;
}
static int worker(int fd,const char *path,int begin,int end,int peer) {
    uint8_t pk[32],sk[64],expected[32],unused[64];identity(peer,pk,sk);identity(1,expected,unused);
    LmbSecure secure;int hs=lmb_secure_handshake(fd,0,sk,pk,&secure);
    if(hs) perror("V41 worker handshake");assert(!hs);
    assert(secure.have_peer_id && !memcmp(secure.peer_id,expected,32));
    Model engine={.range_enabled=1,.range_begin=begin,.range_end=end};Cfg c;cfg_load(&c,path);
    model_load(&engine,path,c.n_routed,32);warm(&engine);
    size_t w=width(&c),aux=aux_offset(&c),bytes=w*sizeof(float);
    WorkerSession sessions[CHATS]={0};LmbSegTable *table=lmb_seg_table_create(CHATS);assert(table);
    for(int i=0;i<CHATS;i++) {
        WorkerSession *s=&sessions[i];s->open=scope(i,begin,end,(uint32_t)w);
        assert(lmb_seg_table_open(table,&s->open,1)==LMB_SEG_STATUS_OK);
        assert(!lmb_v41_session_model(&s->model,&engine));
        s->last=malloc(bytes);assert(s->last);
    }
    weights_sealed=1;
    for(;;) {
        LmbMsg msg={0};assert(!lmb_secure_recv(&secure,fd,&msg));
        if(msg.op==LMB_SEG_CLOSE) {lmb_msg_free(&msg);break;}
        LmbSegRun run;int32_t token;
        assert(msg.op==LMB_SEG_RUN && !lmb_seg_run_decode(msg.body,msg.body_len,&run,&token,1));
        size_t chat=run.session_id.bytes[0]-1;assert(chat<CHATS);
        WorkerSession *s=&sessions[chat];
        LmbSegStatus status=lmb_seg_table_run_begin(table,&run,msg.pay,msg.pay_len,2);
        if(status==LMB_SEG_STATUS_OK) {
            assert(msg.pay_len==bytes && run.rows==1 && run.position==(uint64_t)s->model.pos);
            const float *in=(const float *)msg.pay;
            assert(token>=0 && token<c.vocab);
            if(lmb_v41_aux_read(&s->model,s->counts,in+aux,lmb_v41_aux_width(&c))) {
                assert(lmb_seg_table_run_abort(table,&run)==LMB_SEG_STATUS_OK);
                status=LMB_SEG_STATUS_BAD_REQUEST;
            } else {
                s->model.boundary_input=in;s->model.boundary_mix=in+(size_t)c.hc_mult*c.dim;
                s->model.boundary_output=s->last;s->model.boundary_output_mix=s->last+(size_t)c.hc_mult*c.dim;
                forward(&s->model,&token,1,NULL);
                assert(!lmb_v41_aux_write(&s->model,s->counts,s->last+aux,lmb_v41_aux_width(&c)));
                assert(lmb_seg_table_run_commit(table,&run,2)==LMB_SEG_STATUS_OK);s->runs++;
            }
        }
        uint8_t code[4];lmb_put32(code,(uint32_t)status);
        int ok=status==LMB_SEG_STATUS_OK || status==LMB_SEG_STATUS_DUPLICATE;
        assert(!lmb_secure_send(&secure,fd,LMB_SEG_RUN_R,code,4,ok ? s->last : NULL,ok ? (uint32_t)bytes : 0));
        lmb_msg_free(&msg);
    }
    assert(!forbidden_reads);
    for(int i=0;i<CHATS;i++) {
        assert(sessions[i].runs==STEPS);free(sessions[i].last);lmb_v41_model_destroy(&sessions[i].model,0);
    }
    lmb_seg_table_destroy(table);lmb_v41_model_destroy(&engine,1);close(fd);return 0;
}
typedef struct {int fd;pid_t pid;LmbSecure secure;} Peer;
static LmbSegStatus request(Peer *p,LmbSegRun *run,const float *in,size_t bytes,float *out) {
    uint8_t *body=NULL;uint32_t body_len=0;
    assert(!lmb_seg_run_encode(run,&body,&body_len));
    assert(!lmb_secure_send(&p->secure,p->fd,LMB_SEG_RUN,body,body_len,in,(uint32_t)bytes));free(body);
    LmbMsg msg={0};assert(!lmb_secure_recv(&p->secure,p->fd,&msg));
    assert(msg.op==LMB_SEG_RUN_R && msg.body_len==4);
    LmbSegStatus status=(LmbSegStatus)lmb_get32(msg.body);
    if(status==LMB_SEG_STATUS_OK || status==LMB_SEG_STATUS_DUPLICATE) {assert(msg.pay_len==bytes);memcpy(out,msg.pay,bytes);}
    else assert(!msg.pay_len);
    lmb_msg_free(&msg);return status;
}
static void embed(Model *reference,int token,float *row) {
    const Cfg *c=&reference->c;size_t mix=(size_t)c->hc_mult*c->dim;
    for(int k=0;k<c->hc_mult;k++) {
        for(int j=0;j<c->dim;j++) row[(size_t)k*c->dim+j]=bf16_to_f32(reference->embed.w[(size_t)token*c->dim+j]);
        row[mix+k]=k ? 0.f : 1.f;
    }
}
int main(int argc,char **argv) {
    assert(argc==2);setenv("CTX","128",1);setenv("PIN","off",1);unsetenv("V41_INDEX_OWNER");
    Cfg c;cfg_load(&c,argv[1]);assert(c.n_layers==6);
    Peer peers[2]={0};
    for(int i=0;i<2;i++) {
        int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
        pid_t pid=fork();assert(pid>=0);
        if(!pid) {close(pair[0]);if(i)close(peers[0].fd);return worker(pair[1],argv[1],i ? 3 : 0,i ? 6 : 3,i+2);}
        close(pair[1]);peers[i].fd=pair[0];peers[i].pid=pid;
        uint8_t pk[32],sk[64],expected[32],unused[64];identity(1,pk,sk);identity(i+2,expected,unused);
        int hs=lmb_secure_handshake(pair[0],1,sk,pk,&peers[i].secure);
        if(hs) perror("V41 controller handshake");assert(!hs);
        assert(!memcmp(peers[i].secure.peer_id,expected,32));
    }
    Model engine={0},ref[CHATS]={0};model_load(&engine,argv[1],c.n_routed,32);warm(&engine);
    size_t w=width(&c),aux=aux_offset(&c),bytes=w*sizeof(float);
    float *row[CHATS],*middle=malloc(bytes),*out=malloc(bytes),*duplicate=malloc(bytes);
    float *expected=malloc((size_t)c.vocab*4),*actual=malloc((size_t)c.vocab*4);
    float *collapsed=malloc((size_t)c.dim*4),*normal=malloc((size_t)c.dim*4);
    assert(middle && out && duplicate && expected && actual && collapsed && normal);
    for(int i=0;i<CHATS;i++) {
        assert(!lmb_v41_session_model(&ref[i],&engine));row[i]=calloc(w,4);assert(row[i]);
        int counts[V41_MAX_LAYERS]={0};assert(!lmb_v41_aux_write(&ref[i],counts,row[i]+aux,lmb_v41_aux_width(&c)));
    }
    int tokens[CHATS]={177,28};weights_sealed=1;
    for(int step=0;step<STEPS;step++) for(int chat=0;chat<CHATS;chat++) {
        embed(&engine,tokens[chat],row[chat]);
        float *in=row[chat];
        for(int i=0;i<2;i++) {
            LmbSegOpen o=scope(chat,i ? 3 : 0,i ? 6 : 3,(uint32_t)w);
            LmbSegRun run={.session_id=o.session_id,.owner=o.owner,.sequence=(uint64_t)step,
                .position=(uint64_t)step,.rows=1,.token_count=1,.token_ids=&tokens[chat]};
            run.request_id.bytes[0]=(uint8_t)(step+2);
            if(step==5) {
                run.owner.fencing_epoch--;assert(request(&peers[i],&run,in,bytes,out)==LMB_SEG_STATUS_STALE_OWNER);
                run.owner.fencing_epoch+=2;assert(request(&peers[i],&run,in,bytes,out)==LMB_SEG_STATUS_CONFLICT);run.owner.fencing_epoch--;
            }
            if(step==6) {
                assert(request(&peers[i],&run,in,bytes-4,out)==LMB_SEG_STATUS_BAD_REQUEST);
                float saved=in[aux];in[aux]=NAN;
                assert(request(&peers[i],&run,in,bytes,out)==LMB_SEG_STATUS_BAD_REQUEST);in[aux]=saved;
            }
            assert(request(&peers[i],&run,in,bytes,out)==LMB_SEG_STATUS_OK);
            if(step==7) {
                assert(request(&peers[i],&run,in,bytes,duplicate)==LMB_SEG_STATUS_DUPLICATE);
                assert(!memcmp(out,duplicate,bytes));
            }
            if(!i) {memcpy(middle,out,bytes);in=middle;}
        }
        memcpy(row[chat]+aux,out+aux,(w-aux)*4); /* THIS chat's tail-to-head feedback */
        for(int j=0;j<c.dim;j++) {
            float sum=0;for(int k=0;k<c.hc_mult;k++) sum+=out[(size_t)c.hc_mult*c.dim+k]*out[(size_t)k*c.dim+j];
            collapsed[j]=sum;
        }
        rms_into(normal,collapsed,engine.norm.w,c.dim,c.norm_eps);mvb(actual,&engine.head,normal);
        forward(&ref[chat],&tokens[chat],1,expected);
        assert(!memcmp(expected,actual,(size_t)c.vocab*4));tokens[chat]=argmax(expected,c.vocab);
    }
    for(int i=0;i<2;i++) {
        assert(!lmb_secure_send(&peers[i].secure,peers[i].fd,LMB_SEG_CLOSE,NULL,0,NULL,0));close(peers[i].fd);
        int status;assert(waitpid(peers[i].pid,&status,0)==peers[i].pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    assert(!forbidden_reads);
    for(int i=0;i<CHATS;i++) {free(row[i]);lmb_v41_model_destroy(&ref[i],0);}
    lmb_v41_model_destroy(&engine,1);
    free(middle);free(out);free(duplicate);free(expected);free(actual);free(collapsed);free(normal);
    puts("V41 encrypted process boundary: 2 isolated chats x 48 exact logits, fenced/invalid/duplicate RUN, zero weight reads. Not a household integration test.");
    return 0;
}
