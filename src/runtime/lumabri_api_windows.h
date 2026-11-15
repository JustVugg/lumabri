/* The local web gateway on native Windows: the same compiled page, the same
 * bearer credentials and capability records, loopback only.
 *
 * What is shared with the POSIX gateway is shared as code, not re-described:
 * the HTTP parser and Host/Origin rules, the JSON reader, the access database
 * (digests, grants, per-user request permits), the operator and management
 * capabilities, the private chat history and the donor approval route.
 *
 * What differs is how a request runs. POSIX forks one bounded child per
 * connection and attaches a local engine; Windows has no fork and its chat is
 * a thin client of the approved resident host. Here each connection is one
 * thread with its own socket; anything that plans or speaks to the household
 * services holds one process-wide lock, and a chat turn streams outside it.
 *
 * Routes whose machinery exists only on POSIX answer 501
 * not_available_on_windows_yet (see api_windows_missing) instead of a partial
 * imitation, and /api/v1/session lists them so the page can say so. */
#ifndef LUMABRI_API_WINDOWS_H
#define LUMABRI_API_WINDOWS_H
#include "src/runtime/lumabri_api_http.h"
#include "src/runtime/lumabri_api_json.h"
#include "src/runtime/lumabri_operator_access.h"
#include "src/runtime/lumabri_preparation_request.h"
#include "src/runtime/lumabri_memlock.h"
#include "build/web_assets.h"
#include "build/download_assets.h"
#include <shellapi.h>

#define LMB_API_CHILDREN 16u
#define LMB_API_TURN_SECONDS 300u
#define LMB_API_UNAVAILABLE "not_available_on_windows_yet"

/* The reply helpers keep the POSIX signatures so that shared route code
 * compiles unchanged. A Windows socket does not fit an int descriptor: the
 * connection is this thread's, and the descriptor argument only says
 * "the client" (>=0) or "standard output" (<0). */
static _Thread_local LmbSocket api_client=LMB_INVALID_SOCKET;
static volatile LONG api_stopping;
static CRITICAL_SECTION api_household;

static int checked_printf(char *dst, size_t cap, const char *format, ...) {
    va_list args; va_start(args,format);
    int n=vsnprintf(dst,cap,format,args); va_end(args);
    return n<0 || (size_t)n>=cap ? -1 : 0;
}
static int api_write(int fd, const void *data, size_t n) {
    (void)fd; const char *p=data;
    while (n) {
        int sent=send(api_client,p,n>INT_MAX ? INT_MAX : (int)n,0);
        if (sent<=0) return -1;
        p+=sent; n-=(size_t)sent;
    }
    return 0;
}
static int api_response(int fd, unsigned status, const char *body) {
    char header[512];
    int n=snprintf(header,sizeof header,"HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n",status,status==200 ? "OK" : status==202 ? "Accepted" : "Request failed",strlen(body));
    return n<0 || (size_t)n>=sizeof header || api_write(fd,header,(size_t)n) || api_write(fd,body,strlen(body)) ? -1 : 0;
}
static void api_error(int fd, unsigned status, const char *code) {
    /* code is a server-owned constant, never an unescaped engine/client string. */
    fprintf(stderr,"[api-request] status=%u code=%s\n",status,code);
    char body[512]; snprintf(body,sizeof body,"{\"error\":\"%s\"}\n",code);
    (void)api_response(fd,status,body);
}
static int api_addf(Cap *c, const char *format, ...) {
    char fragment[512]; va_list ap; va_start(ap,format);
    int n=vsnprintf(fragment,sizeof fragment,format,ap); va_end(ap);
    return n<0 || (size_t)n>=sizeof fragment ? -1 : cap_add(c,fragment,(size_t)n);
}
static int api_json_text(Cap *c, const char *s) {
    if (cap_str(c,"\"")) return -1;
    const unsigned char *end=(const unsigned char *)s+strlen(s);
    for (const unsigned char *p=(const unsigned char *)s; p<end; p++) {
        if (*p=='"' || *p=='\\') { if (cap_addf(c,"\\%c",*p)) return -1; }
        else if (*p<32 || *p==127) { if (cap_addf(c,"\\u%04x",*p)) return -1; }
        else if (*p>=128) {
            unsigned n=lmb_json_utf8(p,(size_t)(end-p));
            if (!n || cap_add(c,(const char *)p,n)) return -1;
            p+=n-1;
        }
        else if (cap_add(c,(const char *)p,1)) return -1;
    }
    return cap_str(c,"\"");
}
static int api_event(int fd, const char *event, const char *json) {
    char header[96]; int n=snprintf(header,sizeof header,"event: %s\ndata: ",event);
    return n<0 || (size_t)n>=sizeof header || api_write(fd,header,(size_t)n) ||
        api_write(fd,json,strlen(json)) || api_write(fd,"\n\n",2) ? -1 : 0;
}
#include "src/runtime/lumabri_chat_history.h"
/* The Windows donor answers accept and decline. Standing repeat consent is
 * not offered by it yet, so the shared approval route never advertises it. */
static int home_resident_required(void) { return 0; }
#include "src/runtime/lumabri_donor_approval_api.h"

/* Only these compiled assets are public. No generic file-serving path and no
 * redirects, external scripts, cookies, CORS or browser-persisted credentials. */
static int api_web(int fd, const LmbApiHttp *request) {
    if (strcmp(request->method,"GET") || request->length) return 0;
    const unsigned char *data=NULL; size_t size=0; const char *type=NULL;
    if (!strcmp(request->path,"/")) { data=lmb_web_html; size=sizeof lmb_web_html-1; type="text/html; charset=utf-8"; }
    else if (!strcmp(request->path,"/app.css")) { data=lmb_web_css; size=sizeof lmb_web_css-1; type="text/css; charset=utf-8"; }
    else if (!strcmp(request->path,"/app.js")) { data=lmb_web_js; size=sizeof lmb_web_js-1; type="text/javascript; charset=utf-8"; }
    else if (!strcmp(request->path,"/capacity.js")) { data=lmb_web_capacity_js; size=sizeof lmb_web_capacity_js-1; type="text/javascript; charset=utf-8"; }
    else if (!strcmp(request->path,"/ui.js")) { data=lmb_web_ui_js; size=sizeof lmb_web_ui_js-1; type="text/javascript; charset=utf-8"; }
    else if (!strcmp(request->path,"/logo.svg")) { data=lmb_web_logo; size=sizeof lmb_web_logo-1; type="image/svg+xml"; }
    else if (!strcmp(request->path,"/download.js")) { data=lmb_web_download_js; size=sizeof lmb_web_download_js-1; type="text/javascript; charset=utf-8"; }
    else if (!strcmp(request->path,"/download.css")) { data=lmb_web_download_css; size=sizeof lmb_web_download_css-1; type="text/css; charset=utf-8"; }
    if (!data) return 0;
    char header[1024];
    int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; "
        "connect-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'\r\n"
        "X-Frame-Options: DENY\r\n\r\n",type,size);
    if (n>0 && (size_t)n<sizeof header && !api_write(fd,header,(size_t)n)) (void)api_write(fd,data,size);
    return 1;
}

/* Every route the POSIX gateway serves and this one does not. */
static const char api_windows_unavailable[]="[\"model_download\",\"capacity_measurement\",\"portfolio_policy\","
    "\"elastic_policy\",\"allocation_control\",\"routing_inspection\",\"demand\",\"client_config\","
    "\"openai_anthropic_clients\",\"repeat_consent\",\"reply_recovery\"]";
static int api_windows_missing(const char *path) {
    static const char *const prefixes[]={"/api/v1/downloads","/api/v1/capacity","/api/v1/portfolio-policy",
        "/api/v1/elastic-policy","/api/v1/allocations/","/api/v1/routing/","/api/v1/demand",
        "/api/v1/client-config","/v1/"};
    for (unsigned i=0;i<sizeof prefixes/sizeof *prefixes;i++) {
        size_t n=strlen(prefixes[i]);
        if (!strncmp(path,prefixes[i],n) && (prefixes[i][n-1]=='/' || !path[n] || path[n]=='/')) return 1;
    }
    return 0;
}

static int api_access_path_windows(char *path, size_t cap) {
    return lmb_private_default_path(path,cap,"api-access");
}
static int api_access_wait_lock(int dir) {
    for (unsigned attempt=0; attempt<40; attempt++) {
        int lock=lmb_api_access_lock(dir);
        if (lock>=0) return lock;
        if (errno!=EAGAIN && errno!=EWOULDBLOCK) return -1;
        Sleep(50);
    }
    return -1;
}
/* The one place an inference grant is written. Caller holds the access lock.
 * 0: the user holds the grant. 1: the user's list is full, nothing changed.
 * -1: unknown user, no approved plan with this allocation, or storage failure. */
