/* Actual replica preparation/publication/retirement on explicitly selected
 * household machines. Uses the same review, approval and drain operations
 * as a human operator. No provider, shell command or consent bypass. */
#ifndef LMB_ELASTIC_CONTROLLER_H
#define LMB_ELASTIC_CONTROLLER_H
#include "src/planner/lumabri_elastic_store.h"
#include "src/planner/lumabri_elastic_json.h"

static int api_elastic_record(Cap *body,const LmbElasticPolicy *p) {
    char id[65],allocation[65]; lmb_hex(id,p->route.id,32); lmb_hex(allocation,p->pending_allocation,32);
    static const char *const phases[]={"idle","preparing","publishing_replica","withdrawing_replica","retiring"};
    uint64_t bytes=0; if (lmb_elastic_total(p,&bytes)) return -1;
    int bad=api_addf(body,"{\"id\":\"%s\",\"revision\":%u,\"enabled\":%s,\"faulted\":%s,"
        "\"phase\":\"%s\",\"replicas\":%u,\"min_replicas\":%u,\"max_replicas\":%u,\"route_revision\":%u,"
        "\"reserved_bytes\":\"%llu\",\"max_reserved_bytes\":\"%llu\",\"cooldown_seconds\":%u,\"idle_seconds\":%u",
        id,p->revision,p->enabled ? "true" : "false",p->faulted ? "true" : "false",phases[p->phase],p->route.count,
        p->min_replicas,p->max_replicas,p->route.revision,(unsigned long long)bytes,(unsigned long long)p->max_reserved_bytes,
        p->cooldown,p->idle_seconds) ||
        api_addf(body,",\"last_check\":%llu,\"last_change\":%llu,\"pending_operation\":\"%s\",\"pending_allocation\":\"%s\",\"reason\":",
        (unsigned long long)p->last_check,(unsigned long long)p->last_change,
        p->request.operation,lmb_route_nonzero(p->pending_allocation) ? allocation : "") ||
        api_json_text(body,p->reason) || cap_str(body,",\"source_model\":") || api_json_text(body,p->request.models[0]);
    if (!bad) bad=api_addf(body,",\"context\":%u,\"sessions\":%u,\"max_new\":%u,\"up_rejections\":%u,\"ttft_ms\":%u,\"nodes\":[",
        p->request.context,p->request.sessions,p->request.max_new,p->up_rejections,p->ttft_ms);
    for (uint32_t i=0;!bad && i<p->request.node_count;i++)
        bad=(i && cap_str(body,",")) || api_json_text(body,p->request.nodes[i]);
    if (!bad) bad=cap_str(body,"],\"adopt_allocations\":[");
    for (uint32_t i=0;!bad && i<p->route.count;i++) {
        lmb_hex(allocation,p->route.replicas[i].allocation,32);
        bad=api_addf(body,"%s\"%s\"",i ? "," : "",allocation);
    }
    if (!bad) bad=cap_str(body,"],\"availability_observations\":[");
    static const char *const states[]={"unknown","accepting","draining","unreachable"};
    for (uint32_t i=0;!bad && i<p->route.count;i++) {
        LmbElasticReplicaHealth h=p->health_revision==p->route.revision ? p->health[i] : (LmbElasticReplicaHealth){0};
        lmb_hex(allocation,p->route.replicas[i].allocation,32);
        bad=api_addf(body,"%s{\"allocation\":\"%s\",\"state\":\"%s\",\"checked_at\":%llu,\"consecutive_failures\":%u}",
            i ? "," : "",allocation,states[h.state],(unsigned long long)h.at_wall,h.failures);
    }
    return bad || cap_str(body,"],\"scope\":\"authorized_machine_replica_lifecycle\",\"cloud_provisioning\":false}") ? -1 : 0;
}
static int api_elastic_ids(int root,uint8_t ids[LMB_ROUTE_MAX][32],uint32_t *count) {
    *count=0; int fd=openat(root,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    DIR *d=fd<0 ? NULL : fdopendir(fd); if (!d) { if (fd>=0) close(fd); return -1; }
    struct dirent *e; int bad=0; errno=0;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..") || !strcmp(e->d_name,"access.lock")) continue;
        if (!lmb_preparation_hex(e->d_name) || *count>=LMB_ROUTE_MAX || lmb_unhex(ids[*count],e->d_name,32)) { bad=1; break; }
        (*count)++; errno=0;
    }
    if (!e && errno) bad=1;
    closedir(d); return bad ? -1 : 0;
}
static int api_elastic_status(int access,const char *tracker,Cap *body) {
    int root=lmb_elastic_root(access,0); uint8_t ids[LMB_ROUTE_MAX][32]; uint32_t count=0;
    int bad=root<0 ? errno!=ENOENT : api_elastic_ids(root,ids,&count);
    if (bad) fprintf(stderr,"[elastic-policy] cannot read policy index (errno=%d)\n",errno);
    if (!bad) bad=cap_str(body,"{\"schema\":1,\"policies\":[");
    for (uint32_t i=0;!bad && i<count;i++) {
        int dir=lmb_elastic_dir(root,ids[i],0); LmbElasticPolicy p;
        int rc=dir<0 ? -1 : lmb_elastic_load(dir,&p); if (dir>=0) close(dir);
        /* Empty directories may survive a failed first configuration. */
        if (rc==1) continue;
        if (rc || memcmp(p.route.id,ids[i],32) || strcmp(p.route.tracker,tracker)) {
            fprintf(stderr,"[elastic-policy] cannot read exact policy record (status=%d, errno=%d)\n",rc,errno);
            bad=1; break;
        }
        if (body->p[body->len-1]!='[') bad=cap_str(body,",");
        if (!bad) bad=api_elastic_record(body,&p);
    }
    if (root>=0) close(root);
    if (!bad) bad=cap_str(body,"]}");
    return bad ? 503 : 200;
}
static int api_elastic_plan_bytes(const LmbElasticPolicy *p,const LmbResidentPlan *plan,uint64_t *bytes) {
    *bytes=0;
    if (!plan->execution.count || plan->execution.hybrid || plan->sessions!=p->request.sessions) return -1;
    for (uint32_t i=0;i<plan->execution.count;i++) {
        char node[65]; lmb_hex(node,plan->peer_keys[i],32); int selected=0;
        for (uint32_t j=0;j<p->request.node_count;j++) selected|=!strcmp(node,p->request.nodes[j]);
        uint64_t size=plan->execution.nodes[i].reserved_bytes;
        if (!selected || !size || UINT64_MAX-*bytes<size) return -1;
        *bytes+=size;
    }
    return 0;
}
static int api_elastic_source(const LmbElasticPolicy *p,const LmbTuiState *st,char stamp[65]) {
    const LmbTuiModel *m=NULL; uint8_t content[32];
    for (int i=0;i<st->nmodels;i++) if (!strcmp(st->models[i].name,p->request.models[0])) {
        if (m) return -1;
        m=&st->models[i];
    }
    char identity[65];
    return !m || lmb_checkpoint_identity(m->dir,identity,NULL,NULL) || lmb_unhex(content,identity,32) ||
        memcmp(content,p->route.content,32) || preparation_source_stamp(m->dir,stamp) ? -1 : 0;
}
/* Explicit recovery never discards uncertain ownership. Lock the API
 * submission path AND the keeper role before interpreting an absent keeper:
 * no concurrent startup can assign an allocation after this observation. */
