/* Explicit local-owner calibration of already approved allocations. Uses the
 * real gateway chat/admission/cleanup path, with socketpairs instead of HTTP
 * auth/network setup. It neither prepares weights nor replays donor consent.
 * Bound process count, response memory, duration and durable record count. */
#ifndef LMB_CAPACITY_PROBE_H
#define LMB_CAPACITY_PROBE_H
#include "src/planner/lumabri_capacity_store.h"
#define LMB_CAPACITY_WIRE (1024u*1024u)

static int api_capacity_snapshot(const char *tracker,LmbCapacity *r,uint8_t digest[32],LmbCapacityCost *cost) {
    LmbMachineReport reports[LMB_INVENTORY_MAX]; uint32_t n=0;
    LmbResidentPlan *plan=calloc(1,sizeof *plan); LmbCalibration *seed=calloc(1,sizeof *seed);
    LmbBuf bytes={0}; int bad=!plan || !seed || lmb_inventory_fetch(tracker,reports,&n);
    uint8_t used[LMB_INVENTORY_MAX]={0}; cost->known=1; cost->micro_per_hour=0; cost->currency[0]=0;
    if (!bad) bad=lmb_buf_str(&bytes,"LMB-CAPACITY-SNAPSHOT1") || lmb_buf_str(&bytes,tracker);
    for (uint32_t m=0;!bad && m<r->models;m++) {
        char records[1200],why[200]; uint8_t key[32]; Engine engine;
        if (api_find_plan(tracker,r->allocations[m],plan) || r->max_new>plan->max_new ||
            home_resident_observation_seed(plan,seed,records) ||
            catalog_runtime_match(&seed->key,reports,n,why,sizeof why)) { bad=1; break; }
        /* Initial probe obtains the numeric contract from the real host.
         * Later inspection must not open a conversation (which would itself
         * consume capacity). Its immutable runtime/build identity is checked
         * above, plus current owner-bound keeper and host control below. */
        if (!r->numeric_abi[m]) {
            if (api_plan_open(plan,&engine,2)) { bad=1; break; }
            r->numeric_abi[m]=engine.numeric_abi;
            snprintf(r->numeric_class[m],sizeof r->numeric_class[m],"%s",engine.numeric_class);
            if (engine_finish(&engine)) { bad=1; break; }
        } else {
            LmbHostControl state;
            if (lmb_host_control_rpc(plan->host,plan->host_key,plan->root,0,NULL,&state) || state.draining) { bad=1; break; }
            for (uint32_t i=0;!bad && i<plan->execution.count;i++) if (home_resident_peer(plan,i,0)) bad=1;
        }
        seed->key.adapter_abi=r->numeric_abi[m];
        snprintf(seed->key.numeric_class,sizeof seed->key.numeric_class,"%s",r->numeric_class[m]);
        for (uint32_t i=0;!bad && i<seed->key.nodes;i++) {
            uint32_t j=0;
            while (j<n && memcmp(plan->peer_keys[i],reports[j].identity,32)) j++;
            if (j==n || !reports[j].workload.known || !reports[j].workload.allocations ||
                reports[j].workload.reserved_bytes<plan->execution.nodes[i].reserved_bytes) { bad=1; break; }
            seed->key.workload[i]=reports[j].workload;
            seed->key.workload[i].active=seed->key.workload[i].queued=0;
            if (!used[j]++) lmb_capacity_cost_add(cost,&reports[j].facts);
        }
        if (!bad) {
            LmbBuf contract={0}; uint8_t content[32],numeric[32],zero[32]={0};
            bad=lmb_buf_str(&contract,"LMB-CAPACITY-CONTRACT1") || lmb_buf_str(&contract,seed->key.model_root) ||
                lmb_buf_str(&contract,seed->key.adapter) || lmb_buf_u32(&contract,seed->key.adapter_abi) ||
                lmb_buf_str(&contract,seed->key.numeric_class) || lmb_buf_u32(&contract,seed->key.context) ||
                lmb_buf_u32(&contract,seed->key.sessions) || lmb_unhex(content,plan->content_id,32);
            if (!bad) {
                LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,contract.p,contract.len); lmb_sha_final(&sha,numeric);
                if (!memcmp(r->contents[m],zero,32)) { memcpy(r->contents[m],content,32); memcpy(r->contracts[m],numeric,32); }
                else if (memcmp(r->contents[m],content,32) || memcmp(r->contracts[m],numeric,32)) bad=1;
            }
            free(contract.p);
            if (!bad) bad=lmb_capacity_key_hash(&seed->key,key) || lmb_buf_bytes(&bytes,r->allocations[m],32) || lmb_buf_bytes(&bytes,key,32);
        }
    }
    if (!bad) { LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,bytes.p,bytes.len); lmb_sha_final(&sha,digest); }
    if (bad) cost->known=0;
    free(bytes.p); free(seed); free(plan); return bad ? -1 : 0;
}
static int api_capacity_number(const LmbJson *j,int index,double *out) {
    if (index<0 || j->tokens[index].kind!=LMB_JSON_NUMBER) return -1;
    const LmbJsonToken *t=&j->tokens[index]; size_t n=t->end-t->begin; char text[64],*end;
    if (!n || n>=sizeof text) return -1;
    memcpy(text,j->text+t->begin,n); text[n]=0; errno=0; double value=strtod(text,&end);
    if (errno || *end || !isfinite(value) || value<0 || value>3600) return -1;
    *out=value; return 0;
}
static void api_capacity_parse(Cap *wire,LmbCapacitySample *s) {
    if (!wire->p || strncmp(wire->p,"HTTP/1.1 ",9) || wire->len<13) return;
    if (wire->p[9]<'1' || wire->p[9]>'5' || wire->p[10]<'0' || wire->p[10]>'9' ||
        wire->p[11]<'0' || wire->p[11]>'9' || wire->p[12]!=' ') return;
    s->status=(unsigned)(wire->p[9]-'0')*100+(unsigned)(wire->p[10]-'0')*10+(unsigned)(wire->p[11]-'0');
    if (s->status!=200) return;
    char *done=strstr(wire->p,"\nevent: done\ndata: ");
    if (!done) return;
    done+=19; char *end=strchr(done,'\n'); if (!end || strcmp(end,"\n\n")) return;
    LmbJsonToken tokens[128]; LmbJson j; int fields[5];
    const char *const keys[]={"stats","observation_saved","recovery_attempts","stats_scope","timing"};
    if (lmb_json_parse(&j,done,(size_t)(end-done),tokens,128) || lmb_json_fields(&j,0,keys,5,fields) ||
        fields[0]<0 || fields[2]<0 || fields[4]<0) return;
    char stat[LMB_REPLY_HEADER]; uint32_t retries;
    if (lmb_api_json_string(&j,(unsigned)fields[0],stat,sizeof stat) ||
        lmb_json_uint(&j,(unsigned)fields[2],&retries) || retries) return;
    LmbGenerationMetrics metrics; if (lmb_metrics_parse(stat,&metrics)) return;
    s->complete=1;
    const char *const timing_keys[]={"scope","generation_seconds","completion_seconds","ttft_seconds",
        "first_token_notification_seconds","token_notifications","token_notification_gaps"};
    int timing[7]; double first,completion,gap; uint32_t notifications;
    if (lmb_json_fields(&j,(unsigned)fields[4],timing_keys,7,timing) || timing[5]<0 || timing[6]<0 ||
        api_capacity_number(&j,timing[3],&first) || api_capacity_number(&j,timing[2],&completion) ||
        lmb_json_uint(&j,(unsigned)timing[5],&notifications) || notifications!=metrics.generated_tokens || notifications<2) return;
    const char *const gap_keys[]={"count","p50_seconds","p95_seconds","method"}; int gaps[4]; uint32_t gap_count;
    if (lmb_json_fields(&j,(unsigned)timing[6],gap_keys,4,gaps) || gaps[0]<0 ||
        lmb_json_uint(&j,(unsigned)gaps[0],&gap_count) || gap_count!=notifications-1 ||
        api_capacity_number(&j,gaps[2],&gap) || first>completion || gap>completion) return;
    /* STAT's fifth leading field is the actual tokenized prompt length. */
    const char *p=stat;
    for (unsigned i=0;i<5;i++) { p=strchr(p,' '); if (!p) return; p++; }
    const char *start=p; uint32_t prompt;
    if (lmb_response_uint(&p,&prompt) || !prompt || p==start || *p!=' ') return;
    s->timing_known=1; s->ttft=first; s->completion=completion; s->gap_p95=gap;
    s->prompt_tokens=prompt; s->generated_tokens=notifications;
}
static int api_capacity_round(int access,const char *tracker,LmbCapacity *r,uint32_t round,int guard) {
    int pair[LMB_CAPACITY_CLIENTS][2],gate[2]={-1,-1}; pid_t children[LMB_CAPACITY_CLIENTS]={0};
    Cap wires[LMB_CAPACITY_CLIENTS]={0}; int bad=0; uint32_t started=0;
    for (uint32_t i=0;i<LMB_CAPACITY_CLIENTS;i++) pair[i][0]=pair[i][1]=-1;
    if (pipe(gate)) return -1;
    for (uint32_t i=0;i<r->clients;i++) if (socketpair(AF_UNIX,SOCK_STREAM,0,pair[i])) { bad=1; break; }
    for (uint32_t i=0;!bad && i<r->clients;i++) {
        pid_t pid=fork(); if (pid<0) { bad=1; break; }
        if (!pid) {
            close(guard); close(gate[1]);
            for (uint32_t k=0;k<r->clients;k++) { close(pair[k][0]); if (k!=i) close(pair[k][1]); }
            signal(SIGINT,api_stop); signal(SIGTERM,api_stop); signal(SIGALRM,api_stop); alarm(LMB_API_TURN_SECONDS);
            char ready; if (read(gate[0],&ready,1)!=1) _exit(1); close(gate[0]);
            lmb_read_deadline_ms=lmb_io_monotonic_ms()+LMB_API_TURN_SECONDS*1000u;
            LmbApiUser owner={.name="local-capacity-probe",.count=1};
            memcpy(owner.allocations[0],r->allocations[i%r->models],32);
            char id[65],body[256]; lmb_hex(id,owner.allocations[0],32);
            int n=snprintf(body,sizeof body,"{\"model\":\"%s\",\"max_tokens\":%u,\"messages\":[{\"role\":\"user\","
                "\"content\":\"Count from one to ten.\"}]}",id,r->max_new);
            if (n>0 && (size_t)n<sizeof body) api_chat(pair[i][1],access,tracker,&owner,body,(size_t)n);
            close(pair[i][1]); _exit(0);
        }
        children[i]=pid; started++;
    }
    close(gate[0]);
    for (uint32_t i=0;i<r->clients;i++) if (pair[i][1]>=0) { close(pair[i][1]); pair[i][1]=-1; }
    if (!bad) { char ready[LMB_CAPACITY_CLIENTS]={0}; bad=lmb_capacity_write(gate[1],ready,r->clients); }
    close(gate[1]);
    double began=nowd(),deadline=began+LMB_API_TURN_SECONDS+35; uint32_t remaining=started;
    while (!bad && remaining && !api_stopping && nowd()<deadline) {
        struct pollfd pollers[LMB_CAPACITY_CLIENTS];
        for (uint32_t i=0;i<started;i++) pollers[i]=(struct pollfd){pair[i][0],POLLIN,0};
        int rc=poll(pollers,started,200);
        if (rc<0 && errno==EINTR) continue;
        if (rc<0) { bad=1; break; }
        for (uint32_t i=0;i<started;i++) if (pollers[i].revents) {
            char bytes[16384]; ssize_t n=read(pair[i][0],bytes,sizeof bytes);
            if (n<0 && errno==EINTR) continue;
            if (n>0) {
                if (wires[i].len+(size_t)n>LMB_CAPACITY_WIRE || cap_add(&wires[i],bytes,(size_t)n)) { bad=1; break; }
            } else {
                if (n<0) bad=1;
                close(pair[i][0]); pair[i][0]=-1; remaining--;
                LmbCapacitySample *s=&r->samples[round*r->clients+i]; s->completion=nowd()-began;
                api_capacity_parse(&wires[i],s);
            }
        }
    }
    if (remaining || api_stopping) bad=1;
    for (uint32_t i=0;i<r->clients;i++) {
        if (pair[i][0]>=0) close(pair[i][0]);
        if (children[i]) {
            if (bad) kill(children[i],SIGTERM);
            /* Only our own probe children. A hung cleanup is bounded too. */
            int status=0; double until=nowd()+2; pid_t got;
            while ((got=waitpid(children[i],&status,WNOHANG))==0 && nowd()<until) (void)poll(NULL,0,20);
            if (got==0) { kill(children[i],SIGKILL); do { got=waitpid(children[i],&status,0); } while (got<0 && errno==EINTR); bad=1; }
            if (got!=children[i] || !WIFEXITED(status) || WEXITSTATUS(status)) bad=1;
        }
        free(wires[i].p);
    }
    return bad ? -1 : 0;
}
static int api_capacity_json(const LmbCapacity *r,const char *state,const LmbCapacityCost *cost) {
    Cap body={0}; char hash[65]; lmb_hex(hash,r->fingerprint,32);
    int bad=api_addf(&body,"{\"schema\":1,\"scope\":\"local_owner_gateway_bursts\",\"production_capacity_certified\":false,"
        "\"fingerprint\":\"%s\",\"state\":",hash) || api_json_text(&body,state) ||
        api_addf(&body,",\"clients\":%u,\"rounds\":%u,\"max_tokens\":%u,\"measured_at\":%llu,\"stable\":%s,\"cost\":",
            r->clients,r->rounds,r->max_new,(unsigned long long)r->measured_at,r->stable ? "true" : "false");
    if (!bad) bad=cost->known ? api_addf(&body,"{\"scope\":\"declared_whole_machine_footprint_not_bill\","
        "\"currency\":\"%.3s\",\"micro_per_hour\":%llu}",cost->currency,(unsigned long long)cost->micro_per_hour) : cap_str(&body,"null");
    if (!bad) bad=cap_str(&body,",\"models\":[");
    for (uint32_t m=0;!bad && m<r->models;m++) {
        LmbCapacitySummary s=lmb_capacity_summary(r,m); lmb_hex(hash,r->allocations[m],32);
        bad=api_addf(&body,"%s{\"allocation\":\"%s\",\"requests\":%u,\"completed\":%u,\"rejected\":%u,\"failed\":%u,"
            "\"timing_samples\":%u,\"ttft_p95_seconds\":",m ? "," : "",hash,s.requests,s.completed,s.rejected,s.failed,s.measured);
        if (!bad) bad=s.measured ? api_addf(&body,"%.9f,\"gap_p95_of_response_p95_seconds\":%.9f,\"completion_p95_seconds\":%.9f}",
            s.ttft_p95,s.gap_p95_of_response_p95,s.completion_p95) : cap_str(&body,"null,\"gap_p95_of_response_p95_seconds\":null,\"completion_p95_seconds\":null}");
    }
    if (!bad) bad=cap_str(&body,"]}\n");
    if (!bad) bad=fputs(body.p,stdout)==EOF;
    free(body.p); return bad ? 1 : 0;
}
static int api_capacity_uint(const char *s,uint32_t low,uint32_t high,uint32_t *out) {
    const char *p=s; uint32_t n;
    if (lmb_response_uint(&p,&n) || *p || n<low || n>high) return -1;
    *out=n; return 0;
}
static int api_capacity_measure(int access,const char *tracker,char *const *args,unsigned count) {
    LmbCapacity r={0}; LmbCapacityCost cost={0};
    if (count<5 || count>4+LMB_CAPACITY_MODELS || !lmb_api_username(args[0]) ||
        api_capacity_uint(args[1],1,LMB_CAPACITY_CLIENTS,&r.clients) ||
        api_capacity_uint(args[2],1,LMB_CAPACITY_ROUNDS,&r.rounds) ||
        api_capacity_uint(args[3],2,512,&r.max_new)) return 2;
    r.models=count-4; if (r.models>r.clients) return 2;
    for (uint32_t m=0;m<r.models;m++) {
        if (strlen(args[m+4])!=64 || lmb_unhex(r.allocations[m],args[m+4],32)) return 2;
        for (uint32_t j=0;j<m;j++) if (!memcmp(r.allocations[j],r.allocations[m],32)) return 2;
    }
    int dir=lmb_capacity_dir(access,1); if (dir<0) return 1;
    int guard=lmb_capacity_lock(dir); if (guard<0) { close(dir); fprintf(stderr,"Capacity probe already running or storage unsafe\n"); return 1; }
    int bad=lmb_capacity_room(dir,args[0]);
    r.measured_at=(uint64_t)time(NULL); r.stable=1; r.count=r.clients*r.rounds;
    for (uint32_t i=0;i<r.count;i++) { r.samples[i].model=(i%r.clients)%r.models; r.samples[i].round=i/r.clients; }
    if (!bad) bad=api_capacity_snapshot(tracker,&r,r.fingerprint,&cost);
    if (bad) { fprintf(stderr,"Capacity probe needs a new record name and live approved allocations with current inventory\n"); close(guard); close(dir); return 1; }
    struct sigaction stop={0},old_int,old_term; stop.sa_handler=api_stop; sigemptyset(&stop.sa_mask);
    sigaction(SIGINT,&stop,&old_int); sigaction(SIGTERM,&stop,&old_term); api_stopping=0;
    fprintf(stderr,"[capacity] real inference: %u clients, %u rounds, at most %u output tokens/request; no models will be loaded or released\n",
        r.clients,r.rounds,r.max_new);
    for (uint32_t round=0;!bad && round<r.rounds;round++) {
        bad=api_capacity_round(access,tracker,&r,round,guard);
        uint8_t current[32]; LmbCapacityCost latest={0};
        if (!bad && (api_capacity_snapshot(tracker,&r,current,&latest) || memcmp(current,r.fingerprint,32))) bad=1;
        fprintf(stderr,"[capacity] round %u/%u %s\n",round+1,r.rounds,bad ? "interrupted or changed" : "recorded");
    }
    sigaction(SIGINT,&old_int,NULL); sigaction(SIGTERM,&old_term,NULL);
    if (bad) r.stable=0;
    int saved=lmb_capacity_save(dir,args[0],&r); close(guard); close(dir);
    int printed=api_capacity_json(&r,saved ? "record_not_saved" : bad ? "incomplete" : "measured",&cost);
    return bad || saved || printed ? 1 : 0;
}
static int api_capacity_check(int access,const char *tracker,char *const *args,unsigned count) {
    uint32_t first,gap,age;
    if (count!=4 || !lmb_api_username(args[0]) || api_capacity_uint(args[1],1,3600000,&first) ||
        api_capacity_uint(args[2],1,3600000,&gap) || api_capacity_uint(args[3],1,86400,&age)) return 2;
    int dir=lmb_capacity_dir(access,0); if (dir<0) return 1;
    LmbCapacity r; int rc=lmb_capacity_load(dir,args[0],&r); close(dir); if (rc) return 1;
    uint8_t current[32]; LmbCapacityCost cost={0};
    const char *state=api_capacity_snapshot(tracker,&r,current,&cost) ? "inventory_or_runtime_unavailable" :
        lmb_capacity_evaluate(&r,current,(uint64_t)time(NULL),age,first/1000.0,gap/1000.0);
    return api_capacity_json(&r,state,&cost);
}
/* Read-only bounded global choice among whole, actually measured portfolios.
 * The first candidate is the operator's static baseline. No profile from one
 * mix is composed with another: shared CPU/network interference is retained.
 * This does not prepare new placements, change routing or release capacity. */
