/* Logical model registry and bounded selection among already approved
 * replicas. No loading, credit, provisioning, KV migration or hidden retry
 * after text generation. Requires the gateway's plan/Engine/JSON helpers. */
#ifndef LMB_MANAGED_MODELS_H
#define LMB_MANAGED_MODELS_H
#include "lumabri_model_routes.h"

static int api_route_read(int access, const uint8_t id[32], const char *tracker, LmbModelRoute *route) {
    int dir=lmb_route_dir(access,0);
    if (dir<0) return errno==ENOENT ? LMB_ROUTE_MISSING : LMB_ROUTE_UNSAFE;
    int rc=lmb_route_load(dir,id,route); close(dir);
    return !rc && strcmp(route->tracker,tracker) ? LMB_ROUTE_MISSING : rc;
}
static int api_route_record(Cap *body, const LmbModelRoute *route, int comma, int operator) {
    char id[65], content[65]; lmb_hex(id,route->id,32); lmb_hex(content,route->content,32);
    int bad=api_addf(body,"%s{\"id\":\"%s\",\"name\":",comma ? "," : "",id) || api_json_text(body,route->name) ||
        api_addf(body,",\"context\":%u,\"max_tokens\":%u,\"sessions\":null,\"replicas\":%u,"
            "\"state\":\"saved_route\",\"revision\":%u",route->context,route->max_new,route->count,route->revision);
    if (operator && !bad) {
        bad=api_addf(body,",\"content_id\":\"%s\",\"adapter\":",content) || api_json_text(body,route->adapter) ||
            cap_str(body,",\"numeric_class\":") || api_json_text(body,route->numeric_class) || cap_str(body,",\"allocations\":[");
        for (uint32_t i=0; i<route->count && !bad; i++) {
            lmb_hex(id,route->replicas[i].allocation,32); bad=api_addf(body,"%s\"%s\"",i ? "," : "",id);
        }
        if (!bad) bad=cap_str(body,"]");
    }
    return bad || cap_str(body,"}") ? -1 : 0;
}
static int api_route_append(Cap *body, int access, const char *tracker, const LmbApiUser *user, int *comma) {
    int dir=lmb_route_dir(access,0);
    if (dir<0) return errno==ENOENT ? 0 : -1;
    int lock=lmb_api_access_lock(dir); if (lock<0) { close(dir); return -1; }
    uint8_t ids[LMB_ROUTE_MAX][32]; size_t count; int bad=lmb_route_list(dir,ids,&count);
    for (size_t i=0; i<count && !bad; i++) {
        LmbModelRoute route; bad=lmb_route_load(dir,ids[i],&route); if (bad) break;
        if (strcmp(route.tracker,tracker) || (user && !lmb_api_user_allows(user,route.id))) continue;
        bad=api_route_record(body,&route,(*comma)++,user==NULL);
    }
    close(lock); close(dir); return bad ? -1 : 0;
}
static int api_plan_open(const LmbResidentPlan *plan, Engine *engine, uint32_t max_new) {
    if (max_new>plan->max_new) return -1;
    for (uint32_t i=0; i<plan->execution.count; i++) if (home_resident_peer(plan,i,0)) return -1;
    int requested=(int)max_new;
    if (host_connect(plan->host,NULL,plan->host_key,plan->root,engine,&requested,0)) return -1;
    if (requested!=(int)max_new || engine->proto!=PROTO_SERVE2) { engine_stop(engine); return -1; }
    return 0;
}
static int api_route_engine_matches(const LmbModelRoute *route, const Engine *engine) {
    const LmbModelFamily *family=lmb_family_by_id(route->adapter);
    return family && engine->kind==engine_kind_of(family->engine) && route->numeric_abi==engine->numeric_abi &&
        !strcmp(route->numeric_class,engine->numeric_class) && route->greedy_only==(uint32_t)engine->greedy_only;
}
static int api_route_plan_matches(const LmbModelRoute *route, unsigned index, const LmbResidentPlan *plan) {
    uint8_t content[32], root[32], key[32]; const LmbModelReplica *replica=&route->replicas[index];
    return plan->observation.build_id[0] && !lmb_unhex(content,plan->content_id,32) &&
        !lmb_unhex(root,plan->root,32) && !lmb_unhex(key,plan->host_key,32) &&
        !memcmp(content,route->content,32) && !memcmp(root,replica->root,32) && !memcmp(key,replica->host_key,32) &&
        !memcmp(plan->allocation,replica->allocation,32) && !strcmp(plan->observation.adapter,route->adapter) &&
        plan->context>=route->context && plan->max_new>=route->max_new;
}
/* The ordered replica set is an operator policy, not a claimed cost/speed
 * optimizer. Every selected donor and actual host is re-authenticated before
 * text leaves this process; a stale registry never recreates an allocation. */
