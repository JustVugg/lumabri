/* Text client protocols over the existing authenticated, admitted chat path.
 * No model aliases, external provider calls, alternate scheduler or shared KV.
 * Requires the gateway's Cap, JSON, HTTP and engine helpers. */
#ifndef LUMABRI_API_COMPAT_H
#define LUMABRI_API_COMPAT_H

typedef struct {
    int dialect, streaming, include_usage, begun, input_known, anthropic_modern;
    int has_temperature, has_top_p, has_seed;
    double temperature, top_p;
    uint64_t seed;
    uint32_t input_tokens;
    char model[65], id[80]; long long created;
    Cap normalized, system, payload, output;
    unsigned char pending[4]; size_t pending_size, output_bytes;
} ApiCompat;

static void api_compat_free(ApiCompat *c) {
    Cap *parts[]={&c->normalized,&c->system,&c->payload,&c->output};
    for (unsigned i=0;i<4;i++) {
        if (parts[i]->p) { lmb_api_wipe(parts[i]->p,parts[i]->len); free(parts[i]->p); }
    }
    memset(c,0,sizeof *c);
}
static int api_compat_number(const LmbJson *j,int index,double low,double high,double *out) {
    if (index<0 || j->tokens[index].kind!=LMB_JSON_NUMBER) return -1;
    const LmbJsonToken *t=&j->tokens[index]; char text[64]; size_t n=t->end-t->begin;
    if (!n || n>=sizeof text) return -1;
    memcpy(text,j->text+t->begin,n); text[n]=0; char *end;
    errno=0; double value=strtod(text,&end);
    if (errno || *end || !isfinite(value) || value<low || value>high) return -1;
    *out=value; return 0;
}
static int api_compat_text(const LmbJson *j,unsigned index,Cap *text) {
    if (index>=j->count) return -1;
    const LmbJsonToken *t=&j->tokens[index];
    if (t->kind==LMB_JSON_STRING) {
        char *s=malloc(t->end-t->begin+1); if (!s) return -1;
        int bad=lmb_api_json_string(j,index,s,t->end-t->begin+1) ||
            strlen(s)>65536-text->len || cap_str(text,s);
        free(s); return bad ? -1 : 0;
    }
    if (t->kind!=LMB_JSON_ARRAY || !t->children || t->children>128) return -1;
    const char *const keys[]={"type","text"};
    for (unsigned p=index+1;p<t->next;p=j->tokens[p].next) {
        int f[2]; char type[16];
        if (lmb_json_fields(j,p,keys,2,f) || f[0]<0 || f[1]<0 ||
            lmb_api_json_string(j,(unsigned)f[0],type,sizeof type) || strcmp(type,"text") ||
            j->tokens[f[1]].kind!=LMB_JSON_STRING || api_compat_text(j,(unsigned)f[1],text)) return -1;
    }
    return 0;
}
static int api_compat_turn(Cap *body,const char *role,Cap *text,int comma) {
    return api_addf(body,"%s{\"role\":\"%s\",\"content\":",comma ? "," : "",role) ||
        api_json_text(body,text->p ? text->p : "") || cap_str(body,"}") ? -1 : 0;
}
/* A strict subset: reject unsupported parameters before touching a replica.
 * Consecutive same-role text turns are joined; final assistant prefills are not
 * supported by the current resident chat contract. */