static int api_capacity_select(int access,const char *tracker,char *const *args,unsigned count) {
    uint32_t first,gap,age;
    if (count<5 || count>3+LMB_CAPACITY_MODELS || api_capacity_uint(args[0],1,3600000,&first) ||
        api_capacity_uint(args[1],1,3600000,&gap) || api_capacity_uint(args[2],1,86400,&age)) return 2;
    int dir=lmb_capacity_dir(access,0); if (dir<0) return 1;
    LmbCapacity *records=calloc(count-3,sizeof *records); LmbCapacityCost costs[LMB_CAPACITY_MODELS]={0};
    const char *states[LMB_CAPACITY_MODELS]={0}; int eligible[LMB_CAPACITY_MODELS]={0};
    int prices=1,baseline_ok=0; Cap body={0};
    if (!records) { close(dir); return 1; }
    for (unsigned i=0;i<count-3;i++) {
        uint8_t current[32];
        if (lmb_capacity_load(dir,args[i+3],&records[i])) { states[i]="record_unavailable"; continue; }
        if (!i) baseline_ok=1;
        if (!baseline_ok || !lmb_capacity_comparable(&records[0],&records[i])) { states[i]="workload_not_comparable"; continue; }
        states[i]=api_capacity_snapshot(tracker,&records[i],current,&costs[i]) ? "inventory_or_runtime_unavailable" :
            lmb_capacity_evaluate(&records[i],current,(uint64_t)time(NULL),age,first/1000.0,gap/1000.0);
        if (strcmp(states[i],"observed_workload_passed")) continue;
        eligible[i]=1;
    }
    close(dir);
    int best=lmb_capacity_choose(costs,eligible,count-3,&prices);
    int bad=api_addf(&body,"{\"schema\":1,\"scope\":\"measured_existing_portfolios\",\"applied\":false,"
        "\"production_capacity_certified\":false,\"objective\":\"%s\",\"selected\":",
        prices ? "declared_machine_footprint_subject_to_observed_latency" : "operator_order_prices_unknown_or_mixed");
    if (!bad) bad=best<0 ? cap_str(&body,"null") : api_json_text(&body,args[best+3]);
    if (!bad) bad=cap_str(&body,",\"baseline\":") || api_json_text(&body,args[3]) || cap_str(&body,",\"candidates\":[");
    for (unsigned i=0;!bad && i<count-3;i++) {
        bad=(i && cap_str(&body,",")) || cap_str(&body,"{\"name\":") || api_json_text(&body,args[i+3]) ||
            cap_str(&body,",\"state\":") || api_json_text(&body,states[i]) || cap_str(&body,",\"declared_machine_cost\":");
        if (!bad) bad=costs[i].known ? api_addf(&body,"{\"currency\":\"%.3s\",\"micro_per_hour\":%llu}",
            costs[i].currency,(unsigned long long)costs[i].micro_per_hour) : cap_str(&body,"null");
        if (!bad) bad=cap_str(&body,"}");
    }
    if (!bad) bad=cap_str(&body,"]}\n") || fputs(body.p,stdout)==EOF;
    free(body.p); free(records); return bad ? 1 : 0;
}
/* Explicit owner action, never an automatic consequence of measurement.
 * Replace future-turn routing for the entire ordered measured model mix.
 * All selected allocations must already be approved and resident. Existing
 * turns continue with their original contract; no model is prepared/retired.
 * The registry publication is atomic even if a revision changed concurrently.
 * Return an HTTP status so CLI and authenticated service share this operation. */
