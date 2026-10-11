/* Management interface to the same preparation keeper used by the TUI.
 * The catalogue root is owner configured. Review is read-only; start needs
 * an exact review fence, a durable operation ID and separate donor consent. */
#ifndef LMB_PREPARATION_API_H
#define LMB_PREPARATION_API_H
#include "lumabri_preparation_store.h"

static LmbTuiState *preparation_catalogue(const char *tracker,const LmbPreparationRequest *r) {
    HomeSettings settings; home_settings_load(&settings);
    LmbTuiState *st=calloc(1,sizeof *st); if (!st) return NULL;
    st->context=r ? r->context : 512; st->sessions=r ? r->sessions : 1; st->max_new=r ? r->max_new : 128;
    if (checked_printf(st->root,sizeof st->root,"%s",settings.models) ||
        checked_printf(st->tracker,sizeof st->tracker,"%s",tracker)) { free(st); return NULL; }
    if (r) for (uint32_t i=0;i<r->node_count;i++) strcpy(st->selected_nodes[i],r->nodes[i]);
    st->refresh=catalog_state_refresh;
    if (catalog_state_refresh(st,NULL)) { free(st); return NULL; }
    return st;
}
static void api_preparation_catalogue(int fd,const char *tracker) {
    LmbTuiState *st=preparation_catalogue(tracker,NULL); Cap body={0};
    int bad=!st || cap_str(&body,"{\"schema\":1,\"source\":\"owner_configured_folder\",\"models\":[");
    for (int i=0;st && i<st->nmodels && !bad;i++) {
        const LmbTuiModel *m=&st->models[i];
        bad=(i && cap_str(&body,",")) || cap_str(&body,"{\"name\":") || api_json_text(&body,m->name) ||
            cap_str(&body,",\"adapter\":") || api_json_text(&body,m->shape.segment_id) ||
            api_addf(&body,",\"layers\":%u,\"checkpoint_bytes\":%llu,\"source_ready\":%s,\"decode_tok_s\":null}",
                m->shape.layers,(unsigned long long)m->checkpoint_bytes,
                m->weights_present && m->checkpoint_inventory_ok && m->shape.sizing_verified ? "true" : "false");
    }
    if (!bad) bad=cap_str(&body,"]}");
    if (bad) api_error(fd,503,"catalogue_unavailable"); else (void)api_response(fd,200,body.p);
    free(body.p); free(st);
}
/* Cheap source-change fence, not a signed checkpoint identity. Weight bytes
 * are hashed by the existing source indexer before any donor approves them. */
static int preparation_file_stamp(const char *root,const char *rel,const struct stat *st,void *opaque) {
    (void)root; LmbBuf item={0},*all=opaque; uint8_t hash[32];
    int bad=lmb_buf_str(&item,rel) || lmb_buf_u64(&item,(uint64_t)st->st_dev) ||
        lmb_buf_u64(&item,(uint64_t)st->st_ino) || lmb_buf_u64(&item,(uint64_t)st->st_size) ||
        lmb_buf_u64(&item,lmb_stat_mtime_ns(st)) || lmb_buf_u64(&item,lmb_stat_ctime_ns(st));
    if (!bad) { preparation_digest(item.p,item.len,hash); bad=lmb_buf_bytes(all,hash,32); }
    free(item.p); return bad;
}
static int preparation_stamp_cmp(const void *a,const void *b) { return memcmp(a,b,32); }
static int preparation_source_stamp(const char *root,char out[65]) {
    LmbBuf files={0}; LmbCheckpointInventory inv={0}; uint8_t hash[32];
    int bad=lmb_checkpoint_walk(root,"",0,&inv,preparation_file_stamp,&files);
    if (!bad) {
        qsort(files.p,files.len/32,32,preparation_stamp_cmp);
        bad=lmb_buf_str(&files,root);
        if (!bad) { preparation_digest(files.p,files.len,hash); lmb_hex(out,hash,32); }
    }
    free(files.p); return bad;
}
/* This JSON is a second consumer of the typed plan, not its internal ABI.
 * Volatile free RAM/report ages are deliberately not a review identity;
 * exact reservations and donor admission still recheck remaining capacity. */