static int api_elastic_recover_preparation(int access,LmbElasticPolicy *p) {
    if (p->phase!=LMB_ELASTIC_PREPARING) return -1;
    int operations=preparation_dir(access),guard=operations<0 ? -1 : lmb_api_access_lock(operations);
    int keeper=-1,status=-1; char path[1200],name[80]; HomeServiceSnapshot s;
    uint8_t hash[32],expected[32]; size_t size=0;
    if (guard<0 || home_service_path("prepare","lock",path,sizeof path)) goto done;
    keeper=open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if (keeper<0 || lmb_api_private_fd(keeper,0) || flock(keeper,LOCK_EX|LOCK_NB)) goto done;
    snprintf(name,sizeof name,"%s.intent",p->request.operation);
    preparation_request_hash(p->route.tracker,&p->request,expected);
    if (preparation_read(operations,name,hash,sizeof hash,&size) || size!=32 || memcmp(hash,expected,32) ||
        preparation_load(operations,p->request.operation,&s) || strcmp(s.tracker,p->route.tracker) ||
        !s.preparation_results_known) goto done;
    if (s.preparation_result_count) {
        /* The owner may have retired the unpublished result explicitly.
         * Require the exact plan's complete retirement journal, not absence
         * from discovery or a failed network connection. */
        if (s.preparation_result_count!=1) goto done;
        const uint8_t *id=s.preparation_results[0].allocation; char allocation[65]; lmb_hex(allocation,id,32);
        snprintf(name,sizeof name,"%s.retire",allocation);
        int retire=lmb_retirement_dir(access); LmbRetirement r; uint8_t plan_hash[64];
        LmbResidentPlan *plan=calloc(1,sizeof *plan);
        int confirmed=retire>=0 && plan && !lmb_retirement_load(retire,name,&r) && r.phase==LMB_RET_DONE &&
            !api_find_plan(p->route.tracker,id,plan) && !lmb_retirement_plan_hash(plan,plan_hash) &&
            !memcmp(plan_hash,r.plan_hash,64);
        if (retire>=0) close(retire);
        free(plan);
        if (!confirmed) {
            if (s.state==HOME_SVC_DONE && s.preparation_results[0].ready_observed) {
                p->faulted=0; strcpy(p->reason,"exact_completed_preparation_observation_resumed"); status=0;
            }
            goto done;
        }
    }
    s.state=HOME_SVC_FAILED;
    strcpy(s.detail,"Operator reconciled quiescent preparation: no assigned allocation, or exact retirement confirmed. Intent retained.");
    if (preparation_save(operations,&s)) goto done;
    lmb_elastic_idle(p); p->faulted=0; p->last_change=(uint64_t)time(NULL);
    strcpy(p->reason,"preparation_reconciled_no_outstanding_allocation"); status=0;
done:
    if (keeper>=0) close(keeper);
    if (guard>=0) close(guard);
    if (operations>=0) close(operations);
    return status;
}
static int api_elastic_configure(int access,const char *tracker,const char *text,size_t size,Cap *body) {
    LmbElasticPolicy p,old; int configure;
    if (lmb_elastic_parse(text,size,&p,&configure)) return 400;
    int root=lmb_elastic_root(access,1),lock=root<0 ? -1 : lmb_api_access_lock(root),dir=-1,status=503;
    LmbTuiState *st=NULL; LmbResidentPlan *plan=NULL; LmbModelRoute current;
    if (lock<0) goto done;
    uint8_t ids[LMB_ROUTE_MAX][32]; uint32_t count=0;
    if (api_elastic_ids(root,ids,&count)) goto done;
    int existing=0; for (uint32_t i=0;i<count;i++) existing|=!memcmp(ids[i],p.route.id,32);
    if (!existing && count==LMB_ROUTE_MAX) { status=409; goto done; }
    dir=lmb_elastic_dir(root,p.route.id,configure==1); if (dir<0) goto done;
    int rc=lmb_elastic_load(dir,&old); if (rc<0) goto done;
    if ((rc && p.revision) || (!rc && (p.revision!=old.revision || strcmp(old.route.tracker,tracker)))) { status=409; goto done; }
    if (configure!=1) {
        if (configure==3) {
            if (api_route_read(access,old.route.id,tracker,&current) || !lmb_elastic_route_equal(&old.route,&current) ||
                api_elastic_recover_preparation(access,&old)) { status=409; goto done; }
            old.revision++;
            if (lmb_elastic_save(dir,&old)) goto done;
            status=api_elastic_record(body,&old) ? 500 : 200; goto done;
        }
        if (configure==2 && old.faulted) { status=409; goto done; }
        old.enabled=configure==2; if (old.revision<UINT32_MAX) old.revision++;
        strcpy(old.reason,configure==2 ? "resumed_existing_intent_not_recreated" : "disabled_no_new_decisions_pending_intent_retained");
        if (lmb_elastic_save(dir,&old)) goto done;
        status=api_elastic_record(body,&old) ? 500 : 200; goto done;
    }
    if (!rc && old.phase) { status=409; goto done; }
    if (api_route_read(access,p.route.id,tracker,&current) || current.revision!=p.route.revision ||
        current.count!=p.route.count) { status=409; goto done; }
    for (uint32_t i=0;i<current.count;i++) if (memcmp(current.replicas[i].allocation,p.route.replicas[i].allocation,32)) {
        status=409; goto done;
    }
    /* Reconfiguration cannot discard an allocation the previous policy owned. */
    if (!rc) for (uint32_t i=0;i<old.route.count;i++) {
        int found=0; for (uint32_t j=0;j<current.count;j++) found|=!memcmp(old.route.replicas[i].allocation,current.replicas[j].allocation,32);
        if (!found) { status=409; goto done; }
    }
    p.route=current; p.revision++; p.last_change=rc ? 0 : old.last_change;
    if (p.request.context<current.context || p.request.max_new<current.max_new) { status=422; goto done; }
    /* Two autonomous writers of the same route must not fight each other. */
    int policies=lmb_policy_dir(access,0);
    if (policies>=0) {
        LmbPortfolioPolicy portfolio; int loaded=lmb_policy_load(policies,&portfolio); close(policies);
        if (loaded<0) goto done;
        if (!loaded && (portfolio.enabled || portfolio.pending)) for (uint32_t i=0;i<portfolio.models;i++)
            if (!memcmp(portfolio.model_ids[i],p.route.id,32)) { status=409; goto done; }
    } else if (errno!=ENOENT) goto done;
    plan=calloc(1,sizeof *plan); if (!plan) goto done;
    for (uint32_t i=0;i<current.count;i++) {
        if (api_find_plan(tracker,current.replicas[i].allocation,plan) || !api_route_plan_matches(&current,i,plan) ||
            api_elastic_plan_bytes(&p,plan,&p.resident_bytes[i])) { status=422; goto done; }
        LmbHostControl live;
        if (lmb_host_control_rpc(plan->host,plan->host_key,plan->root,0,NULL,&live) || live.draining) { status=422; goto done; }
    }
    st=preparation_catalogue(tracker,&p.request);
    if (!st || api_elastic_source(&p,st,p.source_stamp)) { status=422; goto done; }
    strcpy(p.reason,"configured_explicit_allocation_ownership");
    if (!lmb_elastic_valid(&p)) { status=422; goto done; }
    if (lmb_elastic_save(dir,&p)) goto done;
    status=api_elastic_record(body,&p) ? 500 : 200;
done:
    free(st); free(plan);
    if (dir>=0) close(dir);
    if (lock>=0) close(lock);
    if (root>=0) close(root);
    return status;
}
static void api_elastic_http(int fd,int access,const char *tracker,const char *text,size_t size) {
    Cap body={0}; int code=text ? api_elastic_configure(access,tracker,text,size,&body) : api_elastic_status(access,tracker,&body);
    if (code==200) (void)api_response(fd,200,body.p);
    else api_error(fd,(unsigned)code,code==400 ? "invalid_elastic_policy" : code==409 ? "elastic_revision_or_ownership_conflict" :
        code==422 ? "elastic_source_or_budget_unavailable" : "elastic_state_unavailable");
    free(body.p);
}
static int api_elastic_due(int access) {
    int root=lmb_elastic_root(access,0); if (root<0) return 0;
    uint8_t ids[LMB_ROUTE_MAX][32]; uint32_t count=0; int yes=0;
    if (!api_elastic_ids(root,ids,&count)) for (uint32_t i=0;i<count && !yes;i++) {
        int dir=lmb_elastic_dir(root,ids[i],0); LmbElasticPolicy p;
        if (dir>=0) { if (!lmb_elastic_load(dir,&p)) yes=!p.faulted && (p.enabled || p.phase); close(dir); }
    }
    close(root); return yes;
}
static void api_elastic_fault(LmbElasticPolicy *p,const char *reason) {
    p->faulted=1; snprintf(p->reason,sizeof p->reason,"%s",reason);
}
/* Caller holds the route registry lock. Other aliases may not lose an
 * allocation simply because this policy is its first recorded owner. */
