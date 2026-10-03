/* Scientific boundary gate for the pinned V4.1 engine. Not an ABI adapter.
 * A generated BUILD COPY supplies range/load hooks, not different math. */
#define _GNU_SOURCE
#undef NDEBUG
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/wait.h>
static int weights_sealed;
static unsigned forbidden_reads;
static ssize_t guarded_pread(int fd, void *buf, size_t n, off_t off) {
    if (weights_sealed) { forbidden_reads++; errno = EIO; return -1; }
    return pread(fd, buf, n, off);
}
#define pread guarded_pread
#define main upstream_cli_main
#include "v41_range_core.c"
#undef main
#undef pread
#include "engine_patches/v41_boundary.h"
#include "engine_patches/v41_lifecycle.h"

static void warm(Model *m) {
    for (int layer = m->range_enabled ? m->range_begin : 0;
         layer < (m->range_enabled ? m->range_end : m->c.n_layers); layer++)
        for (int e = 0; e < m->c.n_routed; e++)
            (void)expert_slot_at(m, &m->cache[layer], "layers", layer, e);
}

/* Deliberately simple oracle transfer. Production needs a bounded delta
 * frame and session fencing, not full-context KV on every hop. */
static void shared_transfer(const Model *from, Model *to) {
    const Cfg *c = &from->c;
    for (int i = 0; i < c->n_layers; i++) if (c->kv_source[i]) {
        size_t rows = (size_t)c->max_positions / c->compress_ratio[i];
        memcpy(to->L[i].ckv, from->L[i].ckv, rows * c->head_dim * sizeof(float));
        memcpy(to->L[i].ikey, from->L[i].ikey, rows * c->index_head_dim * sizeof(float));
    }
    to->published_index_layer = from->published_index_layer;
    to->published_index_k = from->published_index_k ? to->L[from->published_index_layer].ikey : NULL;
    size_t n = (size_t)from->candidate_rows * from->candidate_width;
    to->candidates = realloc(to->candidates, n ? n : 1);
    if (n) memcpy(to->candidates, from->candidates, n);
    to->candidate_rows = from->candidate_rows; to->candidate_width = from->candidate_width;
    n = (size_t)from->shared_topk_rows * from->shared_topk_width;
    to->shared_topk = realloc(to->shared_topk, n ? n * sizeof(int) : 1);
    if (n) memcpy(to->shared_topk, from->shared_topk, n * sizeof(int));
    to->shared_topk_rows = from->shared_topk_rows; to->shared_topk_width = from->shared_topk_width;
}

