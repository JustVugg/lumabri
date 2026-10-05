/* Authenticated loopback gateway to ALREADY approved resident allocations.
 * One bounded child per HTTP request. No model loading, donor approval,
 * endpoint supplied by clients, persistent chat state or shared Engine globals.
 * Requires the controller's Engine/template and resident-plan helpers. */
#ifndef LUMABRI_API_H
#define LUMABRI_API_H
#include "lumabri_api_http.h"
#include "lumabri_api_json.h"
#include "lumabri_api_access.h"
#include "lumabri_resident_control.h"

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
static void api_chat(int fd, const char *tracker, const LmbApiUser *user, const char *body, size_t length) {
    LmbJsonToken tokens[1024]; LmbJson j; int fields[3];
    const char *const keys[]={"model","messages","max_tokens"}; char id[65]; uint8_t allocation[32]; uint32_t max_new;
    if (lmb_json_parse(&j,body,length,tokens,1024) || lmb_json_fields(&j,0,keys,3,fields) ||
        fields[0]<0 || fields[1]<0 || fields[2]<0 || lmb_api_json_string(&j,(unsigned)fields[0],id,sizeof id) ||
        strlen(id)!=64 || lmb_unhex(allocation,id,32) || lmb_json_uint(&j,(unsigned)fields[2],&max_new) || !max_new) {
        api_error(fd,400,"invalid_chat_request"); return;
    }
    if (!lmb_api_user_allows(user,allocation)) { api_error(fd,403,"model_not_authorized"); return; }
    LmbResidentPlan *plan=calloc(1,sizeof *plan);
    if (!plan) { api_error(fd,500,"allocation_failed"); return; }
    if (api_find_plan(tracker,allocation,plan)) { free(plan); api_error(fd,404,"resident_plan_not_found"); return; }
    if (max_new>plan->max_new) { free(plan); api_error(fd,400,"max_tokens_exceeds_plan"); return; }
    /* Verify live approval/root on every participating donor. No previous
     * on-disk hint grants permission to recreate or substitute an allocation. */
    for (uint32_t i=0; i<plan->execution.count; i++) if (home_resident_peer(plan,i,0)) {
        free(plan); api_error(fd,503,"approved_allocation_unavailable"); return;
    }
    Engine engine; int requested=(int)max_new;
    if (host_connect(plan->host,NULL,plan->host_key,plan->root,&engine,&requested)) {
        free(plan); api_error(fd,503,"host_unavailable_or_busy"); return;
    }
    free(plan); Cap history={0}; char *prompt=NULL;
    if (engine.proto!=PROTO_SERVE2 || requested!=(int)max_new ||
        api_messages(&j,(unsigned)fields[1],engine.kind,&history,&prompt)) {
        api_error(fd,400,"unsupported_messages_or_limits"); goto finished;
    }
    if (submit_serve2(&engine,history.p ? history.p : "",prompt,(int)max_new)) {
        api_error(fd,503,"submit_failed"); goto finished;
    }
    const char *header="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\nX-Accel-Buffering: no\r\n\r\n";
    if (api_write(fd,header,strlen(header))) goto finished;
    LmbReplyStream stream; LmbReplySink sink={api_delta,NULL,api_engine_error,&fd};
    if (lmb_reply_init(&stream,engine.request_id,UINT64_C(8)<<20,sink)) goto finished;
    while (stream.status==LMB_REPLY_MORE && !api_stopping) {
        struct pollfd pollers[2]={{engine.from,POLLIN,0},{fd,POLLIN,0}};
        int rc=poll(pollers,2,1000);
        if (rc<0 && errno==EINTR) continue;
        if (rc<0) break;
        /* No pipelining or half-closed request bodies. A browser AbortController
         * disconnect cancels this conversation, never another user's slot. */
        if (pollers[1].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)) break;
        if (pollers[0].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)) {
            unsigned char bytes[16384]; ssize_t got=read(engine.from,bytes,sizeof bytes);
            if (got<0 && errno==EINTR) continue;
            if (got<=0) break;
            lmb_reply_feed(&stream,bytes,(size_t)got,NULL);
        }
    }
    if (stream.status==LMB_REPLY_DONE) {
        Cap payload={0};
        if (!cap_str(&payload,"{\"stats\":") && !api_json_text(&payload,stream.stat) && !cap_str(&payload,"}"))
            (void)api_event(fd,"done",payload.p);
        free(payload.p);
    } else if (stream.status!=LMB_REPLY_ERROR && stream.status!=LMB_REPLY_ABORTED)
        (void)api_event(fd,"error","{\"message\":\"Stream interrupted; response is incomplete\"}");
