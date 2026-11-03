/* Native preparation owner. The console hands off an exact reviewed plan;
 * this keeper owns source/leases, while donors alone authorize allocations. */
#ifndef LMB_REQUESTER_WINDOWS_H
#define LMB_REQUESTER_WINDOWS_H
#include "src/planner/lumabri_catalogue_data.h"
#include "src/runtime/lumabri_source_inspect.h"
#include "src/runtime/lumabri_resident_plan_windows.h"
#include "src/runtime/lumabri_prepare_progress.h"
#include "lumabri_checkpoint_identity.h"
#include "src/runtime/lumabri_calibration_windows.h"
#include "src/planner/lumabri_portfolio_snapshot.h"
#include "src/runtime/lumabri_joint_review.h"
static int home_control_io_ms(int fallback) {
    int ms=lmb_env_int("LUMABRI_IO_TIMEOUT_MS",fallback,100,600000);return ms>10000 ? 10000 : ms;
}
#include "src/runtime/lumabri_home_request_plan.h"

static int catalog_refresh_windows(LmbTuiState *st,void *unused) {
    (void)unused;
    CatalogEntry *entries=calloc(LMB_TUI_MAX_MODELS,sizeof *entries);
    if (!entries) return -1;
    int count=catalog_scan(st->root,entries,LMB_TUI_MAX_MODELS);
    int rc=catalog_inventory(st);st->nmodels=count;
    (void)catalog_build_windows(st->build_id);
    for (int i=0;i<count;i++) {
        LmbClusterNode selected[LMB_CLUSTER_MAX_NODES];uint32_t mapping[LMB_CLUSTER_MAX_NODES],n;
        LmbTuiModel *m=&st->models[i];
        int household=catalog_plan_entry(st,m,&entries[i],selected,mapping,&n);
        if (!lmb_checkpoint_identity(m->dir,m->content_id,NULL,NULL) &&
            !catalog_calibration_load_windows(m->content_id,&m->calibration)) {
            m->has_calibration=1;
            catalog_calibration_apply(st,m,household,selected,mapping,n);
        }
    }
    catalog_advice_refresh(st);catalog_joint_refresh(st);free(entries);return rc;
}

/* Deterministic review fingerprint: file metadata and the exact assignments,
 * identities/runtime, context and token limit. A fresh inventory may reduce
 * headroom, but cannot silently substitute a different approved placement. */
typedef struct {uint8_t hash[32];} LmbReviewFiles;
static int home_review_file(const char *path,const char *rel,const LmbSourceStat *s,void *context) {
    (void)path;LmbReviewFiles *files=context;LmbBuf b={0};uint8_t hash[32];LmbSha sha;
    int rc=lmb_buf_str(&b,rel)||lmb_buf_u64(&b,s->size)||lmb_buf_u64(&b,s->device)||
        lmb_buf_u64(&b,s->inode)||lmb_buf_u64(&b,s->mtime_ns)||lmb_buf_u64(&b,s->ctime_ns);
    if (!rc) {
        lmb_sha_init(&sha);lmb_sha_update(&sha,b.p,b.len);lmb_sha_final(&sha,hash);
        for (unsigned i=0;i<32;i++) files->hash[i]^=hash[i];
    }
    free(b.p);return rc ? -1 : 0;
}
static int home_review_digest_windows(const LmbTuiState *st,int selected,char hex[65]) {
    if (selected<0 || selected>=st->nmodels) return -1;
    const LmbTuiModel *m=&st->models[selected];const LmbClusterPlan *p=&m->plan;
    if (!m->planned || !m->checkpoint_inventory_ok || !m->weights_present ||
        p->state!=LMB_PLAN_RESIDENT || !p->nslices || p->nslices>LMB_CLUSTER_MAX_NODES || p->edge_node>=st->nnodes) return -1;
    LmbReviewFiles files={0};LmbCheckpointInventory inventory;
    if ((memset(&inventory,0,sizeof inventory),lmb_checkpoint_walk(m->dir,"",0,&inventory,home_review_file,&files)) || inventory.bytes!=m->checkpoint_bytes) return -1;
    LmbBuf b={0};int rc=lmb_buf_str(&b,"LMB-WINDOWS-REVIEW-1")||lmb_buf_str(&b,st->tracker)||
        lmb_buf_str(&b,m->dir)||lmb_buf_bytes(&b,files.hash,32)||lmb_buf_u64(&b,m->checkpoint_bytes)||
        lmb_buf_u32(&b,st->context)||lmb_buf_u32(&b,st->sessions)||lmb_buf_u32(&b,st->max_new)||
        lmb_buf_u32(&b,p->hybrid)||lmb_buf_u32(&b,p->nslices)||lmb_buf_str(&b,st->identities[p->edge_node]);
    for (uint32_t i=0;!rc && i<p->nslices;i++) {
        const LmbSlice *slice=&p->slices[i];uint32_t j=slice->node;
        if (j>=st->nnodes || !lmb_tui_node_enabled(st,j) || !st->identities[j][0] || !st->runtime_ids[j][0]) {rc=-1;break;}
        rc=lmb_buf_str(&b,st->identities[j])||lmb_buf_str(&b,st->runtime_ids[j])||
            lmb_buf_str(&b,st->nodes[j].addr)||lmb_buf_u32(&b,st->nodes[j].threads)||
            lmb_buf_u32(&b,slice->layer_begin)||lmb_buf_u32(&b,slice->layer_end)||lmb_buf_u64(&b,slice->bytes_resident);
    }
    if (!rc) {uint8_t hash[32];LmbSha sha;lmb_sha_init(&sha);lmb_sha_update(&sha,b.p,b.len);lmb_sha_final(&sha,hash);lmb_hex(hex,hash,32);}
    free(b.p);return rc ? -1 : 0;
}