static void prove(const char *path, int cut, int prefill, int omit_state, int omit_mix, int delta) {
    Model reference = {0}, left = {.range_enabled=1, .range_end=cut},
          right = {.range_enabled=1, .range_begin=cut};
    Cfg c; cfg_load(&c, path); right.range_end = c.n_layers;
    model_load(&reference, path, c.n_routed, 32);
    model_load(&left, path, c.n_routed, 32);
    model_load(&right, path, c.n_routed, 32);
    warm(&reference); warm(&left); warm(&right);
    uint64_t ref_misses=reference.miss,left_misses=left.miss,right_misses=right.miss;
    assert(!left.embed.w && !left.head.w && !right.embed.w && !right.head.w);
    for (int i = 0; i < c.n_layers; i++) {
        assert(!!left.L[i].wkv.q == (i < cut));
        assert(!!right.L[i].wkv.q == (i >= cut));
        assert(!!left.cache[i].slot == (i < cut));
        assert(!!right.cache[i].slot == (i >= cut));
    }
    weights_sealed = 1;
    int ids[8] = {177,108,28,250,26,4,230,64};
    float *h = calloc((size_t)8*c.hc_mult*c.dim,sizeof(float));
    float *mix = calloc((size_t)8*c.hc_mult,sizeof(float));
    float *expected = calloc((size_t)c.vocab,sizeof(float));
    float *got = calloc((size_t)c.vocab,sizeof(float));
    float *collapsed = calloc((size_t)c.dim,sizeof(float));
    float *normal = calloc((size_t)c.dim,sizeof(float));
    assert(h && mix && expected && got && collapsed && normal);
    int differences = 0;
    int left_counts[V41_MAX_LAYERS]={0},right_counts[V41_MAX_LAYERS]={0};
    size_t aux_n=lmb_v41_aux_width(&c);
    float *aux=calloc(aux_n,sizeof(float)); assert(aux);
    for (int step=0;step<48;step++) {
        int rows = step ? 1 : prefill;
        if (step && !omit_state) {
            if(delta) {
                assert(!lmb_v41_aux_write(&right,right_counts,aux,aux_n));
                assert(!lmb_v41_aux_read(&left,left_counts,aux,aux_n));
            } else shared_transfer(&right,&left);
        }
        for(int t=0;t<rows;t++) {
            for(int k=0;k<c.hc_mult;k++) {
                mix[t*c.hc_mult+k] = k ? 0.f : 1.f;
                for(int j=0;j<c.dim;j++) h[((size_t)t*c.hc_mult+k)*c.dim+j] = bf16_to_f32(reference.embed.w[ids[t]*c.dim+j]);
            }
        }
        left.boundary_input=h; left.boundary_mix=mix; left.boundary_output=h; left.boundary_output_mix=mix;
        forward(&left,ids,rows,NULL);
        if(!omit_state) {
            if(delta) {
                assert(!lmb_v41_aux_write(&left,left_counts,aux,aux_n));
                if(!step) {
                    int before[V41_MAX_LAYERS]; memcpy(before,right_counts,sizeof before);
                    float saved=aux[0]; aux[0]=NAN;
                    assert(lmb_v41_aux_read(&right,right_counts,aux,aux_n)); aux[0]=saved;
                    assert(!memcmp(before,right_counts,sizeof before));
                    assert(lmb_v41_aux_read(&right,right_counts,aux,aux_n-1));
                    saved=aux[1]; aux[1]=(float)c.max_positions;
                    assert(lmb_v41_aux_read(&right,right_counts,aux,aux_n)); aux[1]=saved;
                    assert(!memcmp(before,right_counts,sizeof before));
                }
                assert(!lmb_v41_aux_read(&right,right_counts,aux,aux_n));
            } else shared_transfer(&left,&right);
        }
        if(omit_mix) for(int t=0;t<rows;t++) for(int k=0;k<c.hc_mult;k++) mix[t*c.hc_mult+k]=k ? 0.f : 1.f;
        right.boundary_input=h; right.boundary_mix=mix; right.boundary_output=h; right.boundary_output_mix=mix;
        forward(&right,ids,rows,NULL);
        for(int j=0;j<c.dim;j++) {
            float sum=0; for(int k=0;k<c.hc_mult;k++) sum+=mix[(rows-1)*c.hc_mult+k]*h[((size_t)(rows-1)*c.hc_mult+k)*c.dim+j];
            collapsed[j]=sum;
        }
        rms_into(normal,collapsed,reference.norm.w,c.dim,c.norm_eps);
        mvb(got,&reference.head,normal);
        forward(&reference,ids,rows,expected);
        assert(reference.miss==ref_misses && left.miss==left_misses && right.miss==right_misses);
        if(memcmp(got,expected,(size_t)c.vocab*sizeof(float))) differences++;
        if(!omit_state && !omit_mix) {
            if(memcmp(got,expected,(size_t)c.vocab*sizeof(float))) {
                fprintf(stderr,"V41 mismatch split=%d prefill=%d step=%d greedy=%d/%d\n",cut,prefill,step,argmax(got,c.vocab),argmax(expected,c.vocab));
                _exit(2);
            }
            assert(!forbidden_reads);
        }
        ids[0]=argmax(expected,c.vocab);
    }
    assert(!forbidden_reads);
    if(omit_state || omit_mix) assert(differences>0);
    printf("V41 boundary: split %d, prefill %d, 48 steps, %d differing logits blocks, zero weight reads%s%s\n",
        cut,prefill,differences,omit_state || omit_mix ? " (negative control)" : "",delta ? " (bounded delta)" : "");
    free(h); free(mix); free(expected); free(got); free(collapsed); free(normal); free(aux);
    lmb_v41_model_destroy(&reference, 1);
    lmb_v41_model_destroy(&left, 1);
    lmb_v41_model_destroy(&right, 1);
}

