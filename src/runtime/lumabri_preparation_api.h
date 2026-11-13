/* Management interface to the same preparation keeper used by the TUI.
 * The catalogue root is owner configured. Review is read-only; start needs
 * an exact review fence, a durable operation ID and separate donor consent. */
#ifndef LMB_PREPARATION_API_H
#define LMB_PREPARATION_API_H
#include "lumabri_preparation_store.h"

static LmbTuiState *preparation_catalogue(const char *tracker,const LmbPreparationRequest *r) {
    HomeSettings settings; home_settings_load(&settings);
    LmbTuiState *st=calloc(1,sizeof *st); if (!st) return NULL;
    st->context=r ? r->context : LMB_HOME_DEFAULT_CONTEXT; st->sessions=r ? r->sessions : 1; st->max_new=r ? r->max_new : 128;
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
/* Cheap source-change fence, not a signed checkpoint identity. Weight bytes
 * are hashed by the existing source indexer before any donor approves them. */
static int preparation_file_stamp(const char *root,const char *rel,const LmbSourceStat *st,void *opaque) {
    (void)root; LmbBuf item={0},*all=opaque; uint8_t hash[32];
    int bad=lmb_buf_str(&item,rel) || lmb_buf_u64(&item,st->device) ||
        lmb_buf_u64(&item,st->inode) || lmb_buf_u64(&item,st->size) ||
        lmb_buf_u64(&item,st->mtime_ns) || lmb_buf_u64(&item,st->ctime_ns);
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
static int preparation_results_json(Cap *body,const HomeServiceSnapshot *s) {
    if (api_addf(body,",\"results_known\":%s,\"allocations\":[",s->preparation_results_known ? "true" : "false")) return -1;
    for (uint32_t i=0;i<s->preparation_result_count;i++) {
        const HomePreparationResult *r=&s->preparation_results[i]; char id[65]; lmb_hex(id,r->allocation,32);
        if (api_addf(body,"%s{\"id\":\"%s\",\"name\":",i ? "," : "",id) ||
            api_json_text(body,r->catalogue_name) || api_addf(body,",\"state\":\"%s\"}",
                r->ready_observed ? "ready_observed" : "unconfirmed")) return -1;
    }
    return cap_str(body,"],\"result_scope\":\"historical_preparation_not_live_readiness\"");
}
/* A person who prepares a model from the page expects to chat with it. The
 * requester's credential is recorded beside the intent, and each allocation
 * of that operation observed ready is granted to that same credential through
 * the ordinary grant path. Nobody else gains anything: the record names one
 * credential digest (a recreated username does not inherit it), the holder
 * must still have management when the grant is written, and an operation
 * started without a signed-in user (controllers, the TUI) has no record. */
#define LMB_PREPARATION_OWNER 105u
static int preparation_owner_write(int dir,const char *id,const LmbApiUser *user) {
    char name[80],intent[80]; snprintf(name,sizeof name,"%s.owner",id); snprintf(intent,sizeof intent,"%s.intent",id);
    uint8_t record[LMB_PREPARATION_OWNER]={0};
    memcpy(record,"LMBPOWN1",8); memcpy(record+8,user->digest,64);
    memcpy(record+72,user->name,strnlen(user->name,32));
    int target=lmb_journal_location(dir,intent);
    if (target<0) return -1;
    int fd=openat(target,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    int bad=fd<0 || preparation_write(fd,record,sizeof record) || fsync(fd);
    if (fd>=0 && close(fd)) bad=1;
    if (!bad) bad=fsync(target);
    close(target); lmb_api_wipe(record,sizeof record); return bad ? -1 : 0;
}
/* 0: the recorded requester still exists with the same credential and still
 * holds management. 1: no requester, or it is no longer that credential. */
static int preparation_owner(int dir,int access,const char *id,LmbApiUser *owner) {
    char name[80]; snprintf(name,sizeof name,"%s.owner",id);
    uint8_t record[LMB_PREPARATION_OWNER]; size_t n=0; char user[33];
    if (preparation_read(dir,name,record,sizeof record,&n) || n!=sizeof record || memcmp(record,"LMBPOWN1",8)) return 1;
    memcpy(user,record+72,33); user[32]=0;
    int stale=lmb_api_user_load(access,user,owner) || memcmp(owner->digest,record+8,64) ||
        !lmb_management_allows(access,owner);
    lmb_api_wipe(record,sizeof record); return stale ? 1 : 0;
}
enum { LMB_PREPARATION_GRANT_NONE=0,LMB_PREPARATION_GRANTED,LMB_PREPARATION_GRANT_FULL,
       LMB_PREPARATION_GRANT_FAILED,LMB_PREPARATION_GRANT_OTHER };
/* Grants what is ready and not yet held. `caller`, when given, must be the
 * recorded requester: observing someone else's operation grants nothing. */
static int preparation_grant_ready(int dir,int access,const HomeServiceSnapshot *s,const LmbApiUser *caller) {
    char id[65]; lmb_hex(id,s->instance,32);
    LmbApiUser owner; int outcome=LMB_PREPARATION_GRANT_NONE;
    if (preparation_owner(dir,access,id,&owner) ||
        (caller && (strcmp(caller->name,owner.name) || memcmp(caller->digest,owner.digest,64)))) {
        lmb_api_wipe(&owner,sizeof owner); return LMB_PREPARATION_GRANT_OTHER;
    }
    for (uint32_t i=0;i<s->preparation_result_count;i++) {
        const HomePreparationResult *r=&s->preparation_results[i];
        if (!r->ready_observed) continue;
        if (lmb_api_user_allows(&owner,r->allocation)) { if (!outcome) outcome=LMB_PREPARATION_GRANTED; continue; }
        int lock=-1,rc=-1;
        for (unsigned attempt=0;attempt<40 && lock<0;attempt++) {
            lock=lmb_api_access_lock(access);
            if (lock<0) { if (errno!=EAGAIN && errno!=EWOULDBLOCK) break; (void)poll(NULL,0,50); }
        }
        if (lock>=0) {
            /* Decide again under the lock: the credential may have changed. */
            LmbApiUser current;
            if (!preparation_owner(dir,access,id,&current) && !memcmp(current.digest,owner.digest,64))
                rc=api_grant_allocation(access,s->tracker,owner.name,r->allocation);
            lmb_api_wipe(&current,sizeof current); close(lock);
        }
        char allocation[65]; lmb_hex(allocation,r->allocation,32);
        fprintf(stderr,"[preparation-grant] user=%s allocation=%s result=%s\n",owner.name,allocation,
            !rc ? "granted" : rc>0 ? "grant_list_full" : "failed");
        if (rc>0) outcome=LMB_PREPARATION_GRANT_FULL;
        else if (rc<0) { if (outcome!=LMB_PREPARATION_GRANT_FULL) outcome=LMB_PREPARATION_GRANT_FAILED; }
        else if (!outcome) outcome=LMB_PREPARATION_GRANTED;
    }
    lmb_api_wipe(&owner,sizeof owner); return outcome;
}
/* The keeper's observer: the grant does not depend on a page staying open.
 * The descriptor is the preparations directory inside the access directory. */
static int preparation_save_granting(int dir,const HomeServiceSnapshot *s) {
    int rc=preparation_save(dir,s);
    if (rc) return rc;
    int access=openat(dir,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (access>=0 && !lmb_api_private_fd(access,1)) (void)preparation_grant_ready(dir,access,s,NULL);
    if (access>=0) close(access);
    return 0;
}
static int preparation_access_json(Cap *body,int outcome) {
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
/* The keeper may commit its terminal receipt and close the socket between
 * our file read and RPC. Re-read that exact operation on a failed RPC; a
 * timeout alone never proves interruption and never authorizes a new OFFER. */
static int preparation_observe(int dir,const char *id,HomeServiceSnapshot *saved,int *live) {
    HomeServiceSnapshot current; *live=0;
    if (preparation_load(dir,id,saved)) return -1;
    if (saved->state==HOME_SVC_DONE || saved->state==HOME_SVC_FAILED) return 0;
    if (!home_service_query("prepare",HOME_SVC_STATUS,saved,&current)) {
        *live=1; *saved=current; return 0;
    }
    return preparation_load(dir,id,saved);
}
static int preparation_status(int dir,int access,const LmbApiUser *caller,const char *tracker,const char *id,int cancel,Cap *body) {
    HomeServiceSnapshot saved,current; char intent[80]; uint8_t digest[32]; size_t n=0; int live=0;
    snprintf(intent,sizeof intent,"%s.intent",id);
    if (preparation_read(dir,intent,digest,sizeof digest,&n) || n!=32) return 404;
    if (preparation_observe(dir,id,&saved,&live)) return 503;
    if (strcmp(saved.tracker,tracker)) return 409;
    if (cancel && live && saved.state==HOME_SVC_RUNNING) {
        if (home_service_query("prepare",HOME_SVC_CANCEL,&saved,&current)) return 409;
        saved=current;
    }
    const char *state=saved.state==HOME_SVC_DONE ? "done" : saved.state==HOME_SVC_FAILED ? "failed" :
        live ? "running" : "interrupted_or_starting";
    if (api_addf(body,"{\"schema\":1,\"operation\":\"%s\",\"state\":\"%s\",\"live\":%s,"
        "\"cancel_requested\":%s,\"detail\":",id,state,live ? "true" : "false",cancel && live ? "true" : "false") ||
        api_json_text(body,saved.detail) || preparation_results_json(body,&saved) ||
        (caller && preparation_access_json(body,preparation_grant_ready(dir,access,&saved,caller))) ||
        cap_str(body,",\"approval_replayed\":false}")) return 500;
    return 200;
}
static int preparation_list(int dir,const char *tracker,const char *after,Cap *body) {
    if (after[0] && !lmb_preparation_hex(after)) return 400;
    unsigned count=0;
    LmbJournalPage page={0};
    int bad=dir<0 || lmb_journal_page(dir,".state",after,&page) || cap_str(body,"{\"schema\":1,\"operations\":[");
    unsigned shown=page.count>LMB_JOURNAL_PAGE ? LMB_JOURNAL_PAGE : page.count;
    for (unsigned i=0;!bad && i<shown;i++) {
        char id[65]; memcpy(id,page.names[i],64); id[64]=0;
        HomeServiceSnapshot s; struct stat st;
        if (!lmb_preparation_hex(id) || preparation_load(dir,id,&s)) { bad=1; break; }
        if (strcmp(s.tracker,tracker)) continue;
        int record=lmb_journal_open(dir,page.names[i],O_RDONLY);
        if (record<0) { bad=1; break; }
        bad=fstat(record,&st); close(record); if (bad) break;
        count++;
        bad=api_addf(body,"%s{\"operation\":\"%s\",\"updated_at\":%llu,\"recorded_state\":\"%s\",\"detail\":",
            count>1 ? "," : "",id,(unsigned long long)st.st_mtime,
            s.state==HOME_SVC_DONE ? "done" : s.state==HOME_SVC_FAILED ? "failed" : "requires_live_check") ||
            api_json_text(body,s.detail) || preparation_results_json(body,&s) || cap_str(body,"}");
    }
    /* Advance over scanned keys even when they belong to another tracker.
     * An empty page can therefore still have a next cursor. */
    char next[65]={0};
    if (page.count>LMB_JOURNAL_PAGE) memcpy(next,page.names[LMB_JOURNAL_PAGE-1],64);
    if (!bad) bad=api_addf(body,"],\"next_cursor\":\"%s\",\"order\":\"operation_id\","
        "\"page_limit\":%u,\"history_policy\":\"exact_records_retained_disk_space_required\"}",next,LMB_JOURNAL_PAGE);
    return bad ? 503 : 200;
}
static void api_preparation_list(int fd,int access,const char *tracker,const char *after) {
    int dir=preparation_dir(access); Cap body={0};
    int status=preparation_list(dir,tracker,after,&body);
    if (status!=200) api_error(fd,(unsigned)status,status==400 ? "invalid_history_cursor" : "preparation_history_unavailable");
    else (void)api_response(fd,200,body.p);
    if (dir>=0) close(dir);
    free(body.p);
}
/* An accepted intent is immutable. Repeating it observes the existing keeper
 * and must not contend with a new start (or an inherited startup lock while
 * the detached keeper is closing descriptors). Zero means no intent exists;
 * uncertain/corrupt evidence is never permission to start again. */
static int preparation_repeat(int dir,int access,const LmbApiUser *caller,const char *tracker,const LmbPreparationRequest *r,Cap *result) {
    uint8_t digest[32],old[32]; size_t n=0; char name[80];
    preparation_request_hash(tracker,r,digest); snprintf(name,sizeof name,"%s.intent",r->operation);
    int found=preparation_read(dir,name,old,sizeof old,&n);
    if (!found) return n==32 && !memcmp(digest,old,32) ? preparation_status(dir,access,caller,tracker,r->operation,0,result) : 409;
    return found<0 ? 409 : 0;
}
/* The authenticated HTTP handler and service controllers share one typed
 * operation. The caller supplies a validated request; neither transport is
 * allowed to bypass the review, intent or keeper admission path. */
static int preparation_execute_as(int access,const char *tracker,const LmbApiUser *caller,const LmbPreparationRequest *request,Cap *reply) {
    LmbPreparationRequest r=*request; Cap result={0},plan={0}; LmbTuiState *st=NULL;
    int status=400,dir=-1,lock=-1; char review[65];
    int start=!strcmp(r.action,"start"), preview=!strcmp(r.action,"preview");
    if (!preview) {
        dir=preparation_dir(access); if (dir<0) { status=503; goto done; }
        if (!start) { status=preparation_status(dir,access,caller,tracker,r.operation,!strcmp(r.action,"cancel"),&result); goto done; }
        status=preparation_repeat(dir,access,caller,tracker,&r,&result); if (status) goto done;
        lock=lmb_api_access_lock(dir); if (lock<0) { status=409; goto done; }
        /* A concurrent starter may have committed while we acquired the
         * guard. Only the missing-intent path can create a new keeper. */
        status=preparation_repeat(dir,access,caller,tracker,&r,&result); if (status) goto done;
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
    if (caller && preparation_owner_write(dir,r.operation,caller)) { status=503; goto done; }
    HomePreparationObserver observer={.fd=dir,.save=caller ? preparation_save_granting : preparation_save}; HomeServiceSnapshot started;
    (void)lmb_unhex(observer.instance,r.operation,32);
    memset(&started,0,sizeof started); memcpy(started.instance,observer.instance,32);
    started.state=HOME_SVC_RUNNING; strcpy(started.role,"prepare");
    started.preparation_results_known=1;
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
    free(st); free(plan.p); *reply=result;
    return status;
}
/* Controllers act without a signed-in user: nothing is recorded or granted. */
static int preparation_execute(int access,const char *tracker,const LmbPreparationRequest *request,Cap *reply) {
    return preparation_execute_as(access,tracker,NULL,request,reply);
}
static void api_preparation(int fd,int access,const char *tracker,const LmbApiUser *user,const char *text,size_t size) {
    LmbPreparationRequest request; Cap result={0};
    int status=lmb_preparation_parse(text,size,&request) ? 400 : preparation_execute_as(access,tracker,user,&request,&result);
    if (status==200 || status==202) (void)api_response(fd,(unsigned)status,result.p);
    else api_error(fd,(unsigned)status,status==400 ? "invalid_preparation_request" : status==404 ? "preparation_not_found" :
        status==409 ? "review_or_operation_changed" : status==422 ? "resident_plan_unavailable" : "preparation_not_confirmed_inspect_before_retry");
    free(result.p);
}
#endif
