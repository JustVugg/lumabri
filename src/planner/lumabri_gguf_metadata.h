/* Convert checkpoint metadata, never weight payloads. Installed code is C;
 * upstream tokenizer declarations are generated at build time. */
#ifndef LMB_GGUF_METADATA_H
#define LMB_GGUF_METADATA_H
#include <stdarg.h>
#include <math.h>
#include "lumabri_planner.h"
#include "src/runtime/lumabri_api_json.h"
#include "lumabri_gguf_tokenizers_data.h"

typedef struct { char *data; size_t length,capacity; int bad; } LmbGgufJson;
typedef struct { const unsigned char *text; size_t length; uint32_t id,kind; } LmbGgufToken;
typedef struct { LmbGgufJson config,tokenizer,tokenizer_config,generation_config; } LmbGgufMetadata;

static inline void lmb_gguf_json_add(LmbGgufJson *j,const void *data,size_t n) {
    if(j->bad || n>LMB_GGUF_BUDGET-j->length){j->bad=1;return;}
    if(j->length+n+1>j->capacity) {
        size_t cap=(j->length+n+1)*2;
        if(cap>LMB_GGUF_BUDGET+1)cap=LMB_GGUF_BUDGET+1;
        char *p=realloc(j->data,cap);
        if(!p){j->bad=1;return;}j->data=p;j->capacity=cap;
    }
    memcpy(j->data+j->length,data,n);j->length+=n;j->data[j->length]=0;
}
static inline void lmb_gguf_json_printf(LmbGgufJson *j,const char *fmt,...) {
    char line[2048];va_list ap;va_start(ap,fmt);int n=vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
    if(n<0 || (size_t)n>=sizeof line){j->bad=1;return;}lmb_gguf_json_add(j,line,(size_t)n);
}
static inline void lmb_gguf_json_quote(LmbGgufJson *j,const unsigned char *p,size_t n) {
    lmb_gguf_json_add(j,"\"",1);
    for(size_t i=0;i<n;) {
        unsigned k=lmb_json_utf8(p+i,n-i);
        if(!k){j->bad=1;return;}
        if(p[i]=='"' || p[i]=='\\') {lmb_gguf_json_add(j,"\\",1);lmb_gguf_json_add(j,p+i,1);}
        else if(p[i]<32)lmb_gguf_json_printf(j,"\\u%04x",p[i]);
        else lmb_gguf_json_add(j,p+i,k);
        i+=k;
    }
    lmb_gguf_json_add(j,"\"",1);
}
static inline int lmb_gguf_strings(const LmbGgufValue *v,LmbGgufToken **out,uint32_t *count) {
    *out=NULL;*count=0;
    if(!v || v->type!=9 || v->element_type!=8 || v->count>1048576)return -1;
    LmbGgufToken *tokens=calloc((size_t)v->count+1,sizeof *tokens);if(!tokens)return -1;
    uint64_t at=0;
    for(uint32_t i=0;i<v->count;i++) {
        if(v->bytes-at<8)goto fail;
        uint64_t n=lmb_gguf_le(v->data+at,8);at+=8;
        if(n>v->bytes-at || n>SIZE_MAX)goto fail;
        tokens[i]=(LmbGgufToken){v->data+at,(size_t)n,i,1};at+=n;
    }
    if(at!=v->bytes)goto fail;
    *out=tokens;*count=(uint32_t)v->count;return 0;
fail:free(tokens);return -1;
}
static inline int lmb_gguf_token_cmp(const void *a,const void *b) {
    const LmbGgufToken *x=a,*y=b;size_t n=x->length<y->length?x->length:y->length;
    int d=memcmp(x->text,y->text,n);return d?d:x->length<y->length?-1:x->length>y->length;
}
static inline int lmb_gguf_uint_at(const LmbGgufValue *v,uint64_t index,uint64_t *value) {
    if(!v || v->type!=9 || index>=v->count)return -1;
    unsigned bytes=lmb_gguf_scalar_bytes(v->element_type);
    if(!bytes || index>SIZE_MAX/bytes || (index+1)*bytes>v->bytes)return -1;
    LmbGgufValue scalar={.type=v->element_type,.bytes=bytes,.data=v->data+index*bytes};
    return lmb_gguf_integer(&scalar,value);
}
static inline int lmb_gguf_number(const LmbGgufValue *v,double fallback,double *out) {
    if(!v){*out=fallback;return 0;}
    if(v->type==6){uint32_t u=(uint32_t)lmb_gguf_le(v->data,4);float f;memcpy(&f,&u,4);*out=f;}
    else if(v->type==12){uint64_t u=lmb_gguf_le(v->data,8);memcpy(out,&u,8);}
    else {uint64_t u;if(lmb_gguf_integer(v,&u))return -1;*out=(double)u;}
    return isfinite(*out)?0:-1;
}
static inline void lmb_gguf_metadata_free(LmbGgufMetadata *m) {
    free(m->config.data);free(m->tokenizer.data);free(m->tokenizer_config.data);free(m->generation_config.data);
    memset(m,0,sizeof *m);
}
static inline int lmb_gguf_olmoe_config(const LmbGguf *g,LmbGgufJson *j) {
    const char *keys[]={"embedding_length","block_count","attention.head_count",
        "attention.head_count_kv","expert_count","expert_used_count","feed_forward_length","context_length"};
    const char *names[]={"hidden_size","num_hidden_layers","num_attention_heads","num_key_value_heads",
        "num_experts","num_experts_per_tok","intermediate_size","max_position_embeddings"};
    uint64_t values[8]={0};
    for(unsigned i=0;i<8;i++) {
        char key[128];snprintf(key,sizeof key,"olmoe.%s",keys[i]);
        const LmbGgufValue *v=lmb_gguf_get(g,key);
        if(i==6 && lmb_gguf_get(g,"olmoe.expert_feed_forward_length"))v=lmb_gguf_get(g,"olmoe.expert_feed_forward_length");
        if(!v && i==3)values[i]=values[2];
        else if(!v && i==7)values[i]=4096;
        else if(lmb_gguf_integer(v,&values[i]) || !values[i] || values[i]>UINT32_MAX)return -1;
    }
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || !tokens->count || tokens->count>1048576)return -1;
    double eps,theta;uint64_t normalized=0;
    if(lmb_gguf_number(lmb_gguf_get(g,"olmoe.attention.layer_norm_rms_epsilon"),1e-5,&eps) || eps<=0 ||
       lmb_gguf_number(lmb_gguf_get(g,"olmoe.rope.freq_base"),10000,&theta) || theta<=0)return -1;
    const LmbGgufValue *norm=lmb_gguf_get(g,"olmoe.expert_weights_norm");
    if(norm && (lmb_gguf_integer(norm,&normalized) || normalized>1))return -1;
    lmb_gguf_json_printf(j,"{\"model_type\":\"olmoe\",\"architectures\":[\"OlmoeForCausalLM\"],\"hidden_act\":\"silu\",\"tie_word_embeddings\":false");
    for(unsigned i=0;i<8;i++)lmb_gguf_json_printf(j,",\"%s\":%llu",names[i],(unsigned long long)values[i]);
    lmb_gguf_json_printf(j,",\"vocab_size\":%llu,\"rms_norm_eps\":%.17g,\"rope_theta\":%.17g,\"norm_topk_prob\":%s",
        (unsigned long long)tokens->count,eps,theta,normalized?"true":"false");
    const char *ids[]={"bos_token_id","eos_token_id"};
    for(unsigned i=0;i<2;i++) {
        char key[96];snprintf(key,sizeof key,"tokenizer.ggml.%s",ids[i]);
        const LmbGgufValue *v=lmb_gguf_get(g,key);uint64_t id;
        lmb_gguf_json_printf(j,",\"%s\":",ids[i]);
        if(!v)lmb_gguf_json_printf(j,"null");
        else {if(lmb_gguf_integer(v,&id) || id>=tokens->count)return -1;lmb_gguf_json_printf(j,"%llu",(unsigned long long)id);}
    }
    lmb_gguf_json_printf(j,"}");
    return j->bad?-1:0;
}
static inline int lmb_gguf_metadata(const LmbGguf *g,LmbGgufMetadata *out) {
    memset(out,0,sizeof *out);LmbGgufToken *tokens=NULL,*merges=NULL,*ordered=NULL;
    uint32_t nt=0,nm=0;const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"olmoe") || lmb_gguf_olmoe_config(g,&out->config))goto fail;
    const LmbGgufValue *v=lmb_gguf_get(g,"tokenizer.ggml.model");
    if(v && (!lmb_gguf_text(v) || strcmp(lmb_gguf_text(v),"gpt2")))goto fail;
    if(lmb_gguf_strings(lmb_gguf_get(g,"tokenizer.ggml.tokens"),&tokens,&nt) || !nt)goto fail;
    v=lmb_gguf_get(g,"tokenizer.ggml.merges");if(v && lmb_gguf_strings(v,&merges,&nm))goto fail;
    v=lmb_gguf_get(g,"tokenizer.ggml.token_type");
    if(v && (v->type!=9 || v->count!=nt))goto fail;
    for(uint32_t i=0;v && i<nt;i++){uint64_t k;if(lmb_gguf_uint_at(v,i,&k) || k>6)goto fail;tokens[i].kind=(uint32_t)k;}
    ordered=malloc((size_t)nt*sizeof *ordered);if(!ordered)goto fail;
    memcpy(ordered,tokens,(size_t)nt*sizeof *ordered);qsort(ordered,nt,sizeof *ordered,lmb_gguf_token_cmp);
    for(uint32_t i=1;i<nt;i++)if(!lmb_gguf_token_cmp(&ordered[i-1],&ordered[i]))goto fail;
    uint32_t tail=nt;while(tail && (tokens[tail-1].kind==3 || tokens[tail-1].kind==4))tail--;
    const LmbGgufTokenizer *decl=&LMB_GGUF_TOKENIZERS[0];
    const char *pre=lmb_gguf_text(lmb_gguf_get(g,"tokenizer.ggml.pre"));
    for(unsigned i=1;pre && i<sizeof LMB_GGUF_TOKENIZERS/sizeof *LMB_GGUF_TOKENIZERS;i++)
        if(!strcmp(pre,LMB_GGUF_TOKENIZERS[i].name)){decl=&LMB_GGUF_TOKENIZERS[i];break;}
    LmbGgufJson *j=&out->tokenizer;
    lmb_gguf_json_printf(j,"{\"version\":\"1.0\",\"pre_tokenizer\":%s,\"decoder\":{\"type\":\"ByteLevel\",\"add_prefix_space\":true,\"trim_offsets\":true,\"use_regex\":true},\"added_tokens\":[",decl->pre);
    int comma=0;
    for(uint32_t i=0;i<nt;i++)if(tokens[i].kind==3 || tokens[i].kind==4) {
        lmb_gguf_json_printf(j,"%s{\"id\":%u,\"content\":",comma?",":"",i);comma=1;
        lmb_gguf_json_quote(j,tokens[i].text,tokens[i].length);
        lmb_gguf_json_printf(j,",\"single_word\":false,\"lstrip\":false,\"rstrip\":false,\"normalized\":false,\"special\":%s}",tokens[i].kind==3?"true":"false");
    }
    lmb_gguf_json_printf(j,"],\"model\":{\"type\":\"BPE\",\"dropout\":null,\"unk_token\":null,\"continuing_subword_prefix\":null,\"end_of_word_suffix\":null,\"fuse_unk\":false,\"byte_fallback\":false,");
    if(strlen(decl->model)>2){lmb_gguf_json_add(j,decl->model+1,strlen(decl->model)-2);lmb_gguf_json_add(j,",",1);}
    lmb_gguf_json_printf(j,"\"vocab\":{");comma=0;
    for(uint32_t i=0;i<nt;i++)if(i<tail || (tokens[i].kind!=3 && tokens[i].kind!=4)) {
        if(comma)lmb_gguf_json_add(j,",",1);
        comma=1;lmb_gguf_json_quote(j,tokens[i].text,tokens[i].length);lmb_gguf_json_printf(j,":%u",i);
    }
    lmb_gguf_json_printf(j,"},\"merges\":[");
    for(uint32_t i=0;i<nm;i++){if(i)lmb_gguf_json_add(j,",",1);lmb_gguf_json_quote(j,merges[i].text,merges[i].length);}
    lmb_gguf_json_printf(j,"]}}");
    uint64_t ids[5]={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    const char *idnames[]={"bos","eos","padding","eot","eom"};
    for(unsigned i=0;i<5;i++) {
        char key[96];snprintf(key,sizeof key,"tokenizer.ggml.%s_token_id",idnames[i]);v=lmb_gguf_get(g,key);
        if(v && (lmb_gguf_integer(v,&ids[i]) || ids[i]>=nt))goto fail;
    }
    j=&out->tokenizer_config;lmb_gguf_json_printf(j,"{\"chat_template\":");
    v=lmb_gguf_get(g,"tokenizer.chat_template");
    if(v){if(v->type!=8)goto fail;lmb_gguf_json_quote(j,v->data,(size_t)v->bytes);}
    else lmb_gguf_json_printf(j,"null");
    const char *outnames[]={"bos_token","eos_token","pad_token"};
    for(unsigned i=0;i<3;i++) {
        lmb_gguf_json_printf(j,",\"%s\":",outnames[i]);
        if(ids[i]==UINT64_MAX)lmb_gguf_json_printf(j,"null");
        else lmb_gguf_json_quote(j,tokens[ids[i]].text,tokens[ids[i]].length);
    }
    uint64_t bos=0;v=lmb_gguf_get(g,"tokenizer.ggml.add_bos_token");
    if(v && (lmb_gguf_integer(v,&bos) || bos>1))goto fail;
    lmb_gguf_json_printf(j,",\"add_bos_token\":%s}",bos?"true":"false");
    j=&out->generation_config;lmb_gguf_json_printf(j,"{\"eos_token_id\":[");comma=0;
    unsigned char *eos=calloc(nt,1);if(!eos)goto fail;
    for(unsigned i=1;i<5;i++)if(i!=2 && ids[i]!=UINT64_MAX)eos[ids[i]]=1;
    v=lmb_gguf_get(g,"tokenizer.ggml.eog_token_ids");
    if(v)for(uint64_t i=0;i<v->count;i++){uint64_t id;if(lmb_gguf_uint_at(v,i,&id) || id>=nt){free(eos);goto fail;}eos[id]=1;}
    for(uint32_t i=0;i<nt;i++) {
        if((tokens[i].length==13 && !memcmp(tokens[i].text,"<|endoftext|>",13)) ||
           (tokens[i].length==10 && !memcmp(tokens[i].text,"<|im_end|>",10)))eos[i]=1;
        if(eos[i]){lmb_gguf_json_printf(j,"%s%u",comma?",":"",i);comma=1;}
    }
    free(eos);lmb_gguf_json_printf(j,"]");
    for(unsigned i=0;i<3;i+=2){lmb_gguf_json_printf(j,",\"%s\":",i?"pad_token_id":"bos_token_id");
        if(ids[i]==UINT64_MAX)lmb_gguf_json_printf(j,"null");else lmb_gguf_json_printf(j,"%llu",(unsigned long long)ids[i]);}
    lmb_gguf_json_printf(j,"}");
    free(ordered);free(tokens);free(merges);
    if(out->config.bad || out->tokenizer.bad || out->tokenizer_config.bad || out->generation_config.bad){lmb_gguf_metadata_free(out);return -1;}
    return 0;
fail:free(ordered);free(tokens);free(merges);lmb_gguf_metadata_free(out);return -1;
}
#endif
