/* Authenticated loopback gateway to ALREADY approved resident allocations.
 * One bounded child per HTTP request. No model loading, donor approval,
 * endpoint supplied by clients or shared Engine globals. Private transcripts
 * are separate from model authority and engine conversation state.
 * Requires the controller's Engine/template and resident-plan helpers. */
#ifndef LUMABRI_API_H
#define LUMABRI_API_H
#include "lumabri_api_http.h"
#include "lumabri_api_json.h"
#include "lumabri_api_access.h"
#include "lumabri_resident_control.h"
#include "build/web_assets.h"

#define LMB_API_CHILDREN 16u
#define LMB_API_TURN_SECONDS 300u
static volatile sig_atomic_t api_stopping;
static void api_stop(int sig) { (void)sig; api_stopping=1; }

static int api_write(int fd, const void *data, size_t n) {
    const unsigned char *p=data;
    while (n) {
        ssize_t sent=send(fd,p,n,0);
        if (sent<0 && errno==EINTR && !api_stopping) continue;
        if (sent<=0) return -1;
        p+=sent; n-=(size_t)sent;
    }
    return 0;
}
static int api_response(int fd, unsigned status, const char *body) {
    char header[512];
    int n=snprintf(header,sizeof header,"HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n",status,status==200 ? "OK" : "Request failed",strlen(body));
    return n<0 || (size_t)n>=sizeof header || api_write(fd,header,(size_t)n) || api_write(fd,body,strlen(body)) ? -1 : 0;
}
static void api_error(int fd, unsigned status, const char *code) {
    /* code is a server-owned constant, never an unescaped engine/client string. */
    char body[192]; snprintf(body,sizeof body,"{\"error\":\"%s\"}\n",code);
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
#include "lumabri_chat_history.h"

/* Only these compiled assets are public. No generic file-serving path and no
 * redirects, external scripts, cookies, CORS or browser-persisted credentials. */
static int api_web(int fd, const LmbApiHttp *request) {
    if (strcmp(request->method,"GET") || request->length) return 0;
    const unsigned char *data=NULL; size_t size=0; const char *type=NULL;
    if (!strcmp(request->path,"/")) { data=lmb_web_html; size=sizeof lmb_web_html-1; type="text/html; charset=utf-8"; }
    else if (!strcmp(request->path,"/app.css")) { data=lmb_web_css; size=sizeof lmb_web_css-1; type="text/css; charset=utf-8"; }
    else if (!strcmp(request->path,"/app.js")) { data=lmb_web_js; size=sizeof lmb_web_js-1; type="text/javascript; charset=utf-8"; }
    else if (!strcmp(request->path,"/logo.svg")) { data=lmb_web_logo; size=sizeof lmb_web_logo-1; type="image/svg+xml"; }
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
static int api_event(int fd, const char *event, const char *json) {
    char header[96]; int n=snprintf(header,sizeof header,"event: %s\ndata: ",event);
    return n<0 || (size_t)n>=sizeof header || api_write(fd,header,(size_t)n) ||
        api_write(fd,json,strlen(json)) || api_write(fd,"\n\n",2) ? -1 : 0;
}
/* The byte codec may split a UTF-8 scalar between DATA frames. Preserve every
 * byte; clients base64-decode deltas into ONE streaming UTF-8 decoder. */
static int api_delta(void *opaque, const unsigned char *data, size_t n) {
    int fd=*(int *)opaque; static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (n>16384) return -1;
    char encoded[21864]; size_t out=0;
    for (size_t i=0; i<n; i+=3) {
        uint32_t x=(uint32_t)data[i]<<16;
        if (i+1<n) x|=(uint32_t)data[i+1]<<8;
        if (i+2<n) x|=data[i+2];
        encoded[out++]=alphabet[(x>>18)&63]; encoded[out++]=alphabet[(x>>12)&63];
        encoded[out++]=i+1<n ? alphabet[(x>>6)&63] : '='; encoded[out++]=i+2<n ? alphabet[x&63] : '=';
    }
    encoded[out]=0; Cap payload={0};
    int bad=cap_str(&payload,"{\"bytes\":\"") || cap_str(&payload,encoded) ||
        cap_str(&payload,"\"}") || api_event(fd,"delta",payload.p);
    free(payload.p); return bad ? -1 : 0;
}
static int api_engine_error(void *opaque, const char *message) {
    Cap payload={0};
    int bad=cap_str(&payload,"{\"message\":") || api_json_text(&payload,message) ||
        cap_str(&payload,"}") || api_event(*(int *)opaque,"error",payload.p);
    free(payload.p); return bad ? -1 : 0;
}
static int api_find_plan(const char *tracker, const uint8_t allocation[32], LmbResidentPlan *out) {
    LmbResidentPlan *plans=calloc(64,sizeof *plans); if (!plans) return -1;
    size_t n=home_resident_library_list(tracker,plans,64); int found=0;
    for (size_t i=0; i<n; i++) if (!memcmp(plans[i].allocation,allocation,32)) {
        if (found++) { free(plans); return -1; }
        *out=plans[i];
    }
    free(plans); return found==1 ? 0 : -1;
}
#include "lumabri_managed_models.h"
#include "lumabri_workspace.h"
static int api_models(int fd, int access_dir, const char *tracker, const LmbApiUser *user) {
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
    if (!bad) bad=api_route_append(&body,access_dir,tracker,user,&comma);
    if (!bad) bad=cap_str(&body,"]}\n");
    if (bad) { if (fd>=0) api_error(fd,500,"allocation_failed"); }
    else if (fd<0) bad=fputs(body.p,stdout)==EOF;
    else bad=api_response(fd,200,body.p);
    free(body.p); free(plans); return bad ? -1 : 0;
}
/* No system/tool roles yet: the established family templates expose paired
 * user/assistant turns. A missing final user turn is not silently repaired. */
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
#include "lumabri_replay_prefix.h"
typedef struct {
    int fd; double first; LmbReplayPrefix replay;
    char error[LMB_REPLY_HEADER]; int retryable;
} ApiObservedStream;
static int api_emitted_delta(void *opaque, const unsigned char *bytes, size_t length) {
    ApiObservedStream *stream=opaque;
    if (length && !stream->first) stream->first=nowd();
    return api_delta(&stream->fd,bytes,length);
}
static int api_observed_delta(void *opaque, const unsigned char *bytes, size_t length) {
    ApiObservedStream *stream=opaque;
    return lmb_replay_feed(&stream->replay,bytes,length,api_emitted_delta,stream);
}
static int api_observed_error(void *opaque, const char *message) {
    ApiObservedStream *stream=opaque;
    stream->retryable=!strncmp(message,LMB_REPLICA_RETRYABLE,strlen(LMB_REPLICA_RETRYABLE));
    snprintf(stream->error,sizeof stream->error,"%s",message);
    return 0; /* The request loop emits one terminal error, or visible recovery. */
}
static void api_chat(int fd, int access_dir, const char *tracker, const LmbApiUser *user, const char *body, size_t length) {
    double started=nowd();
    LmbJsonToken tokens[1024]; LmbJson j; int fields[3];
    const char *const keys[]={"model","messages","max_tokens"}; char id[65]; uint8_t allocation[32]; uint32_t max_new;
    if (lmb_json_parse(&j,body,length,tokens,1024) || lmb_json_fields(&j,0,keys,3,fields) ||
        fields[0]<0 || fields[1]<0 || fields[2]<0 || lmb_api_json_string(&j,(unsigned)fields[0],id,sizeof id) ||
        strlen(id)!=64 || lmb_unhex(allocation,id,32) || lmb_json_uint(&j,(unsigned)fields[2],&max_new) || !max_new) {
        api_error(fd,400,"invalid_chat_request"); return;
    }
    if (!lmb_api_user_allows(user,allocation)) { api_error(fd,403,"model_not_authorized"); return; }
    LmbResidentPlan *selected=calloc(1,sizeof *selected);
    if (!selected) { api_error(fd,500,"allocation_failed"); return; }
    Engine engine; const char *error=NULL; int permit=-1;
    LmbModelRoute contract;
    int status=api_model_open(access_dir,tracker,allocation,max_new,&engine,&permit,selected,&error,&contract);
    if (status) { api_error(fd,(unsigned)status,error); free(selected); return; }
    int seeded=engine.request_seed_supported;
    int recoverable=(seeded || engine.greedy_only) && contract.count>1;
    uint64_t request_seed=0; if (seeded) lmb_random((uint8_t *)&request_seed,sizeof request_seed);
    uint8_t attempted[3][32]; uint32_t attempts=1;
    memcpy(attempted[0],selected->allocation,32);
    LmbCalibration *observation=calloc(1,sizeof *observation); char records[1200], why[200]; int observing=0;
    g_execution_view=&selected->execution;
    if (observation && !home_resident_observation_seed(selected,observation,records) &&
        !catalog_workload_capture_once(tracker,&observation->key,why,sizeof why)) observing=1;
    Cap history={0}; char *prompt=NULL;
    if (api_messages(&j,(unsigned)fields[1],engine.kind,&history,&prompt)) {
        api_error(fd,400,"unsupported_messages_or_limits"); goto finished;
    }
    if (submit_serve2_seeded(&engine,history.p ? history.p : "",prompt,(int)max_new,seeded ? &request_seed : NULL)) {
        api_error(fd,503,"submit_failed"); goto finished;
    }
    const char *header="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\nX-Accel-Buffering: no\r\n\r\n";
    if (api_write(fd,header,strlen(header))) goto finished;
    ApiObservedStream observed={.fd=fd,.replay={.limit=UINT64_C(8)<<20}};
    LmbReplyStream stream; LmbReplySink sink={api_observed_delta,NULL,api_observed_error,&observed};
    int client_closed=0;
    for (;;) {
      int transport_failed=0;
      if (lmb_reply_init(&stream,engine.request_id,UINT64_C(8)<<20,sink)) break;
      observed.error[0]=0; observed.retryable=0;
      while (stream.status==LMB_REPLY_MORE && !api_stopping) {
        struct pollfd pollers[2]={{engine.from,POLLIN,0},{fd,POLLIN,0}};
        int rc=poll(pollers,2,1000);
        if (rc<0 && errno==EINTR) continue;
        if (rc<0) { transport_failed=1; break; }
        /* No pipelining or half-closed request bodies. A browser AbortController
         * disconnect cancels this conversation, never another user's slot. */
        if (pollers[1].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)) { client_closed=1; break; }
        if (pollers[0].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)) {
            unsigned char bytes[16384]; ssize_t got=read(engine.from,bytes,sizeof bytes);
            if (got<0 && errno==EINTR) continue;
            if (got<=0) { transport_failed=1; break; }
            lmb_reply_feed(&stream,bytes,(size_t)got,NULL);
        }
      }
      if (client_closed || api_stopping || stream.status==LMB_REPLY_DONE || stream.status==LMB_REPLY_ABORTED ||
          !recoverable || attempts>=3 || !(transport_failed || (stream.status==LMB_REPLY_ERROR && observed.retryable))) break;
      /* Recheck revocation and the current model membership before sending
       * any text again. A newly added replica is not implicitly adopted by
       * an in-flight turn; the original immutable contract bounds recovery. */
      LmbApiUser current;
      if (lmb_api_user_load(access_dir,user->name,&current) || memcmp(current.digest,user->digest,sizeof user->digest) ||
          !lmb_api_user_allows(&current,allocation)) { snprintf(observed.error,sizeof observed.error,"Access revoked during recovery"); break; }
      struct pollfd client={fd,POLLIN,0};
      if (poll(&client,1,0)!=0) { client_closed=1; break; }
      if (api_event(fd,"recovering","{\"message\":\"Replica interrupted; replaying on another approved replica and verifying the visible prefix\"}")) { client_closed=1; break; }
      engine_stop(&engine); if (permit>=0) close(permit); permit=-1;
      status=api_model_open_ex(access_dir,tracker,allocation,max_new,&engine,&permit,selected,&error,
          &contract,&attempted[0][0],attempts,seeded,NULL);
      if (status) { snprintf(observed.error,sizeof observed.error,"Replica unavailable; no other approved capacity can recover this response"); break; }
      client.revents=0;
      if (poll(&client,1,0)!=0) { client_closed=1; break; }
      if (lmb_api_user_load(access_dir,user->name,&current) || memcmp(current.digest,user->digest,sizeof user->digest) ||
          !lmb_api_user_allows(&current,allocation)) { snprintf(observed.error,sizeof observed.error,"Access revoked during recovery"); break; }
      memcpy(attempted[attempts++],selected->allocation,32);
      lmb_replay_begin(&observed.replay);
      if (submit_serve2_seeded(&engine,history.p ? history.p : "",prompt,(int)max_new,seeded ? &request_seed : NULL)) {
          snprintf(observed.error,sizeof observed.error,"Recovery submission failed; response remains incomplete"); break;
      }
    }
    if (!client_closed && !api_stopping && stream.status==LMB_REPLY_DONE && lmb_replay_complete(&observed.replay)) {
        int observation_saved=0;
        if (attempts==1 && observing && observed.first>started)
            observation_saved=catalog_record_turn(observation,&observation->key,records,tracker,&engine,stream.stat,
                                                  observed.first-started,LMB_CAL_SOURCE_SESSION);
        Cap payload={0};
        if (!cap_str(&payload,"{\"stats\":") && !api_json_text(&payload,stream.stat) &&
            !api_addf(&payload,",\"observation_saved\":%s,\"recovery_attempts\":%u,\"stats_scope\":\"%s\"}",
                observation_saved ? "true" : "false",attempts-1,attempts>1 ? "final_attempt_only" : "single_attempt"))
            (void)api_event(fd,"done",payload.p);
        free(payload.p);
    } else if (!client_closed && !api_stopping) {
        const char *reason=observed.replay.mismatch || (stream.status==LMB_REPLY_DONE && !lmb_replay_complete(&observed.replay)) ?
            "Recovery did not reproduce the visible prefix; response remains incomplete" :
            observed.error[0] ? observed.error : "Stream interrupted; response is incomplete";
        (void)api_engine_error(&fd,reason);
    }
    lmb_replay_free(&observed.replay);
finished:
    free(history.p); free(prompt); engine_stop(&engine);
    if (permit>=0) close(permit);
    g_execution_view=NULL; free(observation); free(selected);
}
static void api_connection(int fd, int access_dir, unsigned port, const char *tracker) {
    struct timeval timeout={5,0};
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    char header[LMB_API_HEADER_MAX]; size_t length=0; int complete=0;
    uint64_t deadline=lmb_io_monotonic_ms()+5000;
    while (length<sizeof header && lmb_io_monotonic_ms()<deadline) {
        ssize_t got=recv(fd,header+length,1,0);
        if (got<0 && errno==EINTR) continue;
        if (got!=1) break;
        if (++length>=4 && !memcmp(header+length-4,"\r\n\r\n",4)) { complete=1; break; }
    }
    LmbApiHttp request;
    if (!complete || lmb_api_http_parse(&request,header,length)) { api_error(fd,400,"invalid_http_request"); return; }
    if (lmb_api_http_origin(&request,port)) { api_error(fd,403,"origin_or_host_rejected"); return; }
    if (api_web(fd,&request)) return;
    LmbApiUser user;
    if (lmb_api_authorize(access_dir,request.authorization,&user)) { api_error(fd,401,"authentication_required"); return; }
    lmb_api_wipe(header,sizeof header); lmb_api_wipe(request.authorization,sizeof request.authorization);
    int permit=lmb_api_user_permit(access_dir,user.name,2);
    if (permit<0) { api_error(fd,429,"user_request_limit"); return; }
    const char *history_path="/api/v1/conversations";
    size_t history_prefix=strlen(history_path);
    int history=!strncmp(request.path,history_path,history_prefix) &&
        (!request.path[history_prefix] || request.path[history_prefix]=='/');
    if (!strcmp(request.method,"GET") && !strcmp(request.path,"/api/v1/session") && !request.length)
        (void)api_response(fd,200,lmb_operator_allows(access_dir,&user) ?
            "{\"schema\":1,\"operator\":true}" : "{\"schema\":1,\"operator\":false}");
    else if (!strcmp(request.method,"GET") && !strcmp(request.path,"/api/v1/workspace") && !request.length) {
        if (lmb_operator_allows(access_dir,&user)) (void)api_workspace(fd,access_dir,tracker);
        else api_error(fd,403,"operator_permission_required");
    } else if (!strcmp(request.method,"GET") && !strcmp(request.path,"/api/v1/models") && !request.length)
        api_models(fd,access_dir,tracker,&user);
    else if (history && !strcmp(request.method,"GET") && !request.length)
        api_history(fd,access_dir,&user,&request,NULL,0);
    else if (((!strcmp(request.method,"POST") && !strcmp(request.path,"/api/v1/chat")) ||
              (history && (!strcmp(request.method,"POST") || !strcmp(request.method,"DELETE")))) &&
             request.has_length && request.length && request.json) {
        char *body=malloc((size_t)request.length+1); size_t at=0;
        deadline=lmb_io_monotonic_ms()+5000;
        if (!body) api_error(fd,500,"allocation_failed");
        else {
            while (at<request.length && lmb_io_monotonic_ms()<deadline) {
                ssize_t got=recv(fd,body+at,request.length-at,0);
                if (got<0 && errno==EINTR) continue;
                if (got<=0) break;
                at+=(size_t)got;
            }
            body[at]=0;
            if (at!=request.length) api_error(fd,408,"request_body_timeout");
            else if (history) api_history(fd,access_dir,&user,&request,body,at);
            else api_chat(fd,access_dir,tracker,&user,body,at);
            lmb_api_wipe(body,(size_t)request.length+1); free(body);
        }
    } else api_error(fd,400,"unsupported_endpoint_or_framing");
    close(permit);
}
static int api_serve(int access_dir, unsigned port, const char *tracker, HomeService *keeper) {
    int listener=socket(AF_INET,SOCK_STREAM,0); if (listener<0) return 1;
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons((uint16_t)port);
    int one=1; (void)setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    if (fcntl(listener,F_SETFD,FD_CLOEXEC) ||
        bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,16)) {
        fprintf(stderr,"[api] Cannot listen on 127.0.0.1:%u: %s\n",port,strerror(errno)); close(listener); return 1;
    }
    signal(SIGTERM,api_stop); signal(SIGINT,api_stop); api_stopping=0;
    pid_t children[LMB_API_CHILDREN]={0};
    if (keeper) {
        keeper->snapshot.phase=2; /* API role: 1 starting, 2 listening */
        snprintf(keeper->snapshot.detail,sizeof keeper->snapshot.detail,"Listening on %s; approved resident models only.",keeper->snapshot.name);
        if (home_service_save(keeper)) { close(listener); return 1; }
    }
    printf("[api] http://127.0.0.1:%u · bearer authentication · approved resident models only\n",port); fflush(stdout);
    while (!api_stopping) {
        if (keeper) {
            int op=home_service_poll(keeper);
            if (op==HOME_SVC_STOP) break;
            home_service_answer(keeper);
        }
        for (unsigned i=0; i<LMB_API_CHILDREN; i++) if (children[i] && waitpid(children[i],NULL,WNOHANG)==children[i]) children[i]=0;
        struct pollfd p={listener,POLLIN,0}; int rc=poll(&p,1,250);
        if (rc<0 && errno==EINTR) continue;
        if (rc<0) break;
        if (!rc) continue;
        int client=accept(listener,NULL,NULL); if (client<0) continue;
        unsigned slot=0; while (slot<LMB_API_CHILDREN && children[slot]) slot++;
        if (slot==LMB_API_CHILDREN) { close(client); continue; }
        fflush(NULL); pid_t child=fork();
        if (!child) {
            close(listener); signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL); signal(SIGALRM,SIG_DFL);
            /* Requests do not own the keeper lock/socket. A gateway crash
             * must not leave its singleton lock held by orphaned requests. */
            if (keeper) {
                if (keeper->listener>=0) close(keeper->listener);
                if (keeper->lock>=0) close(keeper->lock);
                if (keeper->reply>=0) close(keeper->reply);
            }
            alarm(LMB_API_TURN_SECONDS); /* hard lifetime even during a stalled host handshake */
            api_connection(client,access_dir,port,tracker); shutdown(client,SHUT_RDWR); close(client); fflush(NULL); _exit(0);
        }
        close(client); if (child>0) children[slot]=child;
    }
    close(listener);
    for (unsigned i=0; i<LMB_API_CHILDREN; i++) if (children[i]) (void)kill(children[i],SIGTERM);
    for (unsigned i=0; i<LMB_API_CHILDREN; i++) if (children[i]) while (waitpid(children[i],NULL,0)<0 && errno==EINTR) { }
    return 0;
}
static int api_start(int access_dir, unsigned port, const char *tracker) {
    if (strlen(tracker)>=sizeof ((HomeServiceSnapshot *)0)->tracker) return 1;
    if (home_service_foreground() || home_service_ensure()) {
        fprintf(stderr,"Cannot start the API keeper; use api serve for foreground operation\n"); return 1;
    }
    char endpoint[64]; snprintf(endpoint,sizeof endpoint,"http://127.0.0.1:%u",port);
    HomeServiceSnapshot observed;
    if (!home_service_query("api",HOME_SVC_STATUS,NULL,&observed)) {
        if (strcmp(observed.tracker,tracker) || strcmp(observed.name,endpoint)) {
            fprintf(stderr,"Another API configuration is active; stop it explicitly before changing the household or port\n"); return 1;
        }
        return observed.phase==2 ? 0 : 1;
    }
    HomeService keeper;
    if (home_service_open(&keeper,"api")) {
        /* A concurrent start can be between taking the lock and readiness. */
        for (unsigned i=0; i<40; i++) {
            if (!home_service_query("api",HOME_SVC_STATUS,NULL,&observed) && observed.phase==2 &&
                !strcmp(observed.tracker,tracker) && !strcmp(observed.name,endpoint)) return 0;
            (void)poll(NULL,0,50);
        }
        return 1;
    }
    snprintf(keeper.snapshot.name,sizeof keeper.snapshot.name,"%s",endpoint);
    snprintf(keeper.snapshot.tracker,sizeof keeper.snapshot.tracker,"%s",tracker);
    snprintf(keeper.snapshot.detail,sizeof keeper.snapshot.detail,"Starting authenticated loopback API");
    keeper.snapshot.phase=1;
    if (home_service_save(&keeper)) { home_service_close(&keeper); return 1; }
    int detached=home_service_detach(&keeper,access_dir);
    if (detached<0) { home_service_close(&keeper); return 1; }
    if (detached) {
        for (unsigned i=0; i<60; i++) {
            if (!home_service_query("api",HOME_SVC_STATUS,NULL,&observed) && observed.phase==2 &&
                !strcmp(observed.tracker,tracker) && !strcmp(observed.name,endpoint)) { puts(endpoint); return 0; }
            (void)poll(NULL,0,50);
        }
        fprintf(stderr,"API did not become ready; inspect lumabri service status and api.log\n"); return 1;
    }
    int rc=api_serve(access_dir,port,tracker,&keeper);
    keeper.snapshot.state=rc ? HOME_SVC_FAILED : HOME_SVC_STOPPED; keeper.snapshot.phase=0;
    snprintf(keeper.snapshot.detail,sizeof keeper.snapshot.detail,"API %s; resident models were not unloaded.",rc ? "failed" : "stopped");
    (void)home_service_save(&keeper); home_service_answer(&keeper); home_service_close(&keeper);
    close(access_dir); _exit(rc ? 1 : 0);
}
static int cmd_api(int argc, char **argv) {
    if (!argc) goto usage;
    HomeSettings settings; home_settings_load(&settings);
    const char *tracker=settings.tracker; unsigned port=47380;
    char *args[LMB_ROUTE_REPLICAS+1]={0}; unsigned count=0;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--tracker") && i+1<argc) tracker=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) {
            char *end; unsigned long value=strtoul(argv[++i],&end,10);
            if (!*argv[i] || *end || !value || value>65535) goto usage;
            port=(unsigned)value;
        } else if (count<LMB_ROUTE_REPLICAS+1) args[count++]=argv[i]; else goto usage;
    }
    if (!strcmp(tracker,settings.tracker) && home_settings_activate(&settings)) return 1;
    if (!strcmp(argv[0],"stop") && !count) return home_service_stop_role("api") ? 1 : 0;
    char path[1200];
    if (checked_printf(path,sizeof path,"%s/.lumabri/api-access",getenv("HOME") ? getenv("HOME") : ".")) return 1;
    int dir=lmb_api_access_dir(path,1); if (dir<0) { fprintf(stderr,"Cannot open private API access directory\n"); return 1; }
    if (!strcmp(argv[0],"list") && !count && *tracker) {
        int rc=api_models(-1,dir,tracker,NULL); close(dir); return rc ? 1 : 0;
    }
    if (!strcmp(argv[0],"workspace") && !count && *tracker) {
        int rc=api_workspace(-1,dir,tracker); close(dir); return rc ? 1 : 0;
    }
    if (!strcmp(argv[0],"model-add") || !strcmp(argv[0],"model-set") || !strcmp(argv[0],"model-remove")) {
        int rc=api_route_admin(dir,argv[0],tracker,args,count); close(dir); return rc;
    }
    if (!strcmp(argv[0],"model-policy")) {
        int rc=api_route_policy(dir,tracker,args,count); close(dir); return rc;
    }
    if (!strcmp(argv[0],"replica")) {
        int rc=api_replica_control(tracker,args,count); close(dir); return rc;
    }
    if (!strcmp(argv[0],"segments")) {
        int rc=api_segments_control(tracker,args,count); close(dir); return rc;
    }
    if ((!strcmp(argv[0],"serve") || !strcmp(argv[0],"start")) && !count && *tracker) {
        int rc=!strcmp(argv[0],"start") ? api_start(dir,port,tracker) : api_serve(dir,port,tracker,NULL);
        close(dir); return rc;
    }
    int lock=lmb_api_access_lock(dir),rc=1;
    if (lock<0) { close(dir); fprintf(stderr,"API access database is busy or unsafe\n"); return 1; }
    if (!strcmp(argv[0],"user-add") && count==1) {
        char token[98]; rc=lmb_api_user_create(dir,args[0],token);
        if (!rc) { puts(token); lmb_api_wipe(token,sizeof token); }
    } else if ((!strcmp(argv[0],"operator-grant") || !strcmp(argv[0],"operator-revoke")) && count==1) {
        LmbApiUser user;
        if (!lmb_api_user_load(dir,args[0],&user)) rc=lmb_operator_set(dir,&user,!strcmp(argv[0],"operator-grant"));
    } else if (!strcmp(argv[0],"revoke") && count==1 && lmb_api_username(args[0])) {
        char file[40]; snprintf(file,sizeof file,"%s.user",args[0]); rc=unlinkat(dir,file,0);
        if (!rc) rc=fsync(dir);
    } else if (!strcmp(argv[0],"grant") && count==2 && *tracker) {
        LmbApiUser user; LmbResidentPlan *plan=calloc(1,sizeof *plan); LmbModelRoute route; uint8_t allocation[32];
        if (plan && strlen(args[1])==64 && !lmb_unhex(allocation,args[1],32) &&
            !lmb_api_user_load(dir,args[0],&user) &&
            (!api_route_read(dir,allocation,tracker,&route) || !api_find_plan(tracker,allocation,plan))) {
            if (lmb_api_user_allows(&user,allocation)) rc=0;
            else if (user.count<LMB_API_MODELS_MAX) { memcpy(user.allocations[user.count++],allocation,32); rc=lmb_api_user_save(dir,&user); }
        }
        free(plan);
    } else { close(lock); close(dir); goto usage; }
    close(lock); close(dir);
    if (rc) fprintf(stderr,"API access operation failed; check the user and approved allocation\n");
    return rc ? 1 : 0;