static int home_review_selection_windows(const LmbTuiState *st,int selected,char hex[65]) {
    if (selected>=0) return home_review_digest_windows(st,selected,hex);
    if (!st->joint.ready || !st->joint.plan.model_count) return -1;
    LmbTuiState *one=malloc(sizeof *one);if (!one) return -1;
    LmbSha sha;uint8_t hash[32];int rc=0;lmb_sha_init(&sha);
    lmb_sha_le32(&sha,st->joint.plan.model_count);
    for (uint32_t i=0;i<st->joint.plan.model_count;i++) {
        *one=*st;char digest[65],why[384];
        int index=home_joint_prepare_next(st,one,i,why,sizeof why);
        if (index<0 || home_review_digest_windows(one,index,digest)) {rc=-1;break;}
        lmb_sha_update(&sha,digest,64);
    }
    free(one);if (rc) return -1;
    lmb_sha_final(&sha,hash);lmb_hex(hex,hash,32);return 0;
}

static int home_sibling_windows(const wchar_t *name,char *out,size_t cap) {
    wchar_t wide[2048];DWORD n=GetModuleFileNameW(NULL,wide,2048);
    if (!n || n>=2048) return -1;
    wchar_t *slash=wcsrchr(wide,L'\\');
    if (!slash || (size_t)(slash-wide)+1+wcslen(name)>=2048) return -1;
    wcscpy(slash+1,name);
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,out,(int)cap,NULL,NULL) ? 0 : -1;
}
static int home_source_start_windows(LmbOwnedProcess *source,const LmbTuiState *st,const LmbTuiModel *m,
    const char *model,const char *key,const char *id,char log_path[1200]) {
    char binary[6144],ip[64],port_text[20],address[64],name[64],logs[1100];
    if (home_sibling_windows(L"maintainer.exe",binary,sizeof binary) ||
        lmb_private_default_path(logs,sizeof logs,"logs") || lmb_private_mkdir(logs)) return -1;
    LmbSocket route=lmb_connect_ms_io(st->tracker,1500,1500);
    if (route==LMB_INVALID_SOCKET) return -1;
    struct sockaddr_in local;LmbSocketLength len=sizeof local;
    int rc=getsockname(route,(struct sockaddr *)&local,&len) || local.sin_family!=AF_INET ||
        !inet_ntop(AF_INET,&local.sin_addr,ip,sizeof ip);lmb_close(route);
    if (rc) return -1;
    int port=0;LmbSocket listener=lmb_home_listen_service(&port);
    if (listener==LMB_INVALID_SOCKET) return -1;
    snprintf(port_text,sizeof port_text,"%d",port);snprintf(address,sizeof address,"%.48s:%d",ip,port);
    snprintf(name,sizeof name,"home-source-%.16s",id);
    int n=snprintf(log_path,1200,"%s\\%s.log",logs,name);
    HANDLE handoff[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE},log=INVALID_HANDLE_VALUE,ready=NULL;
    rc=-1;
    if (n<0 || n>=1200 || lmb_pipe_pair(handoff) || !(ready=CreateEventW(NULL,TRUE,FALSE,NULL))) goto done;
    log=lmb_private_open(log_path,1,1,NULL);if (log==INVALID_HANDLE_VALUE) goto done;
    LmbChildHandle inherited[]={{"LUMABRI_HOME_LISTEN_HANDLE",handoff[1]},{"LUMABRI_READY_HANDLE",ready}};
    const char *args[]={"--root",m->dir,"--port",port_text,"--tracker",st->tracker,"--name",name,
        "--model-name",model,"--advertise",address,"--key",key};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=14,.output=log,.error=log,.handles=inherited,.nhandles=2};
    if (lmb_owned_process_start(source,&launch)) goto done;
    CloseHandle(handoff[1]);handoff[1]=INVALID_HANDLE_VALUE;
    if (lmb_home_export_listener(listener,source->pid,handoff[0])) goto done;
    HANDLE waits[]={ready,source->process};
    rc=WaitForMultipleObjects(2,waits,FALSE,5000)==WAIT_OBJECT_0 ? 0 : -1;