static int api_elastic_unshared(int routes,const uint8_t allocation[32],const uint8_t model[32]) {
    LmbRouteRegistry *s=calloc(1,sizeof *s); int bad=!s || lmb_route_registry_read(routes,s);
    for (uint32_t i=0;!bad && i<s->count;i++) if (memcmp(s->routes[i].id,model,32))
        for (uint32_t j=0;j<s->routes[i].count;j++) if (!memcmp(s->routes[i].replicas[j].allocation,allocation,32)) bad=1;
    free(s); return bad ? -1 : 0;
}
static int api_elastic_publish(int access,LmbElasticPolicy *p) {
    int dir=lmb_route_dir(access,0),lock=dir<0 ? -1 : lmb_api_access_lock(dir); int rc=-1;
    LmbRouteRegistry *s=calloc(1,sizeof *s);
    if (lock<0 || !s || lmb_route_registry_read(dir,s)) goto done;
    for (uint32_t i=0;i<s->count;i++) if (!memcmp(s->routes[i].id,p->route.id,32)) {
        if (!lmb_elastic_route_equal(&s->routes[i],&p->route)) break;
        if (p->phase==LMB_ELASTIC_PUBLISH_SHRINK && api_elastic_unshared(dir,p->pending_allocation,p->route.id)) {
            strcpy(p->reason,"replica_shared_by_another_model"); break;
        }
        s->routes[i]=p->target; rc=lmb_route_registry_publish(dir,s); break;
    }
done:
    free(s); if (lock>=0) close(lock); if (dir>=0) close(dir); return rc;
}
static void api_elastic_prepare(int access,int dir,LmbElasticPolicy *p) {
    int operations=preparation_dir(access); if (operations<0) return;
    char name[80]; snprintf(name,sizeof name,"%s.intent",p->request.operation);
    uint8_t bytes[32],expected[32]; size_t size=0;
    int present=preparation_read(operations,name,bytes,sizeof bytes,&size);
    preparation_request_hash(p->route.tracker,&p->request,expected);
    if (present==1) {
        /* Absence of this first-write receipt proves no OFFER was issued
         * under this operation. Never replace the ID on an uncertain reply. */
        if (!p->enabled) { strcpy(p->reason,"disabled_before_preparation"); lmb_elastic_idle(p); }
        else {
            Cap reply={0}; int status=preparation_execute(access,p->route.tracker,&p->request,&reply); free(reply.p);
            if (status!=202 && status!=200) {
                int now_present=preparation_read(operations,name,bytes,sizeof bytes,&size);
                if (now_present==1) {
                    lmb_elastic_idle(p); p->last_change=(uint64_t)time(NULL);
                    strcpy(p->reason,"preparation_review_changed_retry_after_cooldown");
                } else strcpy(p->reason,"preparation_outcome_requires_reconciliation");
            } else strcpy(p->reason,"preparing_requires_donor_permission");
        }
        close(operations); (void)lmb_elastic_save(dir,p); return;
    }
    if (present<0 || size!=32 || memcmp(bytes,expected,32)) {
        api_elastic_fault(p,"preparation_intent_missing_or_changed"); goto done;
    }
    HomeServiceSnapshot saved; int live=0;
    if (preparation_observe(operations,p->request.operation,&saved,&live) || strcmp(saved.tracker,p->route.tracker)) {
        api_elastic_fault(p,"preparation_receipt_unknown_inspect_operation"); goto done;
    }
    if (saved.state!=HOME_SVC_DONE) {
        if (saved.state==HOME_SVC_FAILED) api_elastic_fault(p,"preparation_failed_inspect_operation");
        else if (!live) strcpy(p->reason,"preparation_unconfirmed_same_operation_retained");
        goto done;
    }
    if (!saved.preparation_results_known || saved.preparation_result_count!=1 ||
        !saved.preparation_results[0].ready_observed || strcmp(saved.preparation_results[0].catalogue_name,p->request.models[0])) {
        api_elastic_fault(p,"exact_ready_allocation_not_confirmed"); goto done;
    }
    memcpy(p->pending_allocation,saved.preparation_results[0].allocation,32);
    LmbResidentPlan *plan=calloc(1,sizeof *plan); uint64_t reserved=0;
    if (!plan || api_find_plan(p->route.tracker,p->pending_allocation,plan) ||
        api_elastic_plan_bytes(p,plan,&reserved) || reserved>p->pending_bytes) {
        free(plan); api_elastic_fault(p,"prepared_allocation_exceeds_reviewed_scope"); goto done;
    }
    free(plan);
    /* Validate the new replica alone so busy existing sessions cannot
     * prevent publication. Preserve the logical model's exact contract. */
    char allocation[65]; lmb_hex(allocation,p->pending_allocation,32); const char *one[]={allocation};
    LmbModelRoute verified=p->route;
    if (api_route_build(p->route.tracker,one,1,&verified,1) || verified.context<p->route.context ||
        verified.max_new<p->route.max_new) { strcpy(p->reason,"ready_replica_not_live_or_contract_mismatch"); goto done; }
    if (!p->enabled) { strcpy(p->reason,"disabled_ready_allocation_retained_for_operator"); goto done; }
    p->target=p->route; p->target.revision++; p->target.replicas[p->target.count++]=verified.replicas[0];
    p->phase=LMB_ELASTIC_PUBLISH_GROW; strcpy(p->reason,"ready_replica_publication_intent");
done:
    close(operations); (void)lmb_elastic_save(dir,p);
}
static int api_elastic_begin_grow(int dir,LmbElasticPolicy *p) {
    LmbTuiState *st=preparation_catalogue(p->route.tracker,&p->request); Cap review={0};
    char stamp[65],digest[65]; uint64_t bytes=0,total=0; int bad=1;
    if (!st || api_elastic_source(p,st,stamp) || strcmp(stamp,p->source_stamp)) {
        strcpy(p->reason,"source_changed_or_not_available"); goto done;
    }
    if (preparation_review(st,&p->request,&review,digest)!=200) { strcpy(p->reason,"authorized_resident_plan_unavailable"); goto done; }
    const LmbClusterPlan *plan=&st->joint.plan.plans[0];
    for (uint32_t i=0;i<plan->nslices;i++) {
        uint64_t n=plan->slices[i].bytes_resident;
        if (!n || UINT64_MAX-bytes<n) goto done;
        bytes+=n;
    }
    if (!bytes || lmb_elastic_total(p,&total) || bytes>p->max_reserved_bytes-total) {
        strcpy(p->reason,"replica_budget_exhausted"); goto done;
    }
    uint8_t id[32]; lmb_random(id,32); lmb_hex(p->request.operation,id,32); strcpy(p->request.review,digest);
    p->pending_bytes=bytes; p->phase=LMB_ELASTIC_PREPARING;
    strcpy(p->reason,"preparation_intent_saved_before_offer");
    bad=lmb_elastic_save(dir,p);
done:
    free(st); free(review.p); return bad;
}
/* Availability is queried on the authenticated control channels, not by
 * consuming a conversation slot. Busy but accepting engines remain healthy;
 * an operator drain is not classified as a crash. At most one replica per
 * policy per tick, with a shared two-second read deadline across all its
 * nodes. Connect attempts also retain their existing finite timeouts. */