static void prove_sessions(const char *path) {
    Model engine={0}, a={0}, b={0}; Cfg c; cfg_load(&c,path);
    model_load(&engine,path,c.n_routed,32); warm(&engine);
    assert(!lmb_v41_session_model(&a,&engine));
    assert(!lmb_v41_session_model(&b,&engine));
    assert(a.L[0].wkv.q==engine.L[0].wkv.q && b.L[0].wkv.q==a.L[0].wkv.q);
    assert(a.L[0].window!=b.L[0].window && a.cache[0].slot!=b.cache[0].slot);
    assert(a.L[1].ckv!=b.L[1].ckv && a.L[1].ikey!=b.L[1].ikey);
    assert(a.engram.table[0].resident_w==b.engram.table[0].resident_w);
    assert(a.engram.table[0].value!=b.engram.table[0].value);
    float *history=calloc((size_t)48*c.vocab,sizeof(float));
    float *got=calloc((size_t)c.vocab,sizeof(float));
    assert(history && got);
    weights_sealed=1;
    int token=177;
    for(int step=0;step<48;step++) {
        forward(&a,&token,1,history+(size_t)step*c.vocab);
        token=argmax(history+(size_t)step*c.vocab,c.vocab);
    }
    /* A reset must match a brand-new conversation; B follows another prompt
     * between A's steps, while sharing exactly the same immutable weights. */
    lmb_v41_state_reset(&a);
    token=177; int other=28;
    for(int step=0;step<48;step++) {
        forward(&b,&other,1,got); other=argmax(got,c.vocab);
        forward(&a,&token,1,got);
        assert(!memcmp(got,history+(size_t)step*c.vocab,(size_t)c.vocab*sizeof(float)));
        token=argmax(got,c.vocab);
    }
    lmb_v41_model_destroy(&a,0);
    assert(!lmb_v41_session_model(&a,&engine));
    token=177;
    forward(&a,&token,1,got);
    assert(!memcmp(got,history,(size_t)c.vocab*sizeof(float)));
    assert(!forbidden_reads);
    free(history);free(got);
    lmb_v41_model_destroy(&a,0);lmb_v41_model_destroy(&b,0);lmb_v41_model_destroy(&engine,1);
    puts("V41 session lifetime: shared weights, isolated conversations, reset and recreate exact; no weight reads.");
}

int main(int argc,char **argv) {
    assert(argc==2 || (argc==3 && !strcmp(argv[2],"--tokens-json")));
    setenv("CTX","128",1); setenv("PIN","off",1);
    unsetenv("V41_INDEX_OWNER");
    Cfg c; cfg_load(&c,argv[1]);
    if(argc==3) {
        char path[1024];snprintf(path,sizeof path,"%s/ref.json",argv[1]);
        FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long length=ftell(f);
        assert(length>0 && length<=16*1024*1024 && !fseek(f,0,SEEK_SET));
        char *text=malloc((size_t)length+1);assert(text);
        size_t n=fread(text,1,(size_t)length,f);assert(!ferror(f) && n==(size_t)length);fclose(f);text[n]=0;
        char *arena=NULL;jval *root=json_parse(text,&arena);assert(root);
        int count=0;int *ids=load_ids(root,"prompt_ids",&count);assert(ids && count>0 && count+8<=128);
        Model model={0};model_load(&model,argv[1],c.n_routed,32);
        float *logits=malloc((size_t)c.vocab*4);assert(logits);
        for(int i=0;i<count;i++)forward(&model,&ids[i],1,logits);
        printf("{\"token_ids\":[");
        for(int i=0;i<8;i++) {
            int next=argmax(logits,c.vocab);printf("%s%d",i?",":"",next);
            if(i!=7)forward(&model,&next,1,logits);
        }
        puts("]}");free(ids);free(logits);json_free(root);free(arena);free(text);lmb_v41_model_destroy(&model,1);return 0;
    }
    for(int test=0;test<(c.n_layers-1)*3+3;test++) {
        fflush(NULL); pid_t pid=fork(); assert(pid>=0);
        if(!pid) {
            if(test==(c.n_layers-1)*3+2) {prove_sessions(argv[1]);return 0;}
            int delta=test>=(c.n_layers-1)*2+2,negative=!delta && test>=(c.n_layers-1)*2;
            int cut=delta ? test-((c.n_layers-1)*2+2)+1 : negative ? 3 : test/2+1;
            int prefill=delta ? 1 : test%2 ? 8 : 1;
            prove(argv[1],cut,prefill,negative && !(test%2),negative && (test%2),delta);
            fflush(NULL); return 0;
        }
        int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("V4.1 RANGE ORACLE PASS: exact logits at every cut; foreign KV and mHC are mandatory.");
}