static int api_grant_allocation(int dir, const char *tracker, const char *name, const uint8_t allocation[32]) {
    LmbApiUser user; LmbResidentPlan *plan=calloc(1,sizeof *plan); int rc=-1;
    if (plan && !lmb_api_user_load(dir,name,&user) && !home_resident_library_find(tracker,allocation,plan)) {
        if (lmb_api_user_allows(&user,allocation)) rc=0;
        else if (user.count>=LMB_API_MODELS_MAX) rc=1;
        else { memcpy(user.allocations[user.count++],allocation,32); rc=lmb_api_user_save(dir,&user) ? -1 : 0; }
    }
    free(plan); return rc;
}

static int api_models(int fd, const char *tracker, const LmbApiUser *user) {
    LmbResidentPlan *plans=calloc(64,sizeof *plans); Cap body={0};
    if (!plans) { if (fd>=0) api_error(fd,500,"allocation_failed"); return -1; }
    size_t n=home_resident_library_list(tracker,plans,64); int bad=cap_str(&body,"{\"schema\":1,\"models\":["),comma=0;
    for (size_t i=0; i<n && !bad; i++) {
        const LmbResidentPlan *p=&plans[i];
        if (!lmb_home_nonzero(p->allocation,32) || (user && !lmb_api_user_allows(user,p->allocation))) continue;
        char id[65]; lmb_hex(id,p->allocation,32);
        bad=api_addf(&body,"%s{\"id\":\"%s\",\"name\":",comma++ ? "," : "",id) ||
            api_json_text(&body,p->model) || api_addf(&body,",\"context\":%u,\"max_tokens\":%u,"
                "\"sessions\":%u,\"state\":\"saved_plan\"}",p->context,p->max_new,p->sessions ? p->sessions : 1);
    }
    if (!bad) bad=cap_str(&body,"]}\n");
    if (bad) { if (fd>=0) api_error(fd,500,"allocation_failed"); }
    else if (fd<0) bad=fputs(body.p,stdout)==EOF;
    else bad=api_response(fd,200,body.p);
    free(body.p); free(plans); return bad ? -1 : 0;
}

/* ---- chat: one turn against the approved resident host ---------------- */
static int api_messages(const LmbJson *j, unsigned index, EngKind kind, Cap *history, char **prompt) {
    if (index>=j->count || j->tokens[index].kind!=LMB_JSON_ARRAY || !j->tokens[index].children ||
        j->tokens[index].children>129 || !(j->tokens[index].children&1)) return -1;
    const char *const keys[]={"role","content"}; char *user=NULL;
    unsigned turn=0;
    for (unsigned p=index+1; p<j->tokens[index].next; p=j->tokens[p].next,turn++) {
        int fields[2]; char role[16];
        if (lmb_json_fields(j,p,keys,2,fields) || fields[0]<0 || fields[1]<0 ||
            lmb_api_json_string(j,(unsigned)fields[0],role,sizeof role) || strcmp(role,turn&1 ? "assistant" : "user")) goto fail;
        char *text=malloc(65537);
        if (!text) goto fail;
        if (lmb_api_json_string(j,(unsigned)fields[1],text,65537)) { free(text); goto fail; }
        if (!(turn&1)) user=text;
        else {
            int bad=serve2_turn(history,kind,turn==1,user,text);
            free(user); user=NULL; free(text); if (bad) goto fail;
        }
    }
    *prompt=user; return 0;
fail:
    free(user); return -1;
}
typedef struct { double started,first,last_metrics; int metrics,closed; char error[256]; } ApiWindowsTurn;
static int api_turn_metrics(ApiWindowsTurn *turn, int force) {
    double now=nowd();
    if (!turn->metrics || (!force && now-turn->last_metrics<.25)) return 0;
    char first[64]="null",payload[320];
    if (turn->first) snprintf(first,sizeof first,"%.3f",turn->first-turn->started);
    int n=snprintf(payload,sizeof payload,"{\"phase\":\"%s\",\"scope\":\"gateway_observed\",\"generated_tokens\":null,"
        "\"decode_tok_s\":null,\"elapsed_seconds\":%.3f,\"ttft_seconds\":%s}",turn->first ? "decode" : "preparing",
        now-turn->started,first);
    turn->last_metrics=now;
    return n<0 || (size_t)n>=sizeof payload ? -1 : api_event(0,"metrics",payload);
}
/* The byte codec may split a UTF-8 scalar between DATA frames. Preserve every
 * byte; clients base64-decode deltas into ONE streaming UTF-8 decoder. */