static unsigned api_elastic_probe(const LmbElasticPolicy *p,unsigned index) {
    unsigned state=LMB_ELASTIC_UNKNOWN;
    LmbResidentPlan *plan=calloc(1,sizeof *plan);
    uint64_t now=lmb_io_monotonic_ms(),prior=lmb_read_deadline_ms;
    if (!plan || !now || index>=p->route.count ||
        api_find_plan(p->route.tracker,p->route.replicas[index].allocation,plan) ||
        !api_route_plan_matches(&p->route,index,plan)) goto done;
    /* An exhausted outer controller deadline is not a failed peer probe. */
    if (prior && (prior<=now || prior-now<2000)) goto done;
    lmb_read_deadline_ms=now+2000;
    LmbHostControl host;
    if (lmb_host_control_rpc(plan->host,plan->host_key,plan->root,LMB_HOST_CONTROL_QUERY,NULL,&host)) {
        state=LMB_ELASTIC_UNREACHABLE; goto done;
    }
    if (host.draining) { state=LMB_ELASTIC_DRAINING; goto done; }
    for (uint32_t i=0;i<plan->execution.count;i++) {
        if (lmb_io_monotonic_ms()>=lmb_read_deadline_ms) goto done;
        LmbNodeControl node;
        if (home_resident_node_control(plan,i,LMB_NODE_CONTROL_QUERY,NULL,&node)) {
            state=LMB_ELASTIC_UNREACHABLE; goto done;
        }
        if (node.draining) { state=LMB_ELASTIC_DRAINING; goto done; }
    }
    state=LMB_ELASTIC_READY;
done:
    lmb_read_deadline_ms=prior; free(plan); return state;
}
/* Failure evidence permits a read-only proof query, never forced eviction.
 * One exited participant proves this immutable plan can no longer serve.
 * Other nodes are still drained/sealed through normal retirement and their
 * charges remain until every participant acknowledges release. */
