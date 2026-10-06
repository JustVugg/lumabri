/* Logical model registry and bounded selection among already approved
 * replicas. No loading, credit, provisioning or KV migration. The gateway can
 * visibly replay with an immutable contract and verified byte prefix.
 * Requires the gateway's plan/Engine/JSON helpers. */
#ifndef LMB_MANAGED_MODELS_H
#define LMB_MANAGED_MODELS_H
#include "lumabri_model_routes.h"
#include "lumabri_replica_admission.h"

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
            "\"state\":\"saved_route\",\"revision\":%u,\"policy\":\"%s\"",route->context,route->max_new,route->count,
            route->revision,lmb_route_policy_name(route->policy));
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
    int connected=host_connect(plan->host,NULL,plan->host_key,plan->root,engine,&requested,0);
    if (connected) return connected;
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
#include "lumabri_route_evidence.h"
/* Preferences only reorder independently approved replicas. Every selected
 * donor and actual host is re-authenticated before
 * text leaves this process; a stale registry never recreates an allocation. */
static int api_recovery_member(const LmbModelRoute *contract, const LmbModelRoute *current, uint32_t member) {
    if (!current || member>=current->count || current->count>LMB_ROUTE_REPLICAS) return 0;
    if (!contract) return 1;
    if (contract->count>LMB_ROUTE_REPLICAS) return 0;
    if (memcmp(contract->id,current->id,32) || memcmp(contract->content,current->content,32) ||
        strcmp(contract->adapter,current->adapter) || strcmp(contract->numeric_class,current->numeric_class) ||
        contract->numeric_abi!=current->numeric_abi || contract->greedy_only!=current->greedy_only) return 0;
    for (uint32_t i=0;i<contract->count;i++)
        if (!memcmp(&contract->replicas[i],&current->replicas[member],sizeof contract->replicas[i])) return 1;
    return 0;
}
static int api_model_open_ex(int access, const char *tracker, const uint8_t id[32], uint32_t max_new,
                          Engine *engine, int *permit, LmbResidentPlan *selected, const char **error,
                          const LmbModelRoute *contract, const uint8_t *excluded, uint32_t excluded_count, int require_seed,
                          LmbModelRoute *used_route) {
    *permit=-1;
    LmbModelRoute route; int rc=api_route_read(access,id,tracker,&route);
    if (rc!=LMB_ROUTE_OK && rc!=LMB_ROUTE_MISSING) { *error="model_registry_unavailable"; return 503; }
    LmbResidentPlan *plan=calloc(1,sizeof *plan);
    if (!plan) { *error="allocation_failed"; return 500; }
    int status=503; *error="approved_allocation_unavailable";
    if (rc==LMB_ROUTE_MISSING) {
        if (contract || api_find_plan(tracker,id,plan)) { status=404; *error="resident_plan_not_found"; }
        else if (max_new>plan->max_new) { status=400; *error="max_tokens_exceeds_plan"; }
        else {
            int admitted=lmb_replica_admit(access,plan->allocation,permit);
            if (admitted==LMB_REPLICA_BUSY) { status=429; *error="replicas_busy"; }
            else if (admitted) { *error="admission_unavailable"; }
            else {
                int opened=api_plan_open(plan,engine,max_new);
                if (!opened) status=0;
                else if (opened==HOST_CONNECT_BUSY) { status=429; *error="replicas_busy"; }
            }
        }
    } else if (max_new>route.max_new) { status=400; *error="max_tokens_exceeds_plan"; }
    else {
      uint32_t order[LMB_ROUTE_REPLICAS]; api_route_order(tracker,&route,order);
      for (uint32_t attempt=0; attempt<route.count; attempt++) {
        uint32_t i=order[attempt];
        int skipped=0;
        for (uint32_t k=0;k<excluded_count;k++)
            if (!memcmp(route.replicas[i].allocation,excluded+32*k,32)) skipped=1;
        if (skipped || !api_recovery_member(contract,&route,i)) continue;
        if (api_find_plan(tracker,route.replicas[i].allocation,plan) || !api_route_plan_matches(&route,i,plan)) continue;
        int admitted=lmb_replica_admit(access,plan->allocation,permit);
        if (admitted==LMB_REPLICA_BUSY) { status=429; *error="replicas_busy"; continue; }
        if (admitted) { status=503; *error="admission_unavailable"; break; }
        int opened=api_plan_open(plan,engine,max_new);
        if (opened) {
            if (opened==HOST_CONNECT_BUSY) { status=429; *error="replicas_busy"; }
            close(*permit); *permit=-1; continue;
        }
        if (!api_route_engine_matches(&route,engine) || (require_seed && !engine->request_seed_supported)) {
            engine_stop(engine); close(*permit); *permit=-1; continue;
        }
        char model_id[65], allocation_id[65]; lmb_hex(model_id,route.id,32);
        lmb_hex(allocation_id,route.replicas[i].allocation,32);
        fprintf(stderr,"[model-route] model=%s revision=%u allocation=%s replica=%u/%u\n",
            model_id,route.revision,allocation_id,i+1,route.count);
        status=0; break;
      }
    }
    if (status && *permit>=0) { close(*permit); *permit=-1; }
    if (!status && selected) *selected=*plan;
    if (!status && used_route) { memset(used_route,0,sizeof *used_route); if (!rc) *used_route=route; }
    free(plan); return status;
}
static int api_model_open(int access, const char *tracker, const uint8_t id[32], uint32_t max_new,
    Engine *engine, int *permit, LmbResidentPlan *selected, const char **error, LmbModelRoute *used_route) {
    return api_model_open_ex(access,tracker,id,max_new,engine,permit,selected,error,NULL,NULL,0,0,used_route);
}
static int api_route_policy(int access, const char *tracker, char *const *args, unsigned count) {
    if (count!=2 || !*tracker) return 1;
    uint8_t id[32]; uint32_t policy;
    for (policy=0;policy<=LMB_ROUTE_DECLARED_COST;policy++)
        if (!strcmp(args[1],lmb_route_policy_name(policy))) break;
    if (policy>LMB_ROUTE_DECLARED_COST || strlen(args[0])!=64 || lmb_unhex(id,args[0],32)) return 1;
    int dir=lmb_route_dir(access,0); if (dir<0) return 1;
    LmbModelRoute route; int rc=lmb_route_load(dir,id,&route);
    if (!rc && (strcmp(route.tracker,tracker) || route.revision==UINT32_MAX)) rc=LMB_ROUTE_UNSAFE;
    if (!rc) {
        uint32_t expected=route.revision; route.revision++; route.policy=policy;
        rc=lmb_route_save(dir,&route,expected);
        if (!rc) { Cap record={0}; rc=api_route_record(&record,&route,0,1); if (!rc) puts(record.p); free(record.p); }
    }
    close(dir); return rc ? 1 : 0;
}
/* Local operator only. Each keeper authenticates the original requester and
 * exact allocation before contacting its own Segment child. Partial failure
 * is reported, not interpreted as idle and not silently rolled back. */