static int api_turn_delta(void *opaque, const unsigned char *data, size_t size) {
    ApiWindowsTurn *turn=opaque;
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (size && !turn->first) turn->first=nowd();
    while (size && !turn->closed) {
        size_t n=size>12288 ? 12288 : size; char encoded[16400]; size_t out=0;
        for (size_t i=0; i<n; i+=3) {
            uint32_t x=(uint32_t)data[i]<<16;
            if (i+1<n) x|=(uint32_t)data[i+1]<<8;
            if (i+2<n) x|=data[i+2];
            encoded[out++]=alphabet[(x>>18)&63]; encoded[out++]=alphabet[(x>>12)&63];
            encoded[out++]=i+1<n ? alphabet[(x>>6)&63] : '='; encoded[out++]=i+2<n ? alphabet[x&63] : '=';
        }
        encoded[out]=0; Cap payload={0};
        int bad=cap_str(&payload,"{\"bytes\":\"") || cap_str(&payload,encoded) ||
            cap_str(&payload,"\"}") || api_event(0,"delta",payload.p);
        free(payload.p);
        if (bad) { turn->closed=1; return -1; }
        data+=n; size-=n;
    }
    return turn->closed ? -1 : 0;
}
static int api_turn_error(void *opaque, const char *message) {
    ApiWindowsTurn *turn=opaque; snprintf(turn->error,sizeof turn->error,"%s",message); return 0;
}
static void api_chat(int fd, const char *tracker, const LmbApiUser *user, const char *body, size_t length) {
    ApiWindowsTurn turn={.started=nowd()};
    LmbJsonToken *tokens=calloc(1024,sizeof *tokens); LmbJson j; int fields[5];
    LmbResidentPlan *plan=calloc(1,sizeof *plan); LmbReplyStream *stream=calloc(1,sizeof *stream);
    Cap history={0},payload={0}; char *prompt=NULL; LmbSocket host=LMB_INVALID_SOCKET;
    const char *const keys[]={"model","messages","max_tokens","recovery_wait_seconds","stream_metrics"};
    char id[65]; uint8_t allocation[32]; uint32_t max_new,wait_seconds=0;
    if (!tokens || !plan || !stream) { api_error(fd,500,"allocation_failed"); goto finished; }
    if (lmb_json_parse(&j,body,length,tokens,1024) || lmb_json_fields(&j,0,keys,5,fields) ||
        fields[0]<0 || fields[1]<0 || fields[2]<0 || lmb_api_json_string(&j,(unsigned)fields[0],id,sizeof id) ||
        strlen(id)!=64 || lmb_unhex(allocation,id,32) || lmb_json_uint(&j,(unsigned)fields[2],&max_new) || !max_new ||
        (fields[3]>=0 && (lmb_json_uint(&j,(unsigned)fields[3],&wait_seconds) || wait_seconds>120)) ||
        (fields[4]>=0 && tokens[fields[4]].kind!=LMB_JSON_TRUE && tokens[fields[4]].kind!=LMB_JSON_FALSE)) {
        api_error(fd,400,"invalid_chat_request"); goto finished;
    }
    if (!lmb_api_user_allows(user,allocation)) { api_error(fd,403,"model_not_authorized"); goto finished; }
    if (home_resident_library_find(tracker,allocation,plan)) { api_error(fd,404,"resident_plan_not_found"); goto finished; }
    /* The host proves its identity and the pinned checkpoint root before it
     * is sent a single byte of the conversation. */
    LmbHostGreeting greeting={0};
    int opened=lmb_host_client_open(plan->host,NULL,plan->host_key,plan->root,&host,&greeting);
    if (opened || greeting.codec!=2) {
        api_error(fd,opened==HOST_CONNECT_BUSY ? 429 : 503,"host_unavailable_or_busy"); goto finished;
    }
    if (api_messages(&j,(unsigned)fields[1],greeting.kind,&history,&prompt) ||
        lmb_host_turn_payload(&payload,&greeting,history.p ? history.p : "",prompt)) {
        api_error(fd,400,"unsupported_messages_or_limits"); goto finished;
    }
    if (greeting.max_new && max_new>greeting.max_new) max_new=greeting.max_new;
    LmbReplySink sink={api_turn_delta,NULL,api_turn_error,&turn};
    if (lmb_reply_init(stream,"1",UINT64_C(8)<<20,sink) || lmb_host_turn_send(host,&greeting,"1",&payload,max_new)) {
        api_error(fd,503,"submit_failed"); goto finished;
    }
    const char *header="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\nX-Accel-Buffering: no\r\n\r\n";
    if (api_write(fd,header,strlen(header))) goto finished;
    turn.metrics=fields[4]>=0 && tokens[fields[4]].kind==LMB_JSON_TRUE;
    turn.closed=api_turn_metrics(&turn,1)!=0;
    uint64_t until=GetTickCount64()+(uint64_t)LMB_API_TURN_SECONDS*1000; int transport_failed=0;
    while (!turn.closed && !api_stopping && stream->status==LMB_REPLY_MORE) {
        uint64_t now=GetTickCount64();
        if (now>=until) { snprintf(turn.error,sizeof turn.error,"The reply exceeded the %u second limit",LMB_API_TURN_SECONDS); break; }
        fd_set readable; FD_ZERO(&readable); FD_SET(host,&readable); FD_SET(api_client,&readable);
        struct timeval wait={0,250000};
        int ready=select(0,&readable,NULL,NULL,&wait);
        if (ready<0) { transport_failed=1; break; }
        /* No pipelining or half-closed request bodies. A browser AbortController
         * disconnect cancels this conversation, never another user's slot. */
        if (ready && FD_ISSET(api_client,&readable)) { turn.closed=1; break; }
        if (ready && FD_ISSET(host,&readable)) {
            uint64_t remaining=until-now;
            if (lmb_host_turn_read(host,stream,(uint32_t)(remaining<5000 ? remaining : 5000))) { transport_failed=1; break; }
        }
        if (stream->status==LMB_REPLY_MORE && api_turn_metrics(&turn,0)) turn.closed=1;
    }
    if (!turn.closed && !api_stopping && stream->status==LMB_REPLY_DONE) {
        double completed=nowd(); Cap done={0}; LmbGenerationMetrics m; char speed[64]="null",first[64]="null";
        if (turn.first) snprintf(first,sizeof first,"%.9f",turn.first-turn.started);
        int bad=cap_str(&done,"{\"stats\":") || api_json_text(&done,stream->stat) ||
            cap_str(&done,",\"observation_saved\":false,\"recovery_attempts\":0,\"stats_scope\":\"single_attempt\"") ||
            api_addf(&done,",\"timing\":{\"scope\":\"gateway_observed\",\"generation_seconds\":%.9f,"
                "\"completion_seconds\":%.9f,\"ttft_seconds\":%s,\"first_token_notification_seconds\":null,"
                "\"token_notifications\":null,\"token_notification_gaps\":null}",completed-turn.started,
                completed-turn.started,first);
        if (!bad && turn.metrics) {
            if (lmb_metrics_parse(stream->stat,&m)) bad=cap_str(&done,",\"metrics\":null");
            else {
                double rate=lmb_metrics_decode_rate(&m);
                if (rate>0) snprintf(speed,sizeof speed,"%.3f",rate);
                bad=api_addf(&done,",\"metrics\":{\"scope\":\"engine_reported\",\"generated_tokens\":%u,"
                    "\"decode_tok_s\":%s,\"prefill_seconds\":%.9f,\"decode_seconds\":%.9f}",
                    m.generated_tokens,speed,m.prefill_seconds,m.decode_seconds);
            }
        }
        if (!bad && !cap_str(&done,"}")) (void)api_event(fd,"done",done.p);
        free(done.p);
    } else if (!turn.closed && !api_stopping) {
        /* One attempt: a Windows gateway does not replay on another replica. */
        const char *reason=turn.error[0] ? turn.error : transport_failed ?
            "The model's host closed the connection; the response is incomplete" :
            "Stream interrupted; response is incomplete";
        Cap failed={0};
        if (!cap_str(&failed,"{\"message\":") && !api_json_text(&failed,reason) && !cap_str(&failed,"}"))
            (void)api_event(fd,"error",failed.p);
        free(failed.p);
    }
finished:
    /* Closing the host connection is what cancels an unfinished generation. */
    if (host!=LMB_INVALID_SOCKET) lmb_close(host);
    if (history.p) lmb_api_wipe(history.p,history.len);
    if (payload.p) lmb_api_wipe(payload.p,payload.len);
    if (prompt) lmb_api_wipe(prompt,strlen(prompt));
    free(history.p); free(payload.p); free(prompt); free(stream); free(plan); free(tokens);
}

/* ---- catalogue, workspace -------------------------------------------- */
static LmbTuiState *api_catalogue_state(const char *tracker, const LmbPreparationRequest *r) {
    HomeSettings settings;
    if (home_settings_load_windows(&settings)) return NULL;
    LmbTuiState *st=calloc(1,sizeof *st);
    if (st) {
        st->context=r ? r->context : LMB_HOME_DEFAULT_CONTEXT; st->sessions=r ? r->sessions : 1; st->max_new=r ? r->max_new : 128;
        int bad=checked_printf(st->root,sizeof st->root,"%s",settings.models) ||
            checked_printf(st->disk,sizeof st->disk,"%s",settings.models) ||
            checked_printf(st->tracker,sizeof st->tracker,"%s",tracker);
        if (r) for (uint32_t i=0;i<r->node_count && i<LMB_CLUSTER_MAX_NODES;i++) strcpy(st->selected_nodes[i],r->nodes[i]);
        st->refresh=catalog_refresh_windows;
        if (bad || (r && r->node_count>LMB_CLUSTER_MAX_NODES) || catalog_refresh_windows(st,NULL)) { free(st); st=NULL; }
    }
    SecureZeroMemory(&settings,sizeof settings); return st;
}
static void api_catalogue(int fd, const char *tracker) {
    LmbTuiState *st=api_catalogue_state(tracker,NULL); Cap body={0};
    int bad=!st || cap_str(&body,"{\"schema\":1,\"source\":\"owner_configured_folder\",\"models\":[");
    for (int i=0;st && i<st->nmodels && !bad;i++) {
        const LmbTuiModel *m=&st->models[i];
        bad=(i && cap_str(&body,",")) || cap_str(&body,"{\"name\":") || api_json_text(&body,m->name) ||
            cap_str(&body,",\"adapter\":") || api_json_text(&body,m->shape.segment_id) ||
            cap_str(&body,",\"tier\":") || api_json_text(&body,lmb_catalogue_tier(m)) ||
            cap_str(&body,",\"support_state\":") || api_json_text(&body,lmb_catalogue_support_state(m)) ||
            cap_str(&body,",\"support_reason\":") || api_json_text(&body,lmb_catalogue_capability_reason(m)) ||
            cap_str(&body,",\"weight_format\":") || api_json_text(&body,lmb_weight_format_name(m->weight_formats)) ||
            api_addf(&body,",\"checkpoint_operations\":%u",m->shape.sizing_verified ? m->shape.execution_ops : 0) ||
            api_addf(&body,",\"layers\":%u,\"checkpoint_bytes\":%llu,\"source_ready\":%s,\"decode_tok_s\":null}",
                m->shape.layers,(unsigned long long)m->checkpoint_bytes,
                m->weights_present && m->checkpoint_inventory_ok && m->shape.sizing_verified ? "true" : "false");
    }
    if (!bad) bad=cap_str(&body,"]}");
    if (bad) api_error(fd,503,"catalogue_unavailable"); else (void)api_response(fd,200,body.p);
    free(body.p); free(st);
}
static int api_workspace_node(Cap *body, const LmbMachineReport *r) {
    char id[65]; lmb_hex(id,r->identity,32);
    const LmbMachineProfile *m=&r->machine; const LmbResourceFacts *f=&r->facts;
    int bad=api_addf(body,"{\"id\":\"%s\",\"name\":",id) || api_json_text(body,m->hostname) ||
        cap_str(body,",\"cpu\":") || api_json_text(body,m->cpu_model) ||
        cap_str(body,",\"os\":") || api_json_text(body,m->os) ||
        cap_str(body,",\"arch\":") || api_json_text(body,m->arch) ||
        api_addf(body,",\"age_ms\":%u,\"threads\":%u,\"runtime_threads\":%u,\"ram_total_bytes\":%llu,"
            "\"ram_available_bytes\":%llu,\"ram_offered_bytes\":%llu,\"gpu_detected\":%u,\"vram_inventory_bytes\":%llu,"
            "\"gpu_execution_verified\":false,\"load_one\":",r->age_ms,m->logical_cpus,r->runtime_threads,
            (unsigned long long)m->ram_total_bytes,(unsigned long long)m->ram_available_bytes,
            (unsigned long long)r->ram_budget_bytes,m->gpu_count,(unsigned long long)m->vram_total_bytes);
    if (!bad) bad=f->known&LMB_FACT_LOAD ? api_addf(body,"%.3f",f->load_milli/1000.0) : cap_str(body,"null");
    if (!bad) bad=cap_str(body,",\"workload\":");
    const LmbWorkloadFacts *w=&r->workload;
    if (!bad) bad=w->known ? api_addf(body,"{\"allocations\":%u,\"reserved_bytes\":%llu,\"active\":%u,\"queued\":%u}",
        w->allocations,(unsigned long long)w->reserved_bytes,w->active,w->queued) : cap_str(body,"null");
    if (!bad) bad=cap_str(body,",\"machine_cost\":");
    if (!bad) bad=f->known&LMB_FACT_PRICE ? api_addf(body,
        "{\"state\":\"declared\",\"currency\":\"%.3s\",\"micro_units_per_hour\":%llu}",
        f->currency,(unsigned long long)f->price_micro_per_hour) : cap_str(body,"null");
    if (!bad) bad=cap_str(body,",\"power\":");
    if (!bad) bad=f->known&LMB_FACT_POWER ? api_addf(body,"{\"state\":\"declared_estimate\",\"watts\":%.3f}",
        f->power_milliwatts/1000.0) : cap_str(body,"null");
    return bad || cap_str(body,",\"energy_joules\":null}") ? -1 : 0;
}
/* Read-only snapshot of leased machine reports and saved plans. Managed model
 * routes, demand counters and matched speed observations are not kept by the
 * Windows gateway: they are reported as empty or unknown, never invented. */