typedef struct {
    uint8_t record_digest[32];
    LmbCapacityCost price;
    int (*before_publish)(void *,const LmbModelRoute *,uint32_t);
    void *opaque;
} ApiCapacityGuard;
static int api_capacity_guard_price(const ApiCapacityGuard *guard,const LmbCapacityCost *cost) {
    return !guard || (cost->known && guard->price.known &&
        cost->micro_per_hour==guard->price.micro_per_hour && !memcmp(cost->currency,guard->price.currency,4));
}
static int api_capacity_apply_checked(int access,const char *tracker,char *const *args,unsigned count,Cap *body,
    const ApiCapacityGuard *guard) {
    uint32_t first,gap,age;
    if (count<5 || count>4+LMB_CAPACITY_MODELS || !lmb_api_username(args[0]) ||
        api_capacity_uint(args[1],1,3600000,&first) || api_capacity_uint(args[2],1,3600000,&gap) ||
        api_capacity_uint(args[3],1,86400,&age)) return 400;
    LmbCapacity *record=calloc(1,sizeof *record);
    LmbModelRoute *routes=calloc(count-4,sizeof *routes);
    uint32_t expected[LMB_CAPACITY_MODELS]={0}; int status=503,records=-1,dir=-1;
    if (!record || !routes) goto done;
    records=lmb_capacity_dir(access,0); dir=lmb_route_dir(access,0);
    if (records<0 || dir<0 || lmb_capacity_load(records,args[0],record)) goto done;
    if (guard) {
        LmbBuf b={0}; uint8_t hash[32]; LmbSha sha;
        int bad=lmb_capacity_encode(record,&b);
        if (!bad) { lmb_sha_init(&sha); lmb_sha_update(&sha,b.p,b.len); lmb_sha_final(&sha,hash); }
        free(b.p);
        if (bad || memcmp(hash,guard->record_digest,32)) { status=409; goto done; }
    }
    if (record->models!=count-4) { status=400; goto done; }
    for (uint32_t m=0;m<record->models;m++) {
        char hex[65]; uint8_t id[32]; size_t n=strlen(args[m+4]);
        if (n<66 || n>75 || args[m+4][64]!=':') { status=400; goto done; }
        memcpy(hex,args[m+4],64); hex[64]=0;
        if (lmb_unhex(id,hex,32) || api_capacity_uint(args[m+4]+65,1,UINT32_MAX-1,&expected[m])) { status=400; goto done; }
        for (uint32_t j=0;j<m;j++) if (!memcmp(routes[j].id,id,32)) { status=400; goto done; }
        if (lmb_route_load(dir,id,&routes[m])) goto done;
        if (routes[m].revision!=expected[m]) { status=409; goto done; }
        if (memcmp(routes[m].content,record->contents[m],32) || routes[m].numeric_abi!=record->numeric_abi[m] ||
            strcmp(routes[m].numeric_class,record->numeric_class[m]) || strcmp(routes[m].tracker,tracker)) { status=409; goto done; }
    }
    uint8_t current[32]; LmbCapacityCost cost={0};
    const char *state=api_capacity_snapshot(tracker,record,current,&cost) ? "inventory_or_runtime_unavailable" :
        lmb_capacity_evaluate(record,current,(uint64_t)time(NULL),age,first/1000.0,gap/1000.0);
    if (strcmp(state,"observed_workload_passed")) {
        status=422;
        if (cap_str(body,"{\"applied\":false,\"state\":") || api_json_text(body,state) || cap_str(body,"}")) status=500;
        goto done;
    }
    if (!api_capacity_guard_price(guard,&cost)) { status=409; goto done; }
    for (uint32_t m=0;m<record->models;m++) {
        char allocation[65]; lmb_hex(allocation,record->allocations[m],32);
        const char *members[]={allocation};
        uint32_t context=routes[m].context,max_new=routes[m].max_new;
        routes[m].revision++; routes[m].policy=LMB_ROUTE_ORDERED;
        if (api_route_build(tracker,members,1,&routes[m],1)) goto done;
        /* Do not silently change a public model's context/output limits. */
        if (context>routes[m].context || max_new>routes[m].max_new) { status=409; goto done; }
        routes[m].context=context; routes[m].max_new=max_new;
    }
    /* Host handshakes may take time: evidence is revalidated immediately
     * before publication. Normal admission still checks every future turn. */
    if (api_capacity_snapshot(tracker,record,current,&cost) || !api_capacity_guard_price(guard,&cost) ||
        strcmp(lmb_capacity_evaluate(record,current,(uint64_t)time(NULL),age,first/1000.0,gap/1000.0),
            "observed_workload_passed")) { status=409; goto done; }
    char fingerprint[65]; lmb_hex(fingerprint,record->fingerprint,32);
    if (cap_str(body,"{\"schema\":1,\"applied\":true,\"scope\":\"future_turn_routing\","
        "\"production_capacity_certified\":false,\"weights_loaded\":false,\"weights_released\":false,\"evidence_record\":") ||
        api_json_text(body,args[0]) || api_addf(body,",\"fingerprint\":\"%s\",\"models\":[",fingerprint)) { status=500; goto done; }
    for (uint32_t m=0;m<record->models;m++) if (api_route_record(body,&routes[m],m!=0,1)) { status=500; goto done; }
    if (cap_str(body,"]}")) { status=500; goto done; }
    if (guard && guard->before_publish && guard->before_publish(guard->opaque,routes,record->models)) goto done;
    int rc=lmb_route_save_many(dir,routes,expected,record->models);
    status=rc==LMB_ROUTE_CONFLICT || rc==LMB_ROUTE_MISSING ? 409 : rc ? 503 : 200;
    if (status==200) fprintf(stderr,"[capacity-apply] record=%s models=%u fingerprint=%s routing=atomic weights=unchanged\n",
        args[0],record->models,fingerprint);
done:
    if (status!=200 && status!=422) { free(body->p); memset(body,0,sizeof *body); }
    if (records>=0) close(records);
    if (dir>=0) close(dir);
    free(routes); free(record); return status;
}
static int api_capacity_apply(int access,const char *tracker,char *const *args,unsigned count,Cap *body) {
    return api_capacity_apply_checked(access,tracker,args,count,body,NULL);
}
/* Called only after bearer management authority, origin and framing checks.
 * No paths, endpoints, model weights or donor consents can enter this schema. */