static int api_segments_control(const char *tracker, char *const *args, unsigned count) {
    uint8_t allocation[32]; uint32_t action=LMB_NODE_CONTROL_QUERY;
    if (!*tracker || count<1 || count>2 || strlen(args[0])!=64 || lmb_unhex(allocation,args[0],32)) return 1;
    if (count==2) {
        if (!strcmp(args[1],"drain")) action=LMB_NODE_CONTROL_DRAIN;
        else if (!strcmp(args[1],"resume")) action=LMB_NODE_CONTROL_RESUME;
        else if (strcmp(args[1],"status")) return 1;
    }
    LmbResidentPlan *plan=calloc(1,sizeof *plan); if (!plan) return 1;
    if (api_find_plan(tracker,allocation,plan)) { free(plan); return 1; }
    printf("{\"schema\":1,\"allocation\":\"%s\",\"weights_unloaded\":false,\"nodes\":[",args[0]);
    int failed=0;
    for (uint32_t i=0;i<plan->execution.count;i++) {
        LmbNodeControl observed={0},result={0};
        int bad=home_resident_node_control(plan,i,LMB_NODE_CONTROL_QUERY,NULL,&observed);
        if (!bad && action) bad=home_resident_node_control(plan,i,action,&observed,&result);
        else result=observed;
        printf("%s{\"index\":%u,",i ? "," : "",i);
        if (bad) { failed=1; printf("\"state\":\"unknown\"}"); continue; }
        char instance[65]; lmb_hex(instance,result.instance,32);
        printf("\"instance\":\"%s\",\"revision\":%llu,\"state\":\"%s\",\"sessions\":%u,\"admitted_experts\":%u}",
            instance,(unsigned long long)result.revision,
            !result.draining ? "accepting" : result.sessions || result.experts ? "draining" : "drained",
            result.sessions,result.experts);
    }
    printf("]}\n"); free(plan);
    if (failed) fprintf(stderr,"Some segment states are unknown or changed concurrently. Successful mutations remain in effect; no weights were unloaded.\n");
    return failed;
}
/* Host admission is separate from node session/Hybrid admission. Neither
 * operation below nor api_segments_control grants authority to RELEASE. */
static int api_replica_control(const char *tracker, char *const *args, unsigned count) {
    uint8_t allocation[32]; uint32_t action=LMB_HOST_CONTROL_QUERY;
    if (!*tracker || count<1 || count>2 || strlen(args[0])!=64 || lmb_unhex(allocation,args[0],32)) return 1;
    if (count==2) {
        if (!strcmp(args[1],"drain")) action=LMB_HOST_CONTROL_DRAIN;
        else if (!strcmp(args[1],"resume")) action=LMB_HOST_CONTROL_RESUME;
        else if (strcmp(args[1],"status")) return 1;
    }
    LmbResidentPlan *plan=calloc(1,sizeof *plan); if (!plan) return 1;
    LmbHostControl observed={0},result={0};
    int bad=api_find_plan(tracker,allocation,plan) ||
        lmb_host_control_rpc(plan->host,plan->host_key,plan->root,LMB_HOST_CONTROL_QUERY,NULL,&observed);
    if (!bad && action) bad=lmb_host_control_rpc(plan->host,plan->host_key,plan->root,action,&observed,&result);
    else result=observed;
    free(plan);
    if (bad) {
        fprintf(stderr,"Host control unavailable or changed concurrently. No idle state is assumed and no weights were unloaded.\n");
        return 1;
    }
    char instance[65]; lmb_hex(instance,result.instance,32);
    printf("{\"schema\":1,\"allocation\":\"%s\",\"instance\":\"%s\",\"revision\":%llu,"
        "\"state\":\"%s\",\"connections\":%u,\"admitted_requests\":%u,\"weights_unloaded\":false}\n",
        args[0],instance,(unsigned long long)result.revision,
        !result.draining ? "accepting" : result.requests || result.connections ? "draining" : "drained",
        result.connections,result.requests);
    return 0;
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