static int api_workspace(int fd, const char *tracker) {
    LmbMachineReport *nodes=calloc(LMB_INVENTORY_MAX,sizeof *nodes);
    LmbResidentPlan *plans=calloc(64,sizeof *plans); Cap body={0}; uint32_t node_count=0;
    int bad=!nodes || !plans;
    if (!bad) {
        int inventory_ok=!lmb_inventory_fetch(tracker,nodes,&node_count);
        if (!inventory_ok) node_count=0;
        size_t plan_count=home_resident_library_list(tracker,plans,64);
        bad=api_addf(&body,"{\"schema\":1,\"captured_at\":%llu,\"inventory_ok\":%s,\"registry_ok\":true,"
            "\"inventory_ttl_ms\":%u,\"nodes\":[",(unsigned long long)time(NULL),inventory_ok ? "true" : "false",LMB_INVENTORY_TTL_MS);
        for (uint32_t i=0;i<node_count && !bad;i++) bad=(i && cap_str(&body,",")) || api_workspace_node(&body,&nodes[i]);
        if (!bad) bad=cap_str(&body,"],\"allocations\":[");
        for (size_t i=0;i<plan_count && !bad;i++) {
            const LmbResidentPlan *p=&plans[i]; char id[65]; lmb_hex(id,p->allocation,32);
            bad=api_addf(&body,"%s{\"id\":\"%s\",\"name\":",i ? "," : "",id) || api_json_text(&body,p->model) ||
                cap_str(&body,",\"adapter\":") || api_json_text(&body,p->observation.adapter) ||
                api_addf(&body,",\"state\":\"saved_plan\",\"context\":%u,\"session_limit\":%u,"
                    "\"preparation_seconds\":",p->context,p->sessions ? p->sessions : 1);
            if (!bad) bad=p->preparation_seconds>0 ? api_addf(&body,"%.6f",p->preparation_seconds) : cap_str(&body,"null");
            if (!bad) bad=cap_str(&body,",\"ranges\":[");
            for (uint32_t k=0;k<p->execution.count && !bad;k++) {
                int present=0;
                for (uint32_t n=0;n<node_count;n++) if (!memcmp(nodes[n].identity,p->peer_keys[k],32)) present=1;
                lmb_hex(id,p->peer_keys[k],32);
                bad=api_addf(&body,"%s{\"node\":\"%s\",\"begin\":%u,\"end\":%u,\"edge\":%s,"
                    "\"reserved_bytes\":%llu,\"report_present\":%s}",k ? "," : "",id,
                    p->execution.nodes[k].begin,p->execution.nodes[k].end,p->execution.nodes[k].edge ? "true" : "false",
                    (unsigned long long)p->execution.nodes[k].reserved_bytes,present ? "true" : "false");
            }
            if (!bad) bad=cap_str(&body,"],\"observation\":{\"state\":\"unknown\",\"decode_tok_s\":null}}");
        }
        if (!bad) bad=cap_str(&body,"],\"models\":[],\"demand\":null}\n");
    }
    if (!bad) bad=fd<0 ? fputs(body.p,stdout)==EOF : api_response(fd,200,body.p);
    else if (fd>=0) api_error(fd,503,"workspace_unavailable");
    free(nodes); free(plans); free(body.p); return bad ? -1 : 0;
}

/* ---- preparation ------------------------------------------------------
 * The page names an operation; the native keeper names its own incarnation.
 * One private record per operation ties the two together and remembers which
 * credential asked, so a repeated request observes instead of starting again
 * and a ready model is granted to the requester and to nobody else. The
 * keeper keeps only its latest journal: an older operation is reported as no
 * longer recorded, never as finished. */
#define LMB_PREPARATION_RECORD 137u
typedef struct { uint8_t instance[32],digest[64]; char user[33]; } ApiPreparationRecord;
static int api_preparation_dir(int access) { return lmb_chat_subdir(access,"preparations"); }
static int api_preparation_load(int dir, const char *operation, ApiPreparationRecord *record) {
    char name[80]; snprintf(name,sizeof name,"%s.op",operation);
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    uint8_t bytes[LMB_PREPARATION_RECORD]; struct stat st;
    int bad=lmb_api_private_fd(fd,0) || fstat(fd,&st) || st.st_size!=sizeof bytes ||
        lmb_read_full(fd,bytes,sizeof bytes) || memcmp(bytes,"LMBWPOP1",8);
    close(fd);
    if (!bad) {
        memcpy(record->instance,bytes+8,32); memcpy(record->digest,bytes+40,64);
        memcpy(record->user,bytes+104,33); record->user[32]=0;
    }
    lmb_api_wipe(bytes,sizeof bytes); return bad ? -1 : 0;
}
static int api_preparation_store(int dir, const char *operation, const uint8_t instance[32], const LmbApiUser *user) {
    char name[80]; snprintf(name,sizeof name,"%s.op",operation);
    uint8_t bytes[LMB_PREPARATION_RECORD]={0};
    memcpy(bytes,"LMBWPOP1",8); memcpy(bytes+8,instance,32); memcpy(bytes+40,user->digest,64);
    memcpy(bytes+104,user->name,strnlen(user->name,32));
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    int bad=fd<0 || lmb_api_private_fd(fd,0) || write(fd,bytes,sizeof bytes)!=(int)sizeof bytes || fsync(fd);
    if (fd>=0 && close(fd)) bad=1;
    if (bad && fd>=0) (void)unlinkat(dir,name,0);
    lmb_api_wipe(bytes,sizeof bytes); return bad ? -1 : 0;
}
/* 0: `s` is this operation's keeper state and *live says whether the keeper
 * still answers. 1: the keeper has since recorded another operation. */