static int api_elastic_failed_candidate(const LmbElasticPolicy *p,uint64_t now,uint64_t wall) {
    if (!p->enabled || p->faulted || p->phase || wall<p->last_change || wall<p->last_check ||
        (p->last_change && wall-p->last_change<p->cooldown) ||
        p->health_revision!=p->route.revision || memcmp(p->health_observer,api_demand.instance,32)) return -1;
    for (uint32_t i=0;i<p->route.count;i++) {
        const LmbElasticReplicaHealth *h=&p->health[i];
        if (h->state!=LMB_ELASTIC_UNREACHABLE || h->failures!=3 || !h->at_ms || now<h->at_ms || now-h->at_ms>120000) continue;
        LmbResidentPlan *plan=calloc(1,sizeof *plan); int found=-1;
        if (plan && !api_find_plan(p->route.tracker,p->route.replicas[i].allocation,plan) &&
            api_route_plan_matches(&p->route,i,plan)) {
            for (uint32_t node=0;node<plan->execution.count;node++)
                if (!home_resident_exit_proof(plan,node)) { found=(int)i; break; }
        }
        free(plan); return found; /* one bounded candidate per tick */
    }
    return -1;
}
static void api_elastic_tick_one(int access,int dir,LmbElasticPolicy *p) {
    uint64_t wall=(uint64_t)time(NULL),mono=lmb_io_monotonic_ms(); LmbModelRoute current;
    const char *reason; int choice=lmb_elastic_choose(p,&api_demand,mono,wall,&reason);
    if (api_route_read(access,p->route.id,p->route.tracker,&current)) { strcpy(p->reason,"route_registry_unavailable"); goto save; }
    if (lmb_elastic_reconcile(p,&current,wall)<0) goto save;
    if (p->faulted) goto save;
    if (p->phase==LMB_ELASTIC_PREPARING) { api_elastic_prepare(access,dir,p); return; }
    if (p->phase==LMB_ELASTIC_PUBLISH_GROW || p->phase==LMB_ELASTIC_PUBLISH_SHRINK) {
        if (!p->enabled) { strcpy(p->reason,"disabled_pending_publication_retained"); goto save; }
        (void)api_elastic_publish(access,p); /* even failed fsync may have published */
        if (!api_route_read(access,p->route.id,p->route.tracker,&current)) (void)lmb_elastic_reconcile(p,&current,wall);
        goto save;
    }
    if (p->phase==LMB_ELASTIC_RETIRING) {
        /* Hold the route lock across the first drain so a shared alias
         * cannot be silently ignored. Existing admitted work drains normally. */
        int routes=lmb_route_dir(access,0),guard=routes<0 ? -1 : lmb_api_access_lock(routes);
        if (guard<0 || api_elastic_unshared(routes,p->pending_allocation,p->route.id)) {
            strcpy(p->reason,"retiring_allocation_shared_or_registry_busy");
        } else {
            LmbRetirement result; int conflict=0;
            int rc=api_retirement_execute(access,p->route.tracker,p->pending_allocation,NULL,&result,&conflict);
            if (!rc && result.phase==LMB_RET_DONE) {
                lmb_elastic_idle(p); p->last_change=wall; strcpy(p->reason,"replica_released");
            } else strcpy(p->reason,rc==1 ? "draining_admitted_work" : "retirement_unconfirmed_budget_retained");
        }
        if (guard>=0) close(guard);
        if (routes>=0) close(routes);
        goto save;
    }
    int probe=lmb_elastic_health_next(p,&api_demand,mono);
    if (probe>=0) {
        unsigned state=api_elastic_probe(p,(unsigned)probe);
        lmb_elastic_health_observe(p,(unsigned)probe,state,lmb_io_monotonic_ms(),(uint64_t)time(NULL));
    }
    choice=lmb_elastic_health_choice(p,&api_demand,lmb_io_monotonic_ms(),wall,choice,&reason);
    snprintf(p->reason,sizeof p->reason,"%s",reason);
    /* Prefer restoring availability first when approved headroom permits it.
     * At the hard limit, or with no room to grow, confirmed exit can reclaim
     * the old charge before a new preparation. Neither path overbooks. */
    if (choice==LMB_ELASTIC_GROW && p->route.revision<UINT32_MAX && !api_elastic_begin_grow(dir,p)) {
        api_elastic_prepare(access,dir,p); return;
    }
    int failed=api_elastic_failed_candidate(p,lmb_io_monotonic_ms(),wall);
    if (failed>=0 && !lmb_elastic_withdraw(p,(uint32_t)failed)) {
        strcpy(p->reason,"failed_replica_exit_confirmed_withdrawal_intent");
    } else if (choice==LMB_ELASTIC_SHRINK && p->route.revision<UINT32_MAX) {
        if (!lmb_elastic_withdraw(p,p->route.count-1)) strcpy(p->reason,"idle_replica_withdrawal_intent");
    }
save:
    if (wall>=p->last_check) p->last_check=wall;
    (void)lmb_elastic_save(dir,p);
}
static void api_elastic_tick(int access,const char *tracker) {
    int root=lmb_elastic_root(access,0),lock=root<0 ? -1 : lmb_api_access_lock(root);
    uint8_t ids[LMB_ROUTE_MAX][32]; uint32_t count=0;
    if (lock>=0 && !api_elastic_ids(root,ids,&count)) for (uint32_t i=0;i<count;i++) {
        int dir=lmb_elastic_dir(root,ids[i],0); if (dir<0) continue;
        LmbElasticPolicy p;
        if (!lmb_elastic_load(dir,&p) && !memcmp(p.route.id,ids[i],32) && !strcmp(p.route.tracker,tracker))
            api_elastic_tick_one(access,dir,&p);
        close(dir);
    }
    if (lock>=0) close(lock);
    if (root>=0) close(root);
}
static int api_elastic_cli(int access,const char *tracker,char *const *args,unsigned count) {
    Cap response={0}; int status=400;
    if (!count) status=api_elastic_status(access,tracker,&response);
    else if (count==1) {
        int fd=open(args[0],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); struct stat st; char text[8192];
        int bad=fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>(off_t)sizeof text;
        if (!bad) bad=lmb_read_full(fd,text,(size_t)st.st_size);
        if (fd>=0) close(fd);
        if (!bad) status=api_elastic_configure(access,tracker,text,(size_t)st.st_size,&response);
    }
    if (response.p) puts(response.p);
    if (status!=200) fprintf(stderr,"Elastic policy not confirmed (status %d). Inspect exact route and operation before retrying.\n",status);
    free(response.p); return status==200 ? 0 : 1;
}
#endif