static int api_compat_request(ApiCompat *c,const char *body,size_t length) {
    LmbJsonToken tokens[2048]; LmbJson j; int f[11];
    const char *const openai[]={"model","messages","max_tokens","stream","temperature","top_p",
        "seed","max_completion_tokens","stream_options","n","store"};
    const char *const anthropic[]={"model","messages","max_tokens","stream","temperature","top_p","system"};
    if (lmb_json_parse(&j,body,length,tokens,2048) ||
        lmb_json_fields(&j,0,c->dialect==API_OPENAI ? openai : anthropic,c->dialect==API_OPENAI ? 11 : 7,f) ||
        f[0]<0 || f[1]<0 || lmb_api_json_string(&j,(unsigned)f[0],c->model,sizeof c->model) ||
        strlen(c->model)!=64 || lmb_unhex((uint8_t[32]){0},c->model,32)) return -1;
    uint32_t maximum=256;
    int limit=f[2];
    if (c->dialect==API_OPENAI && f[7]>=0) { if (limit>=0) return -1; limit=f[7]; }
    if ((c->dialect==API_ANTHROPIC && limit<0) ||
        (limit>=0 && lmb_json_uint(&j,(unsigned)limit,&maximum)) || !maximum || maximum>4096) return -1;
    if (f[3]>=0) {
        if (tokens[f[3]].kind!=LMB_JSON_TRUE && tokens[f[3]].kind!=LMB_JSON_FALSE) return -1;
        c->streaming=tokens[f[3]].kind==LMB_JSON_TRUE;
    }
    c->has_temperature=f[4]>=0; c->has_top_p=f[5]>=0;
    if (c->anthropic_modern && (c->has_temperature || c->has_top_p)) return -1;
    if ((c->has_temperature && api_compat_number(&j,f[4],0,c->dialect==API_OPENAI ? 2 : 1,&c->temperature)) ||
        (c->has_top_p && (api_compat_number(&j,f[5],0,1,&c->top_p) || c->top_p<=0))) return -1;
    if (c->dialect==API_OPENAI) {
        if (f[6]>=0) { uint32_t seed; if (lmb_json_uint(&j,(unsigned)f[6],&seed)) return -1; c->has_seed=1; c->seed=seed; }
        if (f[8]>=0) {
            const char *const keys[]={"include_usage","include_obfuscation"}; int o[2];
            if (!c->streaming || lmb_json_fields(&j,(unsigned)f[8],keys,2,o)) return -1;
            if (o[0]>=0) {
                if (tokens[o[0]].kind!=LMB_JSON_TRUE && tokens[o[0]].kind!=LMB_JSON_FALSE) return -1;
                c->include_usage=tokens[o[0]].kind==LMB_JSON_TRUE;
            }
            if (o[1]>=0 && tokens[o[1]].kind!=LMB_JSON_FALSE) return -1;
        }
        uint32_t n;
        if ((f[9]>=0 && (lmb_json_uint(&j,(unsigned)f[9],&n) || n!=1)) ||
            (f[10]>=0 && tokens[f[10]].kind!=LMB_JSON_FALSE)) return -1;
    } else if (f[6]>=0 && api_compat_text(&j,(unsigned)f[6],&c->system)) return -1;
    if (tokens[f[1]].kind!=LMB_JSON_ARRAY || !tokens[f[1]].children || tokens[f[1]].children>130) return -1;
    if (api_addf(&c->normalized,"{\"model\":\"%s\",\"max_tokens\":%u,\"messages\":[",c->model,maximum)) return -1;
    const char *const keys[]={"role","content"}; Cap text={0}; char previous[16]=""; unsigned turns=0;
    for (unsigned p=(unsigned)f[1]+1;p<tokens[f[1]].next;p=tokens[p].next) {
        int m[2]; char role[16]; Cap part={0};
        if (lmb_json_fields(&j,p,keys,2,m) || m[0]<0 || m[1]<0 ||
            lmb_api_json_string(&j,(unsigned)m[0],role,sizeof role) || api_compat_text(&j,(unsigned)m[1],&part)) {
            free(part.p); goto fail;
        }
        if (!strcmp(role,"system") || !strcmp(role,"developer")) {
            int bad=c->dialect!=API_OPENAI || previous[0] ||
                (c->system.len && cap_str(&c->system,"\n\n")) ||
                c->system.len>65536 || part.len>65536-c->system.len || cap_str(&c->system,part.p ? part.p : "");
            free(part.p); if (bad) goto fail; continue;
        }
        if ((strcmp(role,"user") && strcmp(role,"assistant")) || (!previous[0] && strcmp(role,"user"))) { free(part.p); goto fail; }
        if (previous[0] && strcmp(previous,role)) {
            if (api_compat_turn(&c->normalized,previous,&text,turns++)) { free(part.p); goto fail; }
            free(text.p); text=(Cap){0};
        } else if (previous[0] && cap_str(&text,"\n\n")) { free(part.p); goto fail; }
        if (text.len>65536 || part.len>65536-text.len || cap_str(&text,part.p ? part.p : "")) { free(part.p); goto fail; }
        free(part.p); strcpy(previous,role);
    }
    if (strcmp(previous,"user") || turns>=129 || api_compat_turn(&c->normalized,previous,&text,turns) || cap_str(&c->normalized,"]}")) goto fail;
    free(text.p);
    uint8_t random[16]; char hex[33]; lmb_random(random,sizeof random); lmb_hex(hex,random,sizeof random);
    snprintf(c->id,sizeof c->id,"%s%s",c->dialect==API_OPENAI ? "chatcmpl-" : "msg_",hex);
    c->created=(long long)time(NULL); return 0;
fail:
    free(text.p); return -1;
}
/* System text retains a system role in each family template. Existing native
 * turn formatting remains the source for user/assistant and thinking markers. */