done:
    lmb_socket_close(listener);
    if (log!=INVALID_HANDLE_VALUE) CloseHandle(log);
    if (ready) CloseHandle(ready);
    for (unsigned i=0;i<2;i++) if (handoff[i]!=INVALID_HANDLE_VALUE) CloseHandle(handoff[i]);
    return rc ? -1 : 0;
}
static int home_prepare_tick_windows(HomeService *service,const char *detail,uint32_t phase) {
    if (detail) snprintf(service->snapshot.detail,sizeof service->snapshot.detail,"%s",detail);
    service->snapshot.phase=phase;
    if (home_service_save(service)) return home_fail("Cannot persist preparation status (error %d).",errno);
    int op=home_service_poll(service);home_service_answer(service);
    return g_stopping || op==HOME_SVC_STOP || op==HOME_SVC_CANCEL ? -1 : 0;
}

static int home_prepare_execute_windows(HomeService *service,LmbTuiState *st,int selected,uint32_t result_index) {
    LmbTuiModel *m=&st->models[selected];LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES];
    uint32_t indices[LMB_CLUSTER_MAX_NODES],count=0;LmbClusterPlan plan;
    if (home_review_plan(st,m,nodes,indices,&count,&plan)) return -1;
    HomeSession *s=calloc(1,sizeof *s);if (!s) return -1;
    pthread_mutex_init(&s->status_lock,NULL);pthread_mutex_init(&s->send_lock,NULL);
    for (unsigned i=0;i<LMB_CLUSTER_MAX_NODES;i++) s->fd[i]=LMB_INVALID_SOCKET;
    LmbOwnedProcess source={0};LmbPrepareProgress progress={0};pthread_t heartbeat;
    int monitoring=0,rc=-1,prepared=0;char log[1200]="",key[1200],idhex[65],pkhex[65],model[64];
    uint8_t sk[64],pk[32],id[32],edge_pk[32];
    if (lmb_peer_identity(lmb_peer_key_path(key,sizeof key),sk,pk)) goto done;
    SecureZeroMemory(sk,sizeof sk);lmb_hex(pkhex,pk,32);lmb_random(id,32);lmb_hex(idhex,id,32);
    snprintf(model,sizeof model,"home-%.12s-%.12s-%.24s",pkhex,idhex,m->name);
    for (char *p=model;*p;p++) if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-')) *p='_';
    snprintf(service->snapshot.name,sizeof service->snapshot.name,"%s",model);
    service->snapshot.preparation_results_known=1;service->snapshot.preparation_result_count=result_index+1;
    HomePreparationResult *result=&service->snapshot.preparation_results[result_index];
    memcpy(result->allocation,id,32);snprintf(result->catalogue_name,sizeof result->catalogue_name,"%s",m->name);
    /* Operation journals use bounded ASCII labels; the catalogue and paths
     * stay UTF-8. The exact checkpoint is identified by its root and path. */
    for (char *p=result->catalogue_name;*p;p++) if ((unsigned char)*p<32 || (unsigned char)*p>126) *p='_';
    if (home_prepare_tick_windows(service,"Indexing and verifying checkpoint identity",LMB_HOME_IDLE) ||
        home_source_start_windows(&source,st,m,model,key,idhex,log)) goto done;
    LmbModelIdentity identity;Swarm swarm;double start=nowd();int found=0;
    while (nowd()-start<600) {
        char bar[29],detail[256];lmb_prepare_read(&progress,log,nowd());
        lmb_prepare_display(&progress.index,1,nowd(),bar,detail,sizeof detail);
        if (home_prepare_tick_windows(service,detail,LMB_HOME_IDLE)) goto done;
        if (!lmb_model_identity_get(st->tracker,model,&identity) &&
            !swarm_inspect(st->tracker,model,&swarm) && swarm.total_bytes) {found=1;break;}
        if (WaitForSingleObject(source.process,0)!=WAIT_TIMEOUT) break;
        Sleep(100);
    }
    if (!found) {home_fail("Checkpoint indexing did not complete. Inspect the source log.");goto done;}
    uint8_t indexed_root[32];
    if (lmb_checkpoint_identity(m->dir,m->content_id,model,indexed_root) || memcmp(indexed_root,identity.root,32)) m->content_id[0]=0;
    if (lmb_home_plan_budgets(&m->shape,swarm.total_bytes,nodes,count,st->context,&plan) || plan.state!=LMB_PLAN_RESIDENT) {
        home_fail("The indexed checkpoint no longer fits the reviewed plan. Refresh the catalogue.");goto done;
    }
    if (home_offer_plan(s,st,m,nodes,indices,&plan,pk,id,identity.root,model,swarm.total_bytes,swarm.metadata_bytes,1,edge_pk)) goto done;
    if (pthread_create(&heartbeat,NULL,home_session_keepalive,s)) goto done;
    monitoring=1;
    int committed=0,coordinator_started=0,host_started=0;double commit_at=0;
    LmbPrepareWatchdog watchdog={.started=nowd(),.advanced=nowd()};
    while (!atomic_load(&s->failed)) {
        LmbHomePhase phases[LMB_CLUSTER_MAX_NODES];uint32_t ports[LMB_CLUSTER_MAX_NODES];
        home_session_snapshot(s,phases,ports);lmb_prepare_read(&progress,log,nowd());
        unsigned advanced=0,accepted=0,ready=0;
        for (uint32_t i=0;i<s->count;i++) {advanced+=phases[i];accepted+=phases[i]==LMB_HOME_ACCEPTED;ready+=phases[i]>=LMB_HOME_SEGMENT_READY;}
        lmb_prepare_watchdog_advance(&watchdog,nowd(),progress.transfer.done,advanced);
        if (lmb_prepare_watchdog_expired(&watchdog,nowd())) {home_fail("Preparation stopped making progress; incomplete allocations were cancelled.");goto done;}
        char detail[384];snprintf(detail,sizeof detail,"%s | %u/%u ready | %.1f MB served",
            !committed ? "Waiting for every donor's approval" : !host_started ? "Loading assigned weights into RAM" : "Starting the chat host",
            ready,s->count,progress.transfer.done/1e6);
        if (home_prepare_tick_windows(service,detail,!committed ? LMB_HOME_PENDING : !host_started ? LMB_HOME_LOADING : LMB_HOME_STARTING_HOST)) goto done;
        if (!committed && accepted==s->count) {
            commit_at=nowd();
            for (uint32_t i=0;i<s->count;i++) if ((!plan.hybrid || i!=s->edge) && home_session_send(s,i,LMB_HOME_COMMIT)) goto done;
            committed=1;
        }
        if (plan.hybrid && committed && !coordinator_started) {
            int others_ready=1;
            for (uint32_t i=0;i<s->count;i++) if (i!=s->edge && phases[i]!=LMB_HOME_SEGMENT_READY) others_ready=0;
            if (others_ready) {
                if (home_session_hybrid_routes(s) || home_session_send(s,s->edge,LMB_HOME_COMMIT)) goto done;
                coordinator_started=1;
            }
        }
        if (committed && !host_started && ready==s->count) {
            if (home_session_send(s,s->edge,LMB_HOME_START_HOST)) goto done;
            host_started=1;
        }
        if (host_started && phases[s->edge]==LMB_HOME_READY && ports[s->edge]) {
            LmbResidentPlan saved={.context=st->context,.sessions=st->sessions,.max_new=s->offers[s->edge].max_new,
                .preparation_seconds=nowd()-commit_at,.prepared_at=(double)time(NULL)};
            if (saved.preparation_seconds<=0) saved.preparation_seconds=.001;
            saved.execution.count=s->count;saved.execution.layers=m->shape.layers;saved.execution.hybrid=plan.hybrid;
            for (uint32_t i=0;i<s->count;i++) {
                LmbExecutionNode *n=&saved.execution.nodes[i];
                snprintf(n->name,sizeof n->name,"%s",s->names[i]);snprintf(n->address,sizeof n->address,"%s",s->addresses[i]);
                n->begin=s->offers[i].begin;n->end=s->offers[i].end;n->edge=s->offers[i].runs_edge;n->reserved_bytes=s->offers[i].ram_bytes;
            }
            const char *colon=strrchr(s->addresses[s->edge],':');if (!colon) goto done;
            snprintf(saved.host,sizeof saved.host,"%.*s:%u",(int)(colon-s->addresses[s->edge]),s->addresses[s->edge],ports[s->edge]);
            snprintf(saved.tracker,sizeof saved.tracker,"%s",st->tracker);snprintf(saved.model,sizeof saved.model,"%s",model);
            snprintf(saved.content_id,sizeof saved.content_id,"%s",m->content_id);
            lmb_hex(saved.host_key,edge_pk,32);lmb_hex(saved.root,identity.root,32);
            memcpy(saved.allocation,id,32);memcpy(saved.peer_keys,s->peer_keys,s->count*32);
            LmbCalKey observation_key;
            if (!catalog_calibration_key(st,m,0,NULL,&observation_key)) {
                snprintf(saved.observation.build_id,sizeof saved.observation.build_id,"%s",observation_key.build_id);
                snprintf(saved.observation.adapter,sizeof saved.observation.adapter,"%s",observation_key.adapter);
                saved.observation.goal=observation_key.goal;
                for (uint32_t i=0;i<s->count;i++) {
                    snprintf(saved.observation.hardware[i],65,"%s",observation_key.node_hardware_id[i]);
                    snprintf(saved.observation.runtime[i],65,"%s",observation_key.node_build_id[i]);
                    saved.observation.threads[i]=observation_key.threads[i];
                }
            }
            if (home_resident_library_save(&saved)) {home_fail("Cannot save the approved resident plan. No chat was started.");goto done;}
            prepared=1;result->ready_observed=1;service->snapshot.phase=LMB_HOME_READY;rc=0;break;
        }
        if (WaitForSingleObject(source.process,0)!=WAIT_TIMEOUT) {home_fail("The checkpoint source exited during preparation.");goto done;}
        Sleep(50);
    }