static int api_preparation_observe(const ApiPreparationRecord *record, HomeServiceSnapshot *s, int *live) {
    *live=!home_service_query("prepare",HOME_SVC_STATUS,NULL,s) && !memcmp(s->instance,record->instance,32);
    if (*live) return 0;
    return home_service_record("prepare",s) || memcmp(s->instance,record->instance,32) ? 1 : 0;
}
static int api_preparation_results(Cap *body, const HomeServiceSnapshot *s) {
    if (api_addf(body,",\"results_known\":%s,\"allocations\":[",s->preparation_results_known ? "true" : "false")) return -1;
    for (uint32_t i=0;i<s->preparation_result_count;i++) {
        const HomePreparationResult *r=&s->preparation_results[i]; char id[65]; lmb_hex(id,r->allocation,32);
        if (api_addf(body,"%s{\"id\":\"%s\",\"name\":",i ? "," : "",id) ||
            api_json_text(body,r->catalogue_name) || api_addf(body,",\"state\":\"%s\"}",
                r->ready_observed ? "ready_observed" : "unconfirmed")) return -1;
    }
    return cap_str(body,"],\"result_scope\":\"historical_preparation_not_live_readiness\"");
}
enum { API_PREPARATION_GRANT_NONE=0,API_PREPARATION_GRANTED,API_PREPARATION_GRANT_FULL,
       API_PREPARATION_GRANT_FAILED,API_PREPARATION_GRANT_OTHER };
/* Grants what is ready to the credential that asked for it, when that same
 * credential observes it and still holds management. Unlike POSIX there is no
 * keeper-side observer: the grant is written when the requester looks, or by
 * the next `lumabri start` for the local sign-in. */
static int api_preparation_grant(int access, const ApiPreparationRecord *record, const HomeServiceSnapshot *s,
                                 const LmbApiUser *caller) {
    if (strcmp(caller->name,record->user) || memcmp(caller->digest,record->digest,64) ||
        !lmb_management_allows(access,caller)) return API_PREPARATION_GRANT_OTHER;
    int outcome=API_PREPARATION_GRANT_NONE;
    for (uint32_t i=0;i<s->preparation_result_count;i++) {
        const HomePreparationResult *r=&s->preparation_results[i];
        if (!r->ready_observed) continue;
        if (lmb_api_user_allows(caller,r->allocation)) { if (!outcome) outcome=API_PREPARATION_GRANTED; continue; }
        int lock=api_access_wait_lock(access),rc=-1;
        if (lock>=0) {
            /* Decide again under the lock: the credential may have changed. */
            LmbApiUser current;
            if (!lmb_api_user_load(access,caller->name,&current) && !memcmp(current.digest,record->digest,64) &&
                lmb_management_allows(access,&current))
                rc=api_grant_allocation(access,s->tracker,caller->name,r->allocation);
            lmb_api_wipe(&current,sizeof current); close(lock);
        }
        char allocation[65]; lmb_hex(allocation,r->allocation,32);
        fprintf(stderr,"[preparation-grant] user=%s allocation=%s result=%s\n",caller->name,allocation,
            !rc ? "granted" : rc>0 ? "grant_list_full" : "failed");
        if (rc>0) outcome=API_PREPARATION_GRANT_FULL;
        else if (rc<0) { if (outcome!=API_PREPARATION_GRANT_FULL) outcome=API_PREPARATION_GRANT_FAILED; }
        else if (!outcome) outcome=API_PREPARATION_GRANTED;
    }
    return outcome;
}
static int api_preparation_access(Cap *body, int outcome) {
    static const char *const states[]={"none_ready","granted","grant_list_full","not_granted","not_requested_by_this_sign_in"};
    static const char *const details[]={
        "No model of this operation is ready yet.",
        "Ready models of this operation are in your chat model list.",
        "The model is ready, but your sign-in already holds the most model grants one API user can hold, so it was "
            "not added to your chat models. Grant it to another API user: lumabri api grant NAME ALLOCATION_ID.",
        "The model is ready, but it could not be added to your chat models now. Check this operation again, or run: "
            "lumabri api grant NAME ALLOCATION_ID.",
        "This operation was requested by another sign-in. Observing it grants nothing."};
    return api_addf(body,",\"chat_access\":\"%s\",\"chat_access_detail\":\"%s\"",states[outcome],details[outcome]);
}
static int api_preparation_status(int dir, int access, const LmbApiUser *caller, const char *tracker,
                                  const char *operation, int cancel, Cap *body) {
    ApiPreparationRecord record; HomeServiceSnapshot saved,current; int live=0;
    int found=api_preparation_load(dir,operation,&record);
    if (found) return found>0 ? 404 : 503;
    if (api_preparation_observe(&record,&saved,&live)) {
        return api_addf(body,"{\"schema\":1,\"operation\":\"%s\",\"state\":\"interrupted_or_starting\",\"live\":false,"
            "\"cancel_requested\":false,\"detail\":\"This computer has since recorded another preparation, so the outcome of "
            "this one is no longer kept. Check the model list before preparing again.\",\"results_known\":false,"
            "\"allocations\":[],\"result_scope\":\"historical_preparation_not_live_readiness\","
            "\"chat_access\":\"none_ready\",\"chat_access_detail\":\"No model of this operation is ready yet.\","
            "\"approval_replayed\":false}",operation) ? 500 : 200;
    }
    if (strcmp(saved.tracker,tracker)) return 409;
    if (cancel && live && saved.state==HOME_SVC_RUNNING) {
        if (home_service_query("prepare",HOME_SVC_CANCEL,&saved,&current)) return 409;
        saved=current;
    }
    const char *state=saved.state==HOME_SVC_DONE ? "done" : saved.state==HOME_SVC_FAILED ? "failed" :
        live ? "running" : "interrupted_or_starting";
    if (api_addf(body,"{\"schema\":1,\"operation\":\"%s\",\"state\":\"%s\",\"live\":%s,"
        "\"cancel_requested\":%s,\"detail\":",operation,state,live ? "true" : "false",cancel && live ? "true" : "false") ||
        api_json_text(body,saved.detail) || api_preparation_results(body,&saved) ||
        api_preparation_access(body,api_preparation_grant(access,&record,&saved,caller)) ||
        cap_str(body,",\"approval_replayed\":false}")) return 500;
    return 200;
}
/* The reviewed plan as the page draws it, and the native review fingerprint
 * the keeper recomputes before it asks any donor for anything. */