static int preparation_review(LmbTuiState *st,const LmbPreparationRequest *r,Cap *body,char review[65]) {
    const char *names[LMB_PREPARATION_MODELS];
    for (uint32_t i=0;i<r->model_count;i++) {
        unsigned matches=0; names[i]=r->models[i];
        for (int j=0;j<st->nmodels;j++) if (!strcmp(names[i],st->models[j].name)) matches++;
        if (matches!=1) return 422;
    }
    if (catalog_portfolio_build(st,names,r->model_count,&st->joint) || !st->joint.ready || !st->build_id[0]) return 422;
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
        const LmbClusterPlan *p=&st->joint.plan.plans[i]; char stamp[65];
        if (preparation_source_stamp(m->dir,stamp)) return 422;
        if ((i && cap_str(body,",")) || cap_str(body,"{\"name\":") || api_json_text(body,m->name) ||
            api_addf(body,",\"source_stat_fence\":\"%s\",\"checkpoint_bytes\":%llu,\"edge_node\":%u,\"slices\":[",
                stamp,(unsigned long long)m->checkpoint_bytes,p->edge_node)) return 500;
        for (uint32_t j=0;j<p->nslices;j++) {
            const LmbSlice *s=&p->slices[j];
            if (api_addf(body,"%s{\"node\":%u,\"begin\":%u,\"end\":%u,\"reserved_bytes\":%llu}",j ? "," : "",
                s->node,s->layer_begin,s->layer_end,(unsigned long long)s->bytes_resident)) return 500;
        }
        if (cap_str(body,"]}")) return 500;
    }
    if (cap_str(body,"]}")) return 500;
    uint8_t hash[32]; preparation_digest(body->p,body->len,hash); lmb_hex(review,hash,32); return 200;
}
static void preparation_request_hash(const char *tracker,const LmbPreparationRequest *r,uint8_t out[32]) {
    /* Fixed, zero-initialized parser output has no pointer/padding ambiguity:
     * hash explicit string/number fields, not raw C object representation. */
    LmbSha sha; lmb_sha_init(&sha);
    lmb_sha_update(&sha,tracker,strlen(tracker)+1); lmb_sha_update(&sha,r->review,65);
    for (uint32_t i=0;i<r->model_count;i++) lmb_sha_update(&sha,r->models[i],strlen(r->models[i])+1);
    for (uint32_t i=0;i<r->node_count;i++) lmb_sha_update(&sha,r->nodes[i],65);
    uint8_t numbers[20]; lmb_put32(numbers,r->model_count); lmb_put32(numbers+4,r->node_count);
    lmb_put32(numbers+8,r->context); lmb_put32(numbers+12,r->sessions); lmb_put32(numbers+16,r->max_new);
    lmb_sha_update(&sha,numbers,sizeof numbers); lmb_sha_final(&sha,out);
}
static int preparation_status(int dir,const char *tracker,const char *id,int cancel,Cap *body) {
    HomeServiceSnapshot saved,current; char intent[80]; uint8_t digest[32]; size_t n=0;
    snprintf(intent,sizeof intent,"%s.intent",id);
    if (preparation_read(dir,intent,digest,sizeof digest,&n) || n!=32) return 404;
    if (preparation_load(dir,id,&saved)) return 503;
    if (strcmp(saved.tracker,tracker)) return 409;
    int live=!home_service_query("prepare",HOME_SVC_STATUS,&saved,&current);
    if (live) saved=current;
    if (cancel && live && saved.state==HOME_SVC_RUNNING) {
        if (home_service_query("prepare",HOME_SVC_CANCEL,&saved,&current)) return 409;
        saved=current;
    }
    const char *state=saved.state==HOME_SVC_DONE ? "done" : saved.state==HOME_SVC_FAILED ? "failed" :
        live ? "running" : "interrupted_or_starting";
    if (api_addf(body,"{\"schema\":1,\"operation\":\"%s\",\"state\":\"%s\",\"live\":%s,"
        "\"cancel_requested\":%s,\"detail\":",id,state,live ? "true" : "false",cancel && live ? "true" : "false") ||
        api_json_text(body,saved.detail) || cap_str(body,",\"approval_replayed\":false}")) return 500;
    return 200;
}
static void api_preparation_list(int fd,int access,const char *tracker) {
    int dir=preparation_dir(access),scan=dir<0 ? -1 : openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    DIR *d=scan<0 ? NULL : fdopendir(scan); Cap body={0}; unsigned count=0;
    int bad=!d || cap_str(&body,"{\"schema\":1,\"operations\":[");
    struct dirent *e;
    while (!bad && (e=readdir(d))) {
        if (strlen(e->d_name)!=70 || strcmp(e->d_name+64,".state")) continue;
        char id[65]; memcpy(id,e->d_name,64); id[64]=0;
        HomeServiceSnapshot s; struct stat st;
        if (!lmb_preparation_hex(id) || preparation_load(dir,id,&s)) { bad=1; break; }
        if (strcmp(s.tracker,tracker)) continue;
        if (fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW) || ++count>256) { bad=1; break; }
        bad=api_addf(&body,"%s{\"operation\":\"%s\",\"updated_at\":%llu,\"recorded_state\":\"%s\",\"detail\":",
            count>1 ? "," : "",id,(unsigned long long)st.st_mtime,
            s.state==HOME_SVC_DONE ? "done" : s.state==HOME_SVC_FAILED ? "failed" : "requires_live_check") ||
            api_json_text(&body,s.detail) || cap_str(&body,"}");
    }
    if (!bad) bad=cap_str(&body,"]}");
    if (bad) api_error(fd,503,"preparation_history_unavailable"); else (void)api_response(fd,200,body.p);
    if (d) closedir(d); else if (scan>=0) close(scan);
    if (dir>=0) close(dir);
    free(body.p);
}
static void api_preparation(int fd,int access,const char *tracker,const char *text,size_t size) {
    LmbPreparationRequest r; Cap result={0},plan={0}; LmbTuiState *st=NULL;
    int status=400,dir=-1,lock=-1; char review[65];
    if (lmb_preparation_parse(text,size,&r)) goto done;
    int start=!strcmp(r.action,"start"), preview=!strcmp(r.action,"preview");
    if (!preview) {
        dir=preparation_dir(access); if (dir<0) { status=503; goto done; }
        if (!start) { status=preparation_status(dir,tracker,r.operation,!strcmp(r.action,"cancel"),&result); goto done; }
        lock=lmb_api_access_lock(dir); if (lock<0) { status=409; goto done; }
        uint8_t digest[32],old[32]; size_t n=0; char name[80];
        preparation_request_hash(tracker,&r,digest); snprintf(name,sizeof name,"%s.intent",r.operation);
        int found=preparation_read(dir,name,old,sizeof old,&n);
        if (!found) {
            status=n==32 && !memcmp(digest,old,32) ? preparation_status(dir,tracker,r.operation,0,&result) : 409;
            goto done;
        }
        if (found<0) { status=409; goto done; }
    }
    st=preparation_catalogue(tracker,&r); if (!st) { status=503; goto done; }
    status=preparation_review(st,&r,&plan,review); if (status!=200) goto done;
    if (preview) {
        if (api_addf(&result,"{\"schema\":1,\"review\":\"%s\",\"plan\":",review) ||
            cap_add(&result,plan.p,plan.len) || cap_str(&result,"}")) status=500;
        goto done;
    }
    if (strcmp(review,r.review)) { status=409; goto done; }
    uint8_t digest[32]; preparation_request_hash(tracker,&r,digest);
    if (preparation_intent(dir,r.operation,digest,1)) { status=409; goto done; }
    HomePreparationObserver observer={.fd=dir,.save=preparation_save}; HomeServiceSnapshot started;
    (void)lmb_unhex(observer.instance,r.operation,32);
    memset(&started,0,sizeof started); memcpy(started.instance,observer.instance,32);
    started.state=HOME_SVC_RUNNING; strcpy(started.role,"prepare");
    snprintf(started.tracker,sizeof started.tracker,"%s",tracker);
    strcpy(started.detail,"Intent saved; keeper startup not yet confirmed. No automatic retry.");
    if (preparation_save(dir,&started)) { status=503; goto done; }
    if (home_prepare_portfolio_begin(st,&observer,&started)) { status=503; goto done; }
    /* A lost response is safe: the same operation ID only observes this
     * keeper, including after API/manager restart. It never starts another. */
    status=api_addf(&result,"{\"schema\":1,\"operation\":\"%s\",\"state\":\"starting\",\"requires_approval\":true}",r.operation) ? 500 : 202;
done:
    if (lock>=0) close(lock);
    if (dir>=0) close(dir);
    if (status==200 || status==202) (void)api_response(fd,(unsigned)status,result.p);
    else api_error(fd,(unsigned)status,status==400 ? "invalid_preparation_request" : status==404 ? "preparation_not_found" :
        status==409 ? "review_or_operation_changed" : status==422 ? "resident_plan_unavailable" : "preparation_not_confirmed_inspect_before_retry");
    free(st); free(plan.p); free(result.p);
}
#endif