static int api_model_open(int access, const char *tracker, const uint8_t id[32], uint32_t max_new,
                          Engine *engine, const char **error) {
    LmbModelRoute route; int rc=api_route_read(access,id,tracker,&route);
    if (rc!=LMB_ROUTE_OK && rc!=LMB_ROUTE_MISSING) { *error="model_registry_unavailable"; return 503; }
    LmbResidentPlan *plan=calloc(1,sizeof *plan);
    if (!plan) { *error="allocation_failed"; return 500; }
    int status=503; *error="approved_allocation_unavailable";
    if (rc==LMB_ROUTE_MISSING) {
        if (api_find_plan(tracker,id,plan)) { status=404; *error="resident_plan_not_found"; }
        else if (max_new>plan->max_new) { status=400; *error="max_tokens_exceeds_plan"; }
        else if (!api_plan_open(plan,engine,max_new)) status=0;
    } else if (max_new>route.max_new) { status=400; *error="max_tokens_exceeds_plan"; }
    else for (uint32_t i=0; i<route.count; i++) {
        if (api_find_plan(tracker,route.replicas[i].allocation,plan) || !api_route_plan_matches(&route,i,plan) ||
            api_plan_open(plan,engine,max_new)) continue;
        if (!api_route_engine_matches(&route,engine)) { engine_stop(engine); continue; }
        char model_id[65], allocation_id[65]; lmb_hex(model_id,route.id,32);
        lmb_hex(allocation_id,route.replicas[i].allocation,32);
        fprintf(stderr,"[model-route] model=%s revision=%u allocation=%s replica=%u/%u\n",
            model_id,route.revision,allocation_id,i+1,route.count);
        status=0; break;
    }
    free(plan); return status;
}
/* Registration checks the real authenticated numeric contract without
 * submitting a prompt. No half-validated replica set is persisted. */
static int api_route_build(const char *tracker, const char *const *allocations, unsigned count, LmbModelRoute *route, int update) {
    if (!count || count>LMB_ROUTE_REPLICAS || strlen(tracker)>=sizeof route->tracker) return -1;
    LmbModelRoute old=*route; route->count=0; route->context=route->max_new=UINT32_MAX;
    snprintf(route->tracker,sizeof route->tracker,"%s",tracker);
    LmbResidentPlan *plan=calloc(1,sizeof *plan); if (!plan) return -1;
    int bad=0;
    for (unsigned i=0; i<count && !bad; i++) {
        LmbModelReplica *replica=&route->replicas[i]; uint8_t content[32];
        if (strlen(allocations[i])!=64 || lmb_unhex(replica->allocation,allocations[i],32) ||
            api_find_plan(tracker,replica->allocation,plan) || !plan->observation.build_id[0] ||
            !home_resident_digest(plan->content_id) || lmb_unhex(content,plan->content_id,32) ||
            lmb_unhex(replica->root,plan->root,32) || lmb_unhex(replica->host_key,plan->host_key,32)) { bad=1; break; }
        for (unsigned k=0; k<i; k++) if (!memcmp(replica->allocation,route->replicas[k].allocation,32)) bad=1;
        if (bad) break;
        Engine engine;
        if (api_plan_open(plan,&engine,1)) { bad=1; break; }
        if (!i) {
            memcpy(route->content,content,32);
            snprintf(route->adapter,sizeof route->adapter,"%s",plan->observation.adapter);
            route->numeric_abi=engine.numeric_abi; route->greedy_only=(uint32_t)engine.greedy_only;
            snprintf(route->numeric_class,sizeof route->numeric_class,"%s",engine.numeric_class);
        }
        bad=memcmp(route->content,content,32) || strcmp(route->adapter,plan->observation.adapter) ||
            !api_route_engine_matches(route,&engine);
        engine_stop(&engine); if (bad) break;
        if (plan->context<route->context) route->context=plan->context;
        if (plan->max_new<route->max_new) route->max_new=plan->max_new;
        route->count++;
    }
    free(plan);
    if (!bad && update) bad=memcmp(old.content,route->content,32) || strcmp(old.tracker,route->tracker) ||
        strcmp(old.adapter,route->adapter) || strcmp(old.numeric_class,route->numeric_class) ||
        old.numeric_abi!=route->numeric_abi || old.greedy_only!=route->greedy_only;
    return bad || !lmb_route_valid(route) ? -1 : 0;
}
static int api_route_admin(int access, const char *action, const char *tracker, char *const *args, unsigned count) {
    int adding=!strcmp(action,"model-add"), removing=!strcmp(action,"model-remove");
    if (!*tracker || (removing ? count!=1 : count<2 || count>LMB_ROUTE_REPLICAS+1)) return 1;
    int dir=lmb_route_dir(access,1); if (dir<0) return 1;
    LmbModelRoute route={0}; uint32_t expected=0; int rc=1;
    if (adding) {
        if (!lmb_api_username(args[0])) goto finished;
        lmb_random(route.id,32); snprintf(route.name,sizeof route.name,"%s",args[0]); route.revision=1;
    } else {
        uint8_t id[32];
        if (strlen(args[0])!=64 || lmb_unhex(id,args[0],32) || lmb_route_load(dir,id,&route) ||
            strcmp(route.tracker,tracker) || route.revision==UINT32_MAX) goto finished;
        expected=route.revision;
        if (removing) { rc=lmb_route_remove(dir,id,expected); goto finished; }
        route.revision++;
    }
    if (api_route_build(tracker,(const char *const *)(args+1),count-1,&route,!adding)) goto finished;
    rc=lmb_route_save(dir,&route,expected);
    if (!rc) {
        Cap record={0}; rc=api_route_record(&record,&route,0,1);
        if (!rc) puts(record.p);
        free(record.p);
    }
finished:
    close(dir);
    if (rc) fprintf(stderr,"Managed model operation failed. Replicas must be live, independently approved and match checkpoint, adapter and numeric class. No weights were loaded or unloaded.\n");
    return rc ? 1 : 0;
}
#endif