static int api_preparation_review(LmbTuiState *st, const LmbPreparationRequest *r, Cap *body, char review[65]) {
    memset(st->selected_models,0,sizeof st->selected_models);
    if (r->model_count>LMB_PORTFOLIO_MODELS) return 422;
    for (uint32_t i=0;i<r->model_count;i++) {
        int found=-1; unsigned matches=0;
        for (int k=0;k<st->nmodels;k++) if (!strcmp(r->models[i],st->models[k].name)) { matches++; found=k; }
        if (matches!=1 || strlen(st->models[found].dir)>=sizeof st->selected_models[0]) return 422;
        strcpy(st->selected_models[i],st->models[found].dir);
    }
    catalog_joint_refresh(st);
    if (!st->joint.ready || !st->build_id[0] || st->joint.plan.model_count!=r->model_count ||
        home_review_selection_windows(st,-1,review)) return 422;
    if (api_addf(body,"{\"schema\":1,\"mode\":\"resident_segment\",\"requires_approval\":true,"
        "\"decode_tok_s\":null,\"context\":%u,\"sessions\":%u,\"max_new\":%u,\"build_id\":\"%s\",\"tracker\":",
        st->context,st->sessions,st->max_new,st->build_id) || api_json_text(body,st->tracker) || cap_str(body,",\"nodes\":[")) return 500;
    for (uint32_t i=0;i<st->joint.node_count;i++) {
        uint32_t n=st->joint.node_indices[i]; char hardware[65],set[65];
        catalog_hardware_id(&st->profiles[n],st->nodes[n].addr,hardware);
        lmb_hex(set,st->workloads[n].allocation_set,32);
        if (!st->runtime_ids[n][0] || !st->workloads[n].known) return 422;
        if (api_addf(body,"%s{\"id\":\"%s\",\"runtime\":\"%s\",\"hardware\":\"%s\",\"allocation_set\":\"%s\","
            "\"threads\":%u,\"existing_allocations\":%u,\"reserved_bytes\":%llu}",i ? "," : "",st->identities[n],
            st->runtime_ids[n],hardware,set,st->nodes[n].threads,st->workloads[n].allocations,
            (unsigned long long)st->workloads[n].reserved_bytes)) return 500;
    }
    if (cap_str(body,"],\"models\":[")) return 500;
    for (uint32_t i=0;i<st->joint.plan.model_count;i++) {
        const LmbTuiModel *m=&st->models[st->joint.model_indices[i]];
        const LmbClusterPlan *p=&st->joint.plan.plans[i];
        if ((i && cap_str(body,",")) || cap_str(body,"{\"name\":") || api_json_text(body,m->name) ||
            api_addf(body,",\"source_stat_fence\":null,\"checkpoint_bytes\":%llu,\"edge_node\":%u,\"slices\":[",
                (unsigned long long)m->checkpoint_bytes,p->edge_node)) return 500;
        for (uint32_t k=0;k<p->nslices;k++) {
            const LmbSlice *s=&p->slices[k];
            if (api_addf(body,"%s{\"node\":%u,\"begin\":%u,\"end\":%u,\"reserved_bytes\":%llu}",k ? "," : "",
                s->node,s->layer_begin,s->layer_end,(unsigned long long)s->bytes_resident)) return 500;
        }
        if (cap_str(body,"]}")) return 500;
    }
    return cap_str(body,"]}") ? 500 : 200;
}
static int api_preparation_execute(int access, const char *tracker, const LmbApiUser *caller,
                                   const LmbPreparationRequest *r, Cap *result) {
    Cap plan={0}; LmbTuiState *st=NULL; int status=400,dir=-1,lock=-1; char review[65];
    int start=!strcmp(r->action,"start"), preview=!strcmp(r->action,"preview");
    if (!preview) {
        dir=api_preparation_dir(access); if (dir<0) { status=503; goto done; }
        if (!start) { status=api_preparation_status(dir,access,caller,tracker,r->operation,!strcmp(r->action,"cancel"),result); goto done; }
        lock=lmb_api_access_lock(dir); if (lock<0) { status=409; goto done; }
        /* An accepted operation is immutable: repeating it only observes. */
        ApiPreparationRecord existing; int found=api_preparation_load(dir,r->operation,&existing);
        if (found<0) { status=409; goto done; }
        if (!found) { status=api_preparation_status(dir,access,caller,tracker,r->operation,0,result); goto done; }
    }
    st=api_catalogue_state(tracker,r); if (!st) { status=503; goto done; }
    status=api_preparation_review(st,r,&plan,review); if (status!=200) goto done;
    if (preview) {
        if (api_addf(result,"{\"schema\":1,\"review\":\"%s\",\"plan\":",review) ||
            cap_add(result,plan.p,plan.len) || cap_str(result,"}")) status=500;
        goto done;
    }
    if (strcmp(review,r->review)) { status=409; goto done; }
    HomeServiceSnapshot started;
    if (home_prepare_start_windows(st,-1,&started)) { status=errno==EBUSY ? 423 : 503; goto done; }
    if (api_preparation_store(dir,r->operation,started.instance,caller)) {
        /* An operation nobody can observe by its name must not keep running. */
        HomeServiceSnapshot reply; (void)home_service_query("prepare",HOME_SVC_CANCEL,&started,&reply);
        status=503; goto done;
    }
    status=api_addf(result,"{\"schema\":1,\"operation\":\"%s\",\"state\":\"starting\",\"requires_approval\":true}",r->operation) ? 500 : 202;
done:
    if (lock>=0) close(lock);
    if (dir>=0) close(dir);
    free(st); free(plan.p); return status;
}
static void api_preparation(int fd, int access, const char *tracker, const LmbApiUser *user, const char *text, size_t size) {
    LmbPreparationRequest *request=calloc(1,sizeof *request); Cap result={0};
    int status=!request ? 500 : lmb_preparation_parse(text,size,request) ? 400 :
        api_preparation_execute(access,tracker,user,request,&result);
    if (status==200 || status==202) (void)api_response(fd,(unsigned)status,result.p);
    else api_error(fd,status==423 ? 409 : (unsigned)status,status==400 ? "invalid_preparation_request" :
        status==404 ? "preparation_not_found" : status==409 ? "review_or_operation_changed" :
        status==423 ? "another_preparation_is_running" : status==422 ? "resident_plan_unavailable" :
        status==500 ? "allocation_failed" : "preparation_not_confirmed_inspect_before_retry");
    free(result.p); free(request);
}
typedef struct { int dir,bad; unsigned count; const char *tracker; Cap *body; } ApiPreparationListing;
static int api_preparation_list_entry(const char *name, void *context) {
    ApiPreparationListing *l=context; char operation[65]; ApiPreparationRecord record; HomeServiceSnapshot s; int live=0;
    if (strlen(name)!=67 || strcmp(name+64,".op") || l->count>=64) return 0;
    memcpy(operation,name,64); operation[64]=0;
    if (!lmb_preparation_hex(operation)) return 0;
    if (api_preparation_load(l->dir,operation,&record)) { l->bad=1; return 1; }
    int known=!api_preparation_observe(&record,&s,&live);
    if (known && strcmp(s.tracker,l->tracker)) return 0;
    struct stat st; if (fstatat(l->dir,name,&st,AT_SYMLINK_NOFOLLOW)) { l->bad=1; return 1; }
    l->bad=api_addf(l->body,"%s{\"operation\":\"%s\",\"updated_at\":%llu,\"recorded_state\":\"%s\",\"detail\":",
            l->count++ ? "," : "",operation,(unsigned long long)st.st_mtime,
            !known ? "no_longer_recorded" : s.state==HOME_SVC_DONE ? "done" : s.state==HOME_SVC_FAILED ? "failed" : "requires_live_check") ||
        api_json_text(l->body,known ? s.detail : "A later preparation replaced this record on this computer.") ||
        (known ? api_preparation_results(l->body,&s) : cap_str(l->body,",\"results_known\":false,\"allocations\":[],"
            "\"result_scope\":\"historical_preparation_not_live_readiness\"")) || cap_str(l->body,"}");
    return l->bad;
}
static void api_preparation_list(int fd, int access, const char *tracker) {
    int dir=api_preparation_dir(access); Cap body={0};
    ApiPreparationListing listing={dir,0,0,tracker,&body};
    int bad=dir<0 || cap_str(&body,"{\"schema\":1,\"operations\":[") ||
        lmb_private_dir_each(dir,api_preparation_list_entry,&listing)<0 || listing.bad ||
        cap_str(&body,"],\"next_cursor\":\"\",\"order\":\"operation_id\",\"page_limit\":64,"
            "\"history_policy\":\"operations_started_from_this_page_latest_outcome_only\"}");
    if (bad) api_error(fd,503,"preparation_history_unavailable"); else (void)api_response(fd,200,body.p);
    if (dir>=0) close(dir);
    free(body.p);
}