static int api_compat_payload(ApiCompat *c,const LmbJson *j,unsigned messages,EngKind kind) {
    Cap *out=&c->payload; const char *sys=c->system.p ? c->system.p : "";
    if (kind==EK_QWEN38 && *sys) {
        /* Keep the family default reasoning instruction in the same system turn. */
        if (cap_str(out,"<|im_start|>system\n") || cap_str(out,sys) || cap_str(out,"\n\n") ||
            cap_str(out,QW38_SYS+strlen("<|im_start|>system\n"))) return -1;
    } else if (serve2_prefix(out,kind)) return -1;
    if (*sys) {
        int bad=0;
        switch (kind) {
        case EK_OLMOE: bad=cap_str(out,"|||IP_ADDRESS|||<|system|>\n") || cap_str(out,sys) || cap_str(out,"\n"); break;
        case EK_GLM: case EK_GLM53: bad=cap_str(out,"<|system|>") || cap_str(out,sys); break;
        case EK_QWEN38: break;
        case EK_QWEN36: bad=cap_str(out,"<|im_start|>system\n") || cap_str(out,sys) || cap_str(out,QW_AEND); break;
        case EK_INKLING: bad=cap_str(out,"<|message_system|><|content_text|>") || cap_str(out,sys) || cap_str(out,INK_AEND); break;
        case EK_KIMI: bad=cap_addf(out,"M system %zu\n",strlen(sys)) || cap_str(out,sys); break;
        case EK_DEEPSEEK: bad=cap_str(out,sys); break;
        default: return -1;
        }
        if (bad) return -1;
    }
    const char *const keys[]={"role","content"}; unsigned turn=0; char *user=NULL;
    for (unsigned p=messages+1;p<j->tokens[messages].next;p=j->tokens[p].next,turn++) {
        int fields[2]; char role[16];
        if (lmb_json_fields(j,p,keys,2,fields) || fields[0]<0 || fields[1]<0 ||
            lmb_api_json_string(j,(unsigned)fields[0],role,sizeof role) || strcmp(role,turn&1 ? "assistant" : "user")) goto fail;
        char *text=malloc(65537); if (!text) goto fail;
        if (lmb_api_json_string(j,(unsigned)fields[1],text,65537)) { free(text); goto fail; }
        if (!(turn&1)) user=text;
        if ((turn&1) || j->tokens[p].next==j->tokens[messages].next) {
            const char *answer=turn&1 ? text : NULL; Cap rendered={0};
            int first=turn<=1;
            int bad=serve2_turn(&rendered,kind,first,user,answer);
            const char *piece=rendered.p;
            if (!bad && first && *sys && kind==EK_OLMOE) piece+=strlen("|||IP_ADDRESS|||");
            if (!bad) bad=cap_str(out,piece);
            free(rendered.p); free(user); user=NULL;
            if (turn&1) free(text);
            if (bad) goto fail;
        }
    }
    return 0;
fail:
    free(user); return -1;
}
static int api_compat_submit(Engine *e,ApiCompat *c,uint32_t max_new,const uint64_t *seed) {
    double temperature=c->has_temperature ? c->temperature : e->greedy_only ? 0 : c->dialect==API_ANTHROPIC ? 1 : .7;
    double top_p=c->has_top_p ? c->top_p : .95;
    if ((e->greedy_only && temperature>0) || (seed && !e->request_seed_supported)) return -1;
    unsigned id=++e->request_sequence; if (!id) id=++e->request_sequence;
    char header[192],suffix[32]="";
    if (seed) snprintf(suffix,sizeof suffix," %llu",(unsigned long long)*seed);
    int n=snprintf(header,sizeof header,"SUBMIT %u 0 %zu %u %.17g %.17g%s\n",id,c->payload.len,max_new,temperature,top_p,suffix);
    snprintf(e->request_id,sizeof e->request_id,"%u",id);
    return n<0 || (size_t)n>=sizeof header || engine_write_full(e->to,header,(size_t)n) ||
        engine_write_full(e->to,c->payload.p,c->payload.len) || engine_write_full(e->to,"\n",1) ? -1 : 0;
}
static int api_compat_data_event(int fd,const char *json) {
    return api_write(fd,"data: ",6) || api_write(fd,json,strlen(json)) || api_write(fd,"\n\n",2) ? -1 : 0;
}
static int api_compat_begin(int fd,ApiCompat *c) {
    if (c->begun || !c->streaming) return 0;
    Cap p={0}; int bad;
    if (c->dialect==API_OPENAI) {
        bad=api_addf(&p,"{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":\"%s\","
            "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]%s}",
            c->id,c->created,c->model,c->include_usage ? ",\"usage\":null" : "") || api_compat_data_event(fd,p.p);
    } else {
        if (!c->input_known) return -1;
        bad=api_addf(&p,"{\"type\":\"message_start\",\"message\":{\"id\":\"%s\",\"type\":\"message\",\"role\":\"assistant\","
            "\"model\":\"%s\",\"content\":[],\"stop_reason\":null,\"stop_sequence\":null,"
            "\"usage\":{\"input_tokens\":%u,\"output_tokens\":0}}}",c->id,c->model,c->input_tokens) ||
            api_event(fd,"message_start",p.p) || api_event(fd,"content_block_start",
                "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    }
    free(p.p); if (!bad) c->begun=1; return bad ? -1 : 0;
}
static int api_compat_progress(int fd,ApiCompat *c,const char *line) {
    const char *p=strchr(line+9,' '); unsigned current,total; char tail;
    if (p && sscanf(p," PREFILL %u %u %c",&current,&total,&tail)==2 && total && total<=1048576 && current<=total) {
        if (c->input_known && c->input_tokens!=total) return -1;
        c->input_known=1; c->input_tokens=total;
        return api_compat_begin(fd,c);
    }
    return 0;
}
static int api_compat_text_delta(int fd,ApiCompat *c,const char *text) {
    if (!c->streaming) return cap_str(&c->output,text);
    if (api_compat_begin(fd,c)) return -1;
    Cap p={0}; int bad;
    if (c->dialect==API_OPENAI) {
        bad=api_addf(&p,"{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":\"%s\","
            "\"choices\":[{\"index\":0,\"delta\":{\"content\":",c->id,c->created,c->model) || api_json_text(&p,text) ||
            api_addf(&p,"},\"finish_reason\":null}]%s}",c->include_usage ? ",\"usage\":null" : "") || api_compat_data_event(fd,p.p);
    } else {
        bad=cap_str(&p,"{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":") ||
            api_json_text(&p,text) || cap_str(&p,"}}") || api_event(fd,"content_block_delta",p.p);
    }
    free(p.p); return bad ? -1 : 0;
}
/* DATA boundaries need not be UTF-8 boundaries. Carry up to three bytes,
 * never send invalid JSON, and reject malformed text instead of replacing it. */
static int api_compat_delta(int fd,ApiCompat *c,const unsigned char *bytes,size_t length) {
    if (length>16384 || c->output_bytes>(8u<<20)-length) return -1;
    unsigned char joined[16389]; size_t n=c->pending_size+length,at=0;
    memcpy(joined,c->pending,c->pending_size); memcpy(joined+c->pending_size,bytes,length);
    c->output_bytes+=length; c->pending_size=0;
    while (at<n) {
        unsigned lead=joined[at],need=lead<128 ? 1 : lead>=0xc2 && lead<=0xdf ? 2 :
            lead>=0xe0 && lead<=0xef ? 3 : lead>=0xf0 && lead<=0xf4 ? 4 : 0;
        if (!lead || !need) return -1;
        if (n-at<need) { c->pending_size=n-at; memcpy(c->pending,joined+at,c->pending_size); break; }
        if (!lmb_json_utf8(joined+at,n-at)) return -1;
        at+=need;
    }
    joined[at]=0;
    return at ? api_compat_text_delta(fd,c,(const char *)joined) : 0;
}
static int api_compat_finish(int fd,ApiCompat *c,const char *stat) {
    unsigned output,input; double rate,hit,rss;
    const char *stop=strstr(stat," STOP1 ");
    int eos=stop && !strncmp(stop," STOP1 eos ",11),limit=stop && !strncmp(stop," STOP1 length ",14);
    if (c->pending_size || sscanf(stat,"STAT %u %lf %lf %lf %u",&output,&rate,&hit,&rss,&input)!=5 ||
        !input || input>1048576 || !output || output>4096 || (!eos && !limit) ||
        (c->input_known && input!=c->input_tokens)) return -1;
    c->input_known=1; c->input_tokens=input;
    Cap p={0}; int bad=0;
    const char *reason=c->dialect==API_OPENAI ? (eos ? "stop" : "length") : (eos ? "end_turn" : "max_tokens");
    if (!c->streaming) {
        if (c->dialect==API_OPENAI) {
            bad=api_addf(&p,"{\"id\":\"%s\",\"object\":\"chat.completion\",\"created\":%lld,\"model\":\"%s\","
                "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":",c->id,c->created,c->model) ||
                api_json_text(&p,c->output.p ? c->output.p : "") || api_addf(&p,"},\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%u,\"completion_tokens\":%u,\"total_tokens\":%u}}",reason,input,output,input+output);
        } else {
            bad=api_addf(&p,"{\"id\":\"%s\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"%s\","
                "\"content\":[{\"type\":\"text\",\"text\":",c->id,c->model) || api_json_text(&p,c->output.p ? c->output.p : "") ||
                api_addf(&p,"}],\"stop_reason\":\"%s\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":%u,\"output_tokens\":%u}}",reason,input,output);
        }
        if (!bad) bad=api_response(fd,200,p.p);
    } else if (api_compat_begin(fd,c)) bad=1;
    else if (c->dialect==API_OPENAI) {
        bad=api_addf(&p,"{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":\"%s\","
            "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"%s\"}]%s}",c->id,c->created,c->model,reason,
            c->include_usage ? ",\"usage\":null" : "") || api_compat_data_event(fd,p.p);
        if (!bad && c->include_usage) {
            free(p.p); p=(Cap){0};
            bad=api_addf(&p,"{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":\"%s\",\"choices\":[],"
                "\"usage\":{\"prompt_tokens\":%u,\"completion_tokens\":%u,\"total_tokens\":%u}}",c->id,c->created,c->model,input,output,input+output) ||
                api_compat_data_event(fd,p.p);
        }
        if (!bad) bad=api_compat_data_event(fd,"[DONE]");
    } else {
        bad=api_event(fd,"content_block_stop","{\"type\":\"content_block_stop\",\"index\":0}") ||
            api_addf(&p,"{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"%s\",\"stop_sequence\":null},"
                "\"usage\":{\"output_tokens\":%u}}",reason,output) || api_event(fd,"message_delta",p.p) ||
            api_event(fd,"message_stop","{\"type\":\"message_stop\"}");
    }
    free(p.p); return bad ? -1 : 0;
}
static void api_compat_failure(int fd,ApiCompat *c,const char *message) {
    if (!c->streaming) { api_error(fd,502,"generation_incomplete"); return; }
    Cap p={0};
    if (!cap_str(&p,c->dialect==API_ANTHROPIC ? "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":" :
                   "{\"error\":{\"type\":\"server_error\",\"code\":\"generation_incomplete\",\"message\":") &&
        !api_json_text(&p,message) && !cap_str(&p,"}}")) {
        if (c->dialect==API_ANTHROPIC) (void)api_event(fd,"error",p.p);
        else (void)api_compat_data_event(fd,p.p);
    }
    free(p.p); /* No DONE/message_stop: an incomplete reply is not success. */
}
static int api_compat_model(Cap *body,const char *id,const char *name,int comma) {
    return api_addf(body,"%s{\"id\":\"%s\",\"object\":\"model\",\"created\":0,\"owned_by\":\"lumabri\",\"name\":",comma ? "," : "",id) ||
        api_json_text(body,name) || cap_str(body,"}") ? -1 : 0;
}
#endif