finished:
    free(history.p); free(prompt); engine_stop(&engine);
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
    LmbApiUser user;
    if (lmb_api_authorize(access_dir,request.authorization,&user)) { api_error(fd,401,"authentication_required"); return; }
    lmb_api_wipe(header,sizeof header); lmb_api_wipe(request.authorization,sizeof request.authorization);
    int permit=lmb_api_user_permit(access_dir,user.name,2);
    if (permit<0) { api_error(fd,429,"user_request_limit"); return; }
    if (!strcmp(request.method,"GET") && !strcmp(request.path,"/api/v1/models") && !request.length)
        api_models(fd,tracker,&user);
    else if (!strcmp(request.method,"POST") && !strcmp(request.path,"/api/v1/chat") &&
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
            else api_chat(fd,tracker,&user,body,at);
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
    char *args[3]={0}; unsigned count=0;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--tracker") && i+1<argc) tracker=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) {
            char *end; unsigned long value=strtoul(argv[++i],&end,10);
            if (!*argv[i] || *end || !value || value>65535) goto usage;
            port=(unsigned)value;
        } else if (count<3) args[count++]=argv[i]; else goto usage;
    }
    if (!strcmp(tracker,settings.tracker) && home_settings_activate(&settings)) return 1;
    if (!strcmp(argv[0],"stop") && !count) return home_service_stop_role("api") ? 1 : 0;
    if (!strcmp(argv[0],"list") && !count && *tracker) return api_models(-1,tracker,NULL) ? 1 : 0;
    char path[1200];
    if (checked_printf(path,sizeof path,"%s/.lumabri/api-access",getenv("HOME") ? getenv("HOME") : ".")) return 1;
    int dir=lmb_api_access_dir(path,1); if (dir<0) { fprintf(stderr,"Cannot open private API access directory\n"); return 1; }
    if ((!strcmp(argv[0],"serve") || !strcmp(argv[0],"start")) && !count && *tracker) {
        int rc=!strcmp(argv[0],"start") ? api_start(dir,port,tracker) : api_serve(dir,port,tracker,NULL);
        close(dir); return rc;
    }
    int lock=lmb_api_access_lock(dir),rc=1;
    if (lock<0) { close(dir); fprintf(stderr,"API access database is busy or unsafe\n"); return 1; }
    if (!strcmp(argv[0],"user-add") && count==1) {
        char token[98]; rc=lmb_api_user_create(dir,args[0],token);
        if (!rc) { puts(token); lmb_api_wipe(token,sizeof token); }
    } else if (!strcmp(argv[0],"revoke") && count==1 && lmb_api_username(args[0])) {
        char file[40]; snprintf(file,sizeof file,"%s.user",args[0]); rc=unlinkat(dir,file,0);
        if (!rc) rc=fsync(dir);
    } else if (!strcmp(argv[0],"grant") && count==2 && *tracker) {
        LmbApiUser user; LmbResidentPlan *plan=calloc(1,sizeof *plan); uint8_t allocation[32];
        if (plan && strlen(args[1])==64 && !lmb_unhex(allocation,args[1],32) &&
            !lmb_api_user_load(dir,args[0],&user) && !api_find_plan(tracker,allocation,plan)) {
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
        "lumabri api user-add NAME\n"
        "lumabri api grant NAME ALLOCATION [--tracker HOST:PORT]\n"
        "lumabri api revoke NAME\n"
        "lumabri api start [--tracker HOST:PORT] [--port 47380]\n"
        "lumabri api stop\n"
        "lumabri api serve [--tracker HOST:PORT] [--port 47380]   foreground diagnostics\n");
    return 2;
}
#endif
