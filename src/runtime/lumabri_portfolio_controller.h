/* Permanent API keeper's bounded resident-portfolio controller. All routes
 * and allocations remain owner approved. This is not provisioning, model
 * preparation, automatic consent or a guarantee of future service latency. */
#ifndef LMB_PORTFOLIO_CONTROLLER_H
#define LMB_PORTFOLIO_CONTROLLER_H
#include "src/planner/lumabri_portfolio_policy_store.h"
#include "src/planner/lumabri_portfolio_policy_json.h"

static int api_policy_registry(int access,LmbRouteRegistry *s) {
    int dir=lmb_route_dir(access,0); if (dir<0) return -1;
    int rc=lmb_route_registry_read(dir,s); close(dir); return rc;
}
static int api_policy_json(const LmbPortfolioPolicy *p,Cap *body) {
    if (!p) return cap_str(body,"{\"schema\":1,\"configured\":false}");
    if (api_addf(body,"{\"schema\":1,\"configured\":true,\"revision\":%u,\"enabled\":%s,"
        "\"faulted\":%s,\"pending\":%u,\"current\":%u,\"last_check\":%llu,\"last_change\":%llu,\"reason\":",
        p->revision,p->enabled ? "true" : "false",p->faulted ? "true" : "false",p->pending,p->current,
        (unsigned long long)p->last_check,(unsigned long long)p->last_change) || api_json_text(body,p->reason) ||
        api_addf(body,",\"ttft_ms\":%u,\"gap_ms\":%u,\"max_age_seconds\":%u,\"cooldown_seconds\":%u,"
            "\"horizon_seconds\":%u,\"min_saving_bps\":%u,\"currency\":\"%s\",\"ceiling_micro_per_hour\":\"%llu\","
            "\"switch_cost_micro\":\"%llu\",\"records\":[",p->ttft_ms,p->gap_ms,p->max_age,p->cooldown,
            p->horizon,p->min_saving_bps,p->currency,(unsigned long long)p->ceiling_micro_per_hour,
            (unsigned long long)p->switch_cost_micro)) return -1;
    for (uint32_t i=0;i<p->candidates;i++) {
        char hash[65]; lmb_hex(hash,p->records[i],32);
        if (api_addf(body,"%s{\"name\":\"%s\",\"digest\":\"%s\"}",i ? "," : "",p->names[i],hash)) return -1;
    }
    if (cap_str(body,"],\"models\":[")) return -1;
    for (uint32_t i=0;i<p->models;i++) {
        char id[65]; lmb_hex(id,p->model_ids[i],32);
        if (api_addf(body,"%s{\"id\":\"%s\",\"revision\":%u}",i ? "," : "",id,p->route_revisions[i])) return -1;
    }
    return cap_str(body,"],\"scope\":\"approved_resident_routing_only\",\"price_scope\":\"declared_machine_footprint_not_realized_savings\","
        "\"production_capacity_certified\":false,\"cloud_provisioning\":false}");
}
static int api_policy_status(int access,const char *tracker,Cap *body) {
    int dir=lmb_policy_dir(access,0);
    if (dir<0) return errno==ENOENT && !api_policy_json(NULL,body) ? 200 : 503;
    LmbPortfolioPolicy p; int rc=lmb_policy_load(dir,&p); close(dir);
    if (rc<0) return 503;
    if (!rc && strcmp(p.tracker,tracker)) return 409;
    return api_policy_json(rc ? NULL : &p,body) ? 500 : 200;
}
/* One shared operation for local owner and authenticated management API. */
static int api_policy_configure(int access,const char *tracker,const char *text,size_t n,Cap *body) {
    LmbPortfolioPolicy proposed,old; int configure;
    if (lmb_policy_parse(text,n,&proposed,&configure)) return 400;
    int dir=lmb_policy_dir(access,1),lock=dir<0 ? -1 : lmb_policy_lock(dir),records=-1,status=503;
    LmbRouteRegistry *registry=NULL; LmbCapacity *candidates=NULL;
    if (lock<0) goto done;
    int rc=lmb_policy_load(dir,&old);
    if (rc<0) goto done;
    if ((rc && proposed.revision) || (!rc && (proposed.revision!=old.revision || strcmp(old.tracker,tracker)))) {
        status=409; goto done;
    }
    if (!configure) {
        /* Stopping automation must not depend on inventory or an available
         * route registry. A surviving intent is reconciliation-only. */
        if (old.pending) {
            registry=calloc(1,sizeof *registry);
            if (registry && !api_policy_registry(access,registry))
                (void)lmb_policy_reconcile(&old,registry,(uint64_t)time(NULL));
        }
        old.enabled=0;
        if (old.revision<UINT32_MAX) old.revision++;
        strcpy(old.reason,"disabled_by_operator");
        if (lmb_policy_save(dir,&old)) goto done;
        status=api_policy_json(&old,body) ? 500 : 200; goto done;
    }
    registry=calloc(1,sizeof *registry);
    if (!registry || api_policy_registry(access,registry)) goto done;
    if (!rc && old.pending) (void)lmb_policy_reconcile(&old,registry,(uint64_t)time(NULL));
    proposed.revision++; proposed.last_change=rc ? 0 : old.last_change;
    if (strlen(tracker)>=sizeof proposed.tracker) { status=400; goto done; }
    strcpy(proposed.tracker,tracker); strcpy(proposed.reason,proposed.enabled ? "awaiting_controller" : "disabled_by_operator");
    candidates=calloc(proposed.candidates,sizeof *candidates); records=lmb_capacity_dir(access,0);
    if (!candidates || records<0) goto done;
    if (proposed.current>=proposed.candidates) { status=400; goto done; }
    for (uint32_t i=0;i<proposed.candidates;i++) {
        if (lmb_capacity_load(records,proposed.names[i],&candidates[i]) ||
            lmb_policy_record_hash(&candidates[i],proposed.records[i])) { status=422; goto done; }
        if (candidates[i].models!=proposed.models || (i && !lmb_capacity_comparable(&candidates[0],&candidates[i]))) {
            status=422; goto done;
        }
    }
    const LmbCapacity *current=&candidates[proposed.current];
    for (uint32_t i=0;i<proposed.models;i++) {
        const LmbModelRoute *route=NULL;
        for (uint32_t j=0;j<registry->count;j++) if (!memcmp(registry->routes[j].id,proposed.model_ids[i],32)) route=&registry->routes[j];
        if (!route || route->revision!=proposed.route_revisions[i] || route->count!=1 ||
            memcmp(route->replicas[0].allocation,current->allocations[i],32) || strcmp(route->tracker,tracker) ||
            memcmp(route->content,current->contents[i],32) || route->numeric_abi!=current->numeric_abi[i] ||
            strcmp(route->numeric_class,current->numeric_class[i]) || lmb_policy_route_hash(route,proposed.routes[i])) {
            status=409; goto done;
        }
    }
    if (!lmb_portfolio_policy_valid(&proposed)) { status=400; goto done; }
    if (lmb_policy_save(dir,&proposed)) goto done;
    status=api_policy_json(&proposed,body) ? 500 : 200;
done:
    if (records>=0) close(records);
    if (lock>=0) close(lock);
    if (dir>=0) close(dir);
    free(registry); free(candidates); return status;
}
static void api_policy_http(int fd,int access,const char *tracker,const char *body,size_t size) {
    Cap result={0}; int status=body ? api_policy_configure(access,tracker,body,size,&result) : api_policy_status(access,tracker,&result);
    if (result.p) (void)api_response(fd,(unsigned)status,result.p);
    else api_error(fd,(unsigned)status,status==400 ? "invalid_portfolio_policy" : status==409 ? "policy_or_route_revision_changed" :
        status==422 ? "portfolio_evidence_not_comparable" : "policy_operation_not_confirmed");
    free(result.p);
}
static int api_policy_due(int access,const char *tracker) {
    int dir=lmb_policy_dir(access,0); if (dir<0) return 0;
    LmbPortfolioPolicy p; int rc=lmb_policy_load(dir,&p); close(dir);
    return !rc && !strcmp(p.tracker,tracker) && (p.pending || (p.enabled && !p.faulted));
}
typedef struct { int dir; uint32_t candidate; LmbPortfolioPolicy *policy; } ApiPolicyIntent;
static int api_policy_intent(void *opaque,const LmbModelRoute *routes,uint32_t count) {
    ApiPolicyIntent *ctx=opaque; LmbPortfolioPolicy *p=ctx->policy;
    if (count!=p->models) return -1;
    for (uint32_t i=0;i<count;i++) if (lmb_policy_route_hash(&routes[i],p->targets[i])) return -1;
    p->pending=ctx->candidate+1; strcpy(p->reason,"publication_pending");
    return lmb_policy_save(ctx->dir,p);
}
static void api_policy_tick(int access,const char *tracker) {
    int dir=lmb_policy_dir(access,0),lock=dir<0 ? -1 : lmb_policy_lock(dir),records=-1;
    LmbPortfolioPolicy p; LmbRouteRegistry *registry=NULL; LmbCapacity *candidates=NULL;
    if (lock<0 || lmb_policy_load(dir,&p) || strcmp(p.tracker,tracker)) goto done;
    uint64_t now=(uint64_t)time(NULL);
    if (!p.pending && (!p.enabled || p.faulted || (now>=p.last_check && now-p.last_check<30))) goto done;
    registry=calloc(1,sizeof *registry);
    if (!registry || api_policy_registry(access,registry)) goto done;
    int recovering=p.pending!=0;
    (void)lmb_policy_reconcile(&p,registry,now);
    if (recovering || p.faulted) { p.last_check=now; (void)lmb_policy_save(dir,&p); goto done; }
    for (uint32_t i=0;i<p.models;i++) if (p.route_revisions[i]==UINT32_MAX) {
        p.faulted=1; strcpy(p.reason,"route_revision_exhausted"); (void)lmb_policy_save(dir,&p); goto done;
    }
    /* Keep backwards wall-clock detection meaningful; never rewrite the last
     * timestamp downwards. The parent uses a monotonic scheduling interval. */
    if (now<p.last_check || now<p.last_change) { strcpy(p.reason,"clock_moved_backwards"); (void)lmb_policy_save(dir,&p); goto done; }
    p.last_check=now; strcpy(p.reason,"checking_measured_candidates");
    if (lmb_policy_save(dir,&p)) goto done;
    candidates=calloc(p.candidates,sizeof *candidates); records=lmb_capacity_dir(access,0);
    if (!candidates || records<0) goto done;
    int passing[LMB_CAPACITY_MODELS]={0}; LmbCapacityCost prices[LMB_CAPACITY_MODELS]={0};
    for (uint32_t i=0;i<p.candidates;i++) {
        uint8_t hash[32],current[32];
        if (lmb_capacity_load(records,p.names[i],&candidates[i]) || lmb_policy_record_hash(&candidates[i],hash) ||
            memcmp(hash,p.records[i],32)) {
            p.faulted=1; strcpy(p.reason,"evidence_missing_or_replaced"); (void)lmb_policy_save(dir,&p); goto done;
        }
        passing[i]=!api_capacity_snapshot(tracker,&candidates[i],current,&prices[i]) &&
            !strcmp(lmb_capacity_evaluate(&candidates[i],current,(uint64_t)time(NULL),p.max_age,p.ttft_ms/1000.0,p.gap_ms/1000.0),
                "observed_workload_passed");
    }
    const char *reason; int next=lmb_portfolio_policy_choose(&p,passing,prices,(uint64_t)time(NULL),&reason);
    snprintf(p.reason,sizeof p.reason,"%s",reason);
    if (next<0) { (void)lmb_policy_save(dir,&p); goto done; }
    char values[4+LMB_CAPACITY_MODELS][80],*args[4+LMB_CAPACITY_MODELS];
    snprintf(values[0],sizeof values[0],"%s",p.names[next]);
    snprintf(values[1],sizeof values[1],"%u",p.ttft_ms); snprintf(values[2],sizeof values[2],"%u",p.gap_ms);
    snprintf(values[3],sizeof values[3],"%u",p.max_age);
    for (uint32_t i=0;i<p.models;i++) {
        char id[65]; lmb_hex(id,p.model_ids[i],32); snprintf(values[4+i],sizeof values[0],"%s:%u",id,p.route_revisions[i]);
    }
    for (uint32_t i=0;i<4+p.models;i++) args[i]=values[i];
    ApiPolicyIntent intent={dir,(uint32_t)next,&p};
    ApiCapacityGuard guard={.price=prices[next],.before_publish=api_policy_intent,.opaque=&intent};
    memcpy(guard.record_digest,p.records[next],32);
    Cap reply={0}; int status=api_capacity_apply_checked(access,tracker,args,p.models+4,&reply,&guard); free(reply.p);
    /* The intent may have published even when confirmation/fsync failed. Do
     * not infer rollback from status; reconcile exactly on the next tick. */
    if (!api_policy_registry(access,registry)) {
        int changed=lmb_policy_reconcile(&p,registry,(uint64_t)time(NULL));
        if (!changed && !p.faulted) strcpy(p.reason,"application_not_published");
        (void)lmb_policy_save(dir,&p);
    }
    fprintf(stderr,"[portfolio-policy] revision=%u target=%u status=%d state=%s weights=unchanged\n",
        p.revision,(unsigned)next,status,p.reason);
done:
    if (records>=0) close(records);
    if (lock>=0) close(lock);
    if (dir>=0) close(dir);
    free(registry); free(candidates);
}
static int api_policy_cli(int access,const char *tracker,char *const *args,unsigned count) {
    Cap response={0}; int status;
    if (!count) status=api_policy_status(access,tracker,&response);
    else if (count==1) {
        /* Explicit local file input, never supplied through HTTP. */
        int fd=open(args[0],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); struct stat st; char text[4097];
        int bad=fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>4096;
        if (!bad) bad=lmb_read_full(fd,text,(size_t)st.st_size);
        if (fd>=0) close(fd);
        status=bad ? 400 : api_policy_configure(access,tracker,text,(size_t)st.st_size,&response);
    } else status=400;
    if (response.p) puts(response.p);
    if (status!=200) fprintf(stderr,"Portfolio policy operation not confirmed (status %d). Inspect current policy and route revisions before retrying.\n",status);
    free(response.p); return status==200 ? 0 : 1;
}
#endif