static void api_capacity_apply_http(int fd,int access,const char *tracker,const char *body,size_t size) {
    LmbJsonToken tokens[96]; LmbJson j; int fields[5];
    const char *const keys[]={"record","ttft_ms","gap_ms","max_age_seconds","models"};
    char values[4+LMB_CAPACITY_MODELS][80],*args[4+LMB_CAPACITY_MODELS];
    int bad=size>2048 || lmb_json_parse(&j,body,size,tokens,96) || lmb_json_fields(&j,0,keys,5,fields);
    if (!bad) for (unsigned i=0;i<5;i++) if (fields[i]<0) bad=1;
    if (!bad) bad=lmb_api_json_string(&j,(unsigned)fields[0],values[0],33) || !lmb_api_username(values[0]);
    for (unsigned i=1;!bad && i<4;i++) {
        uint32_t n; bad=lmb_json_uint(&j,(unsigned)fields[i],&n);
        if (!bad) snprintf(values[i],sizeof values[i],"%u",n);
    }
    unsigned count=0;
    if (!bad) {
        const LmbJsonToken *list=&tokens[fields[4]];
        bad=list->kind!=LMB_JSON_ARRAY || !list->children || list->children>LMB_CAPACITY_MODELS;
        for (unsigned at=(unsigned)fields[4]+1;!bad && at<list->next;at=tokens[at].next) {
            const char *const model_keys[]={"id","revision"}; int model[2]; char id[65]; uint8_t identity[32]; uint32_t revision;
            bad=lmb_json_fields(&j,at,model_keys,2,model) || model[0]<0 || model[1]<0 ||
                lmb_api_json_string(&j,(unsigned)model[0],id,sizeof id) ||
                lmb_json_uint(&j,(unsigned)model[1],&revision) || strlen(id)!=64 || lmb_unhex(identity,id,32) ||
                !revision || revision==UINT32_MAX;
            if (!bad) snprintf(values[4+count++],sizeof values[0],"%s:%u",id,revision);
        }
    }
    if (bad) { api_error(fd,400,"invalid_capacity_application"); return; }
    for (unsigned i=0;i<count+4;i++) args[i]=values[i];
    Cap response={0}; int status=api_capacity_apply(access,tracker,args,count+4,&response);
    if (response.p) (void)api_response(fd,(unsigned)status,response.p);
    else api_error(fd,(unsigned)status,status==409 ? "model_revision_or_contract_changed" :
        status==400 ? "invalid_capacity_application" : "capacity_application_not_confirmed");
    free(response.p);
}
#endif