/* ---- one connection --------------------------------------------------- */
static void api_connection_dispatch(int access_dir, unsigned port, const char *tracker) {
    const int fd=0; DWORD timeout=5000;
    (void)setsockopt(api_client,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof timeout);
    (void)setsockopt(api_client,SOL_SOCKET,SO_SNDTIMEO,(const char *)&timeout,sizeof timeout);
    char header[LMB_API_HEADER_MAX]; size_t length=0; int complete=0;
    uint64_t deadline=lmb_io_monotonic_ms()+5000;
    while (length<sizeof header && lmb_io_monotonic_ms()<deadline) {
        if (recv(api_client,header+length,1,0)!=1) break;
        if (++length>=4 && !memcmp(header+length-4,"\r\n\r\n",4)) { complete=1; break; }
    }
    LmbApiHttp request;
    if (!complete || lmb_api_http_parse(&request,header,length)) { api_error(fd,400,"invalid_http_request"); return; }
    if (lmb_api_http_origin(&request,port)) { api_error(fd,403,"origin_or_host_rejected"); return; }
    if (api_web(fd,&request)) return;
    if (request.api_key[0]) { api_error(fd,400,"ambiguous_or_unsupported_credentials"); return; }
    LmbApiUser user;
    if (lmb_api_authorize(access_dir,request.authorization,&user)) { api_error(fd,401,"authentication_required"); return; }
    lmb_api_wipe(header,sizeof header); lmb_api_wipe(request.authorization,sizeof request.authorization);
    if (api_windows_missing(request.path)) { api_error(fd,501,LMB_API_UNAVAILABLE); return; }
    int permit=lmb_api_user_permit(access_dir,user.name,2);
    if (permit<0) { api_error(fd,429,"user_request_limit"); return; }
    const char *history_path="/api/v1/conversations";
    size_t history_prefix=strlen(history_path);
    int history=!strncmp(request.path,history_path,history_prefix) &&
        (!request.path[history_prefix] || request.path[history_prefix]=='/');
    int preparation=!strcmp(request.path,"/api/v1/preparation");
    int approval=!strcmp(request.path,"/api/v1/donor/approval");
    int chat=!strcmp(request.path,"/api/v1/chat");
    int management=preparation || approval;
    int reading=!strcmp(request.method,"GET") && !request.length;
    int operator=lmb_operator_allows(access_dir,&user), manager=lmb_management_allows(access_dir,&user);
    if (management && !strcmp(request.method,"POST") && !manager) api_error(fd,403,"management_permission_required");
    else if (reading && !strcmp(request.path,"/api/v1/session")) {
        Cap body={0};
        if (api_addf(&body,"{\"schema\":1,\"operator\":%s,\"management\":%s,\"platform\":\"windows\",\"unavailable\":",
                operator ? "true" : "false",manager ? "true" : "false") ||
            cap_str(&body,api_windows_unavailable) || cap_str(&body,"}")) api_error(fd,500,"allocation_failed");
        else (void)api_response(fd,200,body.p);
        free(body.p);
    }
    else if (reading && !strcmp(request.path,"/api/v1/models")) api_models(fd,tracker,&user);
    else if (reading && history) api_history(fd,access_dir,&user,&request,NULL,0);
    else if (reading && (approval || preparation || !strncmp(request.path,"/api/v1/preparation/after/",26))) {
        /* Reading a request is part of deciding it: same capability as POST. */
        if (!manager) api_error(fd,403,"management_permission_required");
        else {
            EnterCriticalSection(&api_household);
            if (approval) api_donor_approval(fd,tracker,0,NULL,0);
            else api_preparation_list(fd,access_dir,tracker);
            LeaveCriticalSection(&api_household);
        }
    }
    else if (reading && (!strcmp(request.path,"/api/v1/catalogue") || !strcmp(request.path,"/api/v1/workspace"))) {
        if (!operator) api_error(fd,403,"operator_permission_required");
        else {
            EnterCriticalSection(&api_household);
            if (request.path[8]=='c') api_catalogue(fd,tracker); else (void)api_workspace(fd,tracker);
            LeaveCriticalSection(&api_household);
        }
    }
    else if (management && request.length>(preparation ? 8192u : 512u)) api_error(fd,413,"management_request_too_large");
    else if (((!strcmp(request.method,"POST") && (chat || management)) ||
              (history && (!strcmp(request.method,"POST") || !strcmp(request.method,"DELETE")))) &&
             request.has_length && request.length && request.json) {
        char *body=malloc((size_t)request.length+1); size_t at=0;
        deadline=lmb_io_monotonic_ms()+5000;
        if (!body) api_error(fd,500,"allocation_failed");
        else {
            while (at<request.length && lmb_io_monotonic_ms()<deadline) {
                int got=recv(api_client,body+at,(int)(request.length-at),0);
                if (got<=0) break;
                at+=(size_t)got;
            }
            body[at]=0;
            if (at!=request.length) api_error(fd,408,"request_body_timeout");
            else if (history) api_history(fd,access_dir,&user,&request,body,at);
            else if (chat) api_chat(fd,tracker,&user,body,at);
            else if (approval && strstr(body,"_repeat")) api_error(fd,501,LMB_API_UNAVAILABLE);
            else {
                EnterCriticalSection(&api_household);
                if (preparation) api_preparation(fd,access_dir,tracker,&user,body,at);
                else api_donor_approval(fd,tracker,0,body,at);
                LeaveCriticalSection(&api_household);
            }
            lmb_api_wipe(body,(size_t)request.length+1); free(body);
        }
    } else api_error(fd,400,"unsupported_endpoint_or_framing");
    close(permit);
}
typedef struct { LmbSocket client; int access_dir; unsigned port; char tracker[256]; } ApiWindowsConnection;
/* An early rejection may leave an already-sent request body unread. Closing
 * both directions then can reset the connection and erase the response at
 * the client. Finish our response first; discard, never parse, a bounded
 * remainder until the peer closes. No keep-alive, no pipelining. */
static DWORD WINAPI api_connection_thread(void *opaque) {
    ApiWindowsConnection *c=opaque;
    api_client=c->client;
    api_connection_dispatch(c->access_dir,c->port,c->tracker);
    if (!shutdown(c->client,SD_SEND)) {
        uint64_t started=GetTickCount64(); size_t discarded=0;
        while (discarded<LMB_API_BODY_MAX) {
            uint64_t now=GetTickCount64();
            if (now-started>=100) break;
            fd_set readable; FD_ZERO(&readable); FD_SET(c->client,&readable);
            struct timeval wait={0,(long)(100-(now-started))*1000};
            if (select(0,&readable,NULL,NULL,&wait)<=0) break;
            char bytes[4096]; int n=recv(c->client,bytes,sizeof bytes,0);
            if (n<=0) break;
            discarded+=(size_t)n;
        }
    }
    closesocket(c->client); api_client=LMB_INVALID_SOCKET; free(c); return 0;
}
/* Loopback only, and exclusively: on Windows SO_REUSEADDR would let another
 * program bind the same port and receive the page's bearer tokens. */
static LmbSocket api_listen_windows(unsigned port) {
    if (lmb_socket_init()) return LMB_INVALID_SOCKET;
    LmbSocket listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (listener==LMB_INVALID_SOCKET) return LMB_INVALID_SOCKET;
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons((uint16_t)port);
    if (lmb_socket_bind_policy(listener) || lmb_socket_noinherit(listener) ||
        bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,16)) {
        closesocket(listener); return LMB_INVALID_SOCKET;
    }
    return listener;
}
static int api_serve_windows(LmbSocket listener, int access_dir, unsigned port, const char *tracker, HomeService *keeper) {
    static LONG prepared;
    if (!InterlockedExchange(&prepared,1)) InitializeCriticalSection(&api_household);
    HANDLE workers[LMB_API_CHILDREN]={0}; api_stopping=0;
    printf("[api] http://127.0.0.1:%u · bearer authentication · approved resident models only\n",port);
    fflush(stdout);
    while (!g_stopping) {
        if (keeper) {
            int op=home_service_poll(keeper);
            if (op==HOME_SVC_STOP) break;
            home_service_answer(keeper);
        }
        for (unsigned i=0;i<LMB_API_CHILDREN;i++)
            if (workers[i] && WaitForSingleObject(workers[i],0)==WAIT_OBJECT_0) { CloseHandle(workers[i]); workers[i]=NULL; }
        fd_set readable; FD_ZERO(&readable); FD_SET(listener,&readable);
        struct timeval wait={0,100000};
        int ready=select(0,&readable,NULL,NULL,&wait);
        if (ready<0) break;
        if (!ready) continue;
        LmbSocket client=accept(listener,NULL,NULL);
        if (client==LMB_INVALID_SOCKET) continue;
        unsigned slot=0; while (slot<LMB_API_CHILDREN && workers[slot]) slot++;
        ApiWindowsConnection *c=slot<LMB_API_CHILDREN ? calloc(1,sizeof *c) : NULL;
        if (!c || lmb_socket_noinherit(client)) { closesocket(client); free(c); continue; }
        c->client=client; c->access_dir=access_dir; c->port=port; snprintf(c->tracker,sizeof c->tracker,"%s",tracker);
        workers[slot]=CreateThread(NULL,1u<<20,api_connection_thread,c,0,NULL);
        if (!workers[slot]) { closesocket(client); free(c); }
    }
    closesocket(listener);
    /* Unfinished turns see the flag and close their host connection. */
    InterlockedExchange(&api_stopping,1);
    for (unsigned i=0;i<LMB_API_CHILDREN;i++) if (workers[i]) {
        (void)WaitForSingleObject(workers[i],10000); CloseHandle(workers[i]);
    }
    return 0;
}