done:
    if (monitoring) {atomic_store(&s->stop,1);pthread_join(heartbeat,NULL);}
    if (atomic_load(&s->failed)) rc=-1;
    if (rc && !home_error[0]) for (uint32_t i=0;i<s->count;i++) if (s->reason[i][0]) {home_fail("%.64s: %.160s",s->names[i],s->reason[i]);break;}
    home_session_close(s,prepared);
    if (source.process) {
        if (lmb_owned_process_stop(&source,0,5000) || lmb_owned_process_close(&source)) rc=-1;
    }
    pthread_mutex_destroy(&s->send_lock);pthread_mutex_destroy(&s->status_lock);free(s);
    if (rc && !home_error[0]) home_fail("Preparation did not finish. Inspect the private source log; incomplete allocations were cancelled.");
    return rc;
}

/* Internal arguments contain no credential. They select a reviewed snapshot,
 * never a fresh unreviewed plan; the keeper inherits the active household key. */
static int home_prepare_entry_windows(int argc,char **argv) {
    if (!getenv("LUMABRI_READY_HANDLE")) return 2;
    LmbTuiState *st=calloc(1,sizeof *st);if (!st) return 1;
    char models[LMB_PORTFOLIO_MODELS][512]={{0}},review[65]="";uint32_t nodes=0,model_count=0;int rc=1,joint=0;
    for (int i=0;i<argc;i+=2) {
        if (i+1>=argc) goto invalid;
        const char *value=argv[i+1];size_t cap=0;char *dest=NULL;
        if (!strcmp(argv[i],"--root")) {dest=st->root;cap=sizeof st->root;}
        else if (!strcmp(argv[i],"--tracker")) {dest=st->tracker;cap=sizeof st->tracker;}
        else if (!strcmp(argv[i],"--model") && model_count<LMB_PORTFOLIO_MODELS) {dest=models[model_count++];cap=512;}
        else if (!strcmp(argv[i],"--joint") && !strcmp(value,"1")) {joint=1;continue;}
        else if (!strcmp(argv[i],"--review")) {dest=review;cap=sizeof review;}
        else if (!strcmp(argv[i],"--node") && nodes<LMB_CLUSTER_MAX_NODES) {dest=st->selected_nodes[nodes++];cap=65;}
        else if (!strcmp(argv[i],"--context") || !strcmp(argv[i],"--sessions") || !strcmp(argv[i],"--max-new")) {
            char *end;unsigned long n=strtoul(value,&end,10);
            if (!*value || *end || !n || n>1048576) goto invalid;
            if (!strcmp(argv[i],"--context")) st->context=(uint32_t)n;
            else if (!strcmp(argv[i],"--sessions")) st->sessions=(uint32_t)n;
            else st->max_new=(uint32_t)n;
            continue;
        } else goto invalid;
        if (!*value || strlen(value)>=cap) goto invalid;
        strcpy(dest,value);
    }
    if (!model_count || (!joint && model_count!=1) || !*st->tracker || !*st->root || strlen(review)!=64 || !nodes || !st->context ||
        !st->sessions || st->sessions>LMB_HOST_MAX_SESSIONS || !st->max_new) goto invalid;
    if (_putenv_s("LUMABRI_ENCRYPT","1") || _putenv_s("LUMABRI_RESIDENT_REQUIRED","1") || lmb_secure_init() || !lmb_secure_enabled()) goto invalid;
    HomeService service;
    if (home_service_open(&service,"prepare")) {rc=errno==EBUSY ? 75 : 1;goto invalid;}
    snprintf(service.snapshot.tracker,sizeof service.snapshot.tracker,"%s",st->tracker);
    snprintf(service.snapshot.name,sizeof service.snapshot.name,"Preparing resident model");
    snprintf(service.snapshot.detail,sizeof service.snapshot.detail,"Revalidating the exact reviewed plan");
    if (home_service_save(&service) || lmb_ready_notify()) {home_service_close(&service);goto invalid;}
    home_error[0]=0;char actual[65];int selected=-1;
    if (!catalog_refresh_windows(st,NULL)) {
        for (uint32_t m=0;m<model_count;m++) {
            int found=-1;
            for (int i=0;i<st->nmodels;i++) if (!strcmp(st->models[i].dir,models[m])) {found=i;break;}
            if (found<0) {selected=-1;break;}
            selected=found;
            if (joint) {
                _Static_assert(sizeof st->selected_models[0]==sizeof models[0],"reviewed model path size");
                /* Each row was length-checked and zero-initialized above. */
                memcpy(st->selected_models[m],models[m],sizeof models[m]);
            }
        }
        if (joint) catalog_joint_refresh(st);
    }
    if (selected<0 || home_review_selection_windows(st,joint ? -1 : selected,actual) || strcmp(actual,review))
        home_fail("The reviewed plan changed. Refresh the catalogue; no allocation was requested.");
    else if (!joint) rc=home_prepare_execute_windows(&service,st,selected,0);
    else {
        LmbTuiState *current=malloc(sizeof *current);
        if (!current) home_fail("Cannot allocate the joint preparation snapshot.");
        else {
            rc=0;
            for (uint32_t m=0;m<st->joint.plan.model_count;m++) {
                *current=*st;char why[384];
                if (home_prepare_tick_windows(&service,"Preparing the next reviewed model",LMB_HOME_IDLE) || catalog_inventory(current)) {
                    rc=home_fail("Joint preparation cancelled or inventory unavailable. Ready models remain loaded.");break;
                }
                int index=home_joint_prepare_next(st,current,m,why,sizeof why);
                if (index<0) {rc=home_fail("%s",why);break;}
                rc=home_prepare_execute_windows(&service,current,index,m);if (rc) break;
            }
            free(current);
        }
    }
    service.snapshot.state=rc ? HOME_SVC_FAILED : HOME_SVC_DONE;
    snprintf(service.snapshot.detail,sizeof service.snapshot.detail,"%s",rc ? home_error : "Approved models are ready. Open Resident models; weights remain until their owners release them.");
    if (home_service_save(&service)) rc=1;
    /* Keep the same endpoint through terminal observation. No journal PID is
     * used to control this keeper, and no operation is replayed on restart. */
    double until=nowd()+2;
    while (nowd()<until) {int op=home_service_poll(&service);home_service_answer(&service);if (op==HOME_SVC_STOP) break;Sleep(20);}
    home_service_close(&service);
invalid:
    free(st);return rc;
}
/* A completed keeper briefly serves its terminal snapshot. Closing that
 * observer endpoint never releases donors; only unfinished work is busy. */