usage:
    fprintf(stderr,"lumabri api list [--tracker HOST:PORT]\n"
        "lumabri api model-add NAME APPROVED_ALLOCATION... [--tracker HOST:PORT]\n"
        "lumabri api model-set MODEL_ID APPROVED_ALLOCATION... [--tracker HOST:PORT]\n"
        "lumabri api model-remove MODEL_ID [--tracker HOST:PORT]\n"
        "lumabri api model-policy MODEL_ID ordered|observed-decode|declared-cost [--tracker HOST:PORT]\n"
        "lumabri api replica ALLOCATION_ID [status|drain|resume] [--tracker HOST:PORT]\n"
        "lumabri api segments ALLOCATION_ID [status|drain|resume] [--tracker HOST:PORT]\n"
        "lumabri api user-add NAME\n"
        "lumabri api operator-grant NAME    read-only cluster visibility, no inference grant\n"
        "lumabri api operator-revoke NAME\n"
        "lumabri api workspace [--tracker HOST:PORT]\n"
        "lumabri api grant NAME MODEL_OR_ALLOCATION_ID [--tracker HOST:PORT]\n"
        "lumabri api revoke NAME\n"
        "lumabri api start [--tracker HOST:PORT] [--port 47380]\n"
        "lumabri api stop\n"
        "lumabri api serve [--tracker HOST:PORT] [--port 47380]   foreground diagnostics\n");
    return 2;
}
#endif