/* ---- keeper and commands ---------------------------------------------- */
static int api_household_activate_windows(void) {
    const char *token=getenv("LUMABRI_TOKEN");
    if (!token || !*token) {
        HomeSettings settings;
        if (home_settings_load_windows(&settings)) return home_fail("Cannot load private household settings.");
        int bad=home_settings_activate_windows(&settings); SecureZeroMemory(&settings,sizeof settings);
        if (bad) return home_fail("Cannot activate the saved household credentials.");
    }
    if (_putenv_s("LUMABRI_ENCRYPT","1") || lmb_secure_init() || !lmb_secure_enabled())
        return home_fail("Cannot activate encrypted household access.");
    return 0;
}
static int api_access_open_windows(void) {
    char path[1200];
    int dir=api_access_path_windows(path,sizeof path) ? -1 : lmb_api_access_dir(path,1);
    if (dir<0) fprintf(stderr,"Cannot open private API access directory\n");
    return dir;
}
/* The supervised keeper behind `api start` and `lumabri start`. */
static int home_api_entry_windows(int argc, char **argv) {
    if (argc!=4 || strcmp(argv[0],"--port") || strcmp(argv[2],"--tracker") || !getenv("LUMABRI_READY_HANDLE")) return 2;
    char *end; unsigned long port=strtoul(argv[1],&end,10);
    if (!*argv[1] || *end || !port || port>65535 || !*argv[3] ||
        strlen(argv[3])>=sizeof ((HomeServiceSnapshot *)0)->tracker) return 2;
    if (api_household_activate_windows()) return 1;
    HomeService keeper;
    if (home_service_open(&keeper,"api")) return errno==EBUSY ? 75 : 1;
    snprintf(keeper.snapshot.name,sizeof keeper.snapshot.name,"http://127.0.0.1:%lu",port);
    snprintf(keeper.snapshot.tracker,sizeof keeper.snapshot.tracker,"%s",argv[3]);
    int access=api_access_open_windows(), rc=1;
    LmbSocket listener=access<0 ? LMB_INVALID_SOCKET : api_listen_windows((unsigned)port);
    if (listener==LMB_INVALID_SOCKET)
        fprintf(stderr,"[api] Cannot listen on 127.0.0.1:%lu or open the private access directory\n",port);
    else {
        keeper.snapshot.phase=2; /* API role: 1 starting, 2 listening */
        snprintf(keeper.snapshot.detail,sizeof keeper.snapshot.detail,"Listening on %s; approved resident models only.",keeper.snapshot.name);
        if (home_service_save(&keeper) || lmb_ready_notify()) closesocket(listener);
        else rc=api_serve_windows(listener,access,(unsigned)port,argv[3],&keeper);
    }
    if (access>=0) close(access);
    keeper.snapshot.state=rc ? HOME_SVC_FAILED : HOME_SVC_STOPPED; keeper.snapshot.phase=0;
    snprintf(keeper.snapshot.detail,sizeof keeper.snapshot.detail,"API %s; resident models were not unloaded.",rc ? "failed" : "stopped");
    (void)home_service_save(&keeper); home_service_answer(&keeper); home_service_close(&keeper);
    return rc;
}
/* 0: this household's page is being served on this port. A live keeper is
 * adopted, never replaced. */
static int api_start_windows(unsigned port, const char *tracker) {
    if (strlen(tracker)>=sizeof ((HomeServiceSnapshot *)0)->tracker) return 1;
    char endpoint[64],port_text[16]; snprintf(endpoint,sizeof endpoint,"http://127.0.0.1:%u",port);
    snprintf(port_text,sizeof port_text,"%u",port);
    HomeServiceSnapshot observed;
    if (!home_service_query("api",HOME_SVC_STATUS,NULL,&observed)) {
        if (strcmp(observed.tracker,tracker) || strcmp(observed.name,endpoint)) {
            fprintf(stderr,"Another API configuration is active; stop it explicitly before changing the household or port\n"); return 1;
        }
        return observed.phase==2 ? 0 : 1;
    }
    if (home_service_foreground() || home_service_ensure()) {
        fprintf(stderr,"Cannot start the API keeper; use api serve for foreground operation\n"); return 1;
    }
    const char *args[]={"--internal-api","--port",port_text,"--tracker",tracker};
    if (home_service_keeper_start_windows("api",args,5)) {
        fprintf(stderr,"API did not become ready; inspect lumabri service status and api.log\n"); return 1;
    }
    for (unsigned i=0; i<60; i++) {
        if (!home_service_query("api",HOME_SVC_STATUS,NULL,&observed) && observed.phase==2 &&
            !strcmp(observed.tracker,tracker) && !strcmp(observed.name,endpoint)) return 0;
        home_service_wait_ms(50);
    }
    fprintf(stderr,"API did not become ready; inspect lumabri service status and api.log\n"); return 1;
}
static int cmd_api_windows(int argc, char **argv) {
    if (!argc) goto usage;
    HomeSettings settings;
    if (home_settings_load_windows(&settings)) return home_fail("Cannot load private household settings.");
    char tracker[256]; snprintf(tracker,sizeof tracker,"%s",settings.tracker);
    SecureZeroMemory(&settings,sizeof settings);
    unsigned port=47380; char *args[4]={0}; unsigned count=0;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--tracker") && i+1<argc) {
            if (strlen(argv[++i])>=sizeof tracker) goto usage;
            strcpy(tracker,argv[i]);
        } else if (!strcmp(argv[i],"--port") && i+1<argc) {
            char *end; unsigned long value=strtoul(argv[++i],&end,10);
            if (!*argv[i] || *end || !value || value>65535) goto usage;
            port=(unsigned)value;
        } else if (count<sizeof args/sizeof *args) args[count++]=argv[i]; else goto usage;
    }
    if (!strcmp(argv[0],"stop") && !count) return home_service_stop_role("api") ? 1 : 0;
    if ((!strcmp(argv[0],"serve") || !strcmp(argv[0],"start")) && !count) {
        if (!*tracker) return home_fail("Create or join a household first, or pass --tracker HOST:PORT.");
        if (api_household_activate_windows()) return 1;
        if (!strcmp(argv[0],"start")) {
            if (api_start_windows(port,tracker)) return 1;
            printf("http://127.0.0.1:%u\n",port); return 0;
        }
        int access=api_access_open_windows(); if (access<0) return 1;
        LmbSocket listener=api_listen_windows(port);
        if (listener==LMB_INVALID_SOCKET) { close(access); return home_fail("[api] Cannot listen on 127.0.0.1:%u",port); }
        int rc=api_serve_windows(listener,access,port,tracker,NULL);
        close(access); return rc;
    }
    int dir=api_access_open_windows(); if (dir<0) return 1;
    if (!strcmp(argv[0],"list") && !count && *tracker) {
        int rc=api_models(-1,tracker,NULL); close(dir); return rc ? 1 : 0;
    }
    int lock=lmb_api_access_lock(dir),rc=1;
    if (lock<0) { close(dir); fprintf(stderr,"API access database is busy or unsafe\n"); return 1; }
    if (!strcmp(argv[0],"user-add") && count==1) {
        char token[98]; rc=lmb_api_user_create(dir,args[0],token);
        if (!rc) { puts(token); lmb_api_wipe(token,sizeof token); }
    } else if ((!strcmp(argv[0],"operator-grant") || !strcmp(argv[0],"operator-revoke")) && count==1) {
        LmbApiUser user;
        if (!lmb_api_user_load(dir,args[0],&user)) rc=lmb_operator_set(dir,&user,!strcmp(argv[0],"operator-grant"));
    } else if ((!strcmp(argv[0],"manage-grant") || !strcmp(argv[0],"manage-revoke")) && count==1) {
        LmbApiUser user;
        if (!lmb_api_user_load(dir,args[0],&user)) rc=lmb_management_set(dir,&user,!strcmp(argv[0],"manage-grant"));
    } else if (!strcmp(argv[0],"revoke") && count==1 && lmb_api_username(args[0])) {
        char file[40]; snprintf(file,sizeof file,"%s.user",args[0]); rc=unlinkat(dir,file,0);
        if (!rc) rc=fsync(dir);
    } else if (!strcmp(argv[0],"grant") && count==2 && *tracker) {
        uint8_t allocation[32];
        if (strlen(args[1])==64 && !lmb_unhex(allocation,args[1],32)) rc=api_grant_allocation(dir,tracker,args[0],allocation);
        if (rc>0) fprintf(stderr,"%s already holds %u model grants, the most one API user can hold. Nothing was changed; "
            "grant this model to another API user.\n",args[0],LMB_API_MODELS_MAX);
    } else { close(lock); close(dir); goto usage; }
    close(lock); close(dir);
    if (rc) fprintf(stderr,"API access operation failed; check the user and approved allocation\n");
    return rc ? 1 : 0;
usage:
    fprintf(stderr,"lumabri api list [--tracker HOST:PORT]\n"
        "lumabri api user-add NAME\n"
        "lumabri api operator-grant NAME    read-only cluster visibility, no inference grant\n"
        "lumabri api operator-revoke NAME\n"
        "lumabri api manage-grant NAME      preparation, donor approval and visibility; inference only on models it prepares\n"
        "lumabri api manage-revoke NAME\n"
        "lumabri api grant NAME ALLOCATION_ID [--tracker HOST:PORT]\n"
        "lumabri api revoke NAME\n"
        "lumabri api start [--tracker HOST:PORT] [--port 47380]\n"
        "lumabri api stop\n"
        "lumabri api serve [--tracker HOST:PORT] [--port 47380]   foreground diagnostics\n"
        "Managed model routes, capacity, policies, replica control and --https-origin are not available on Windows yet.\n");
    return 2;
}
#endif