static int home_prepare_idle_windows(HomeServiceSnapshot *running) {
    uint64_t until=GetTickCount64()+3000;
    for (;;) {
        HomeServiceSnapshot current,reply;
        if (home_service_query("prepare",HOME_SVC_STATUS,NULL,&current)) return 0;
        if (current.state==HOME_SVC_RUNNING) {if (running) *running=current;return 1;}
        if (home_service_query("prepare",HOME_SVC_STOP,&current,&reply) && GetTickCount64()>=until) return -1;
        if (GetTickCount64()>=until) return -1;
        Sleep(30);
    }
}
static int home_prepare_start_windows(LmbTuiState *st,int selected,HomeServiceSnapshot *created) {
    char review[65],context[20],sessions[20],max_new[20];
    if (home_review_selection_windows(st,selected,review) || home_service_ensure()) return -1;
    if (home_prepare_idle_windows(NULL)) {errno=EBUSY;return -1;}
    snprintf(context,sizeof context,"%u",st->context);snprintf(sessions,sizeof sessions,"%u",st->sessions);
    snprintf(max_new,sizeof max_new,"%u",st->max_new);
    const char *args[96]={"--internal-prepare","--root",st->root,
        "--tracker",st->tracker,"--context",context,"--sessions",sessions,"--max-new",max_new,"--review",review};
    size_t count=13;
    if (selected>=0) {args[count++]="--model";args[count++]=st->models[selected].dir;}
    else {
        args[count++]="--joint";args[count++]="1";
        for (uint32_t i=0;i<LMB_PORTFOLIO_MODELS;i++) if (st->selected_models[i][0]) {
            args[count++]="--model";args[count++]=st->selected_models[i];
        }
    }
    for (unsigned i=0;i<LMB_CLUSTER_MAX_NODES;i++) if (st->selected_nodes[i][0]) {args[count++]="--node";args[count++]=st->selected_nodes[i];}
    return home_service_keeper_start_owned_windows("prepare",args,count,created);
}
#endif
