/* Same HTTP turn, same prompt/seed, verified visible prefix. The independent
 * elastic controller owns preparation and donor consent; this reader owns no
 * allocation. No old host permit is held while waiting. */
#ifndef LMB_RECOVERY_WAIT_H
#define LMB_RECOVERY_WAIT_H
#include "lumabri_recovery_policy.h"

static int api_recovery_policy_load(int access,const uint8_t id[32],LmbElasticPolicy *policy) {
    int root=lmb_elastic_root(access,0);
    if (root<0) return -1;
    int dir=lmb_elastic_dir(root,id,0); close(root);
    if (dir<0) return -1;
    int rc=lmb_elastic_load(dir,policy); close(dir); return rc ? -1 : 0;
}
static int api_recovery_capture(int access,const LmbModelRoute *original,uint8_t key[32]) {
    LmbElasticPolicy *p=calloc(1,sizeof *p); int bad=!p;
    if (!bad) bad=api_recovery_policy_load(access,original->id,p) ||
        !lmb_elastic_route_equal(original,&p->route) || lmb_recovery_policy_key(p,key);
    free(p); return bad ? -1 : 0;
}
static int api_recovery_user(int access,const LmbApiUser *user,const uint8_t model[32]) {
    LmbApiUser current;
    return !lmb_api_user_load(access,user->name,&current) &&
        !memcmp(current.digest,user->digest,sizeof user->digest) && lmb_api_user_allows(&current,model);
}
static int api_recovery_selected(const LmbElasticPolicy *p,const LmbResidentPlan *selected) {
    if (!selected->execution.count || selected->execution.hybrid || selected->sessions!=p->request.sessions) return 0;
    for (uint32_t i=0;i<selected->execution.count;i++) {
        char node[65]; lmb_hex(node,selected->peer_keys[i],32); int authorized=0;
        for (uint32_t j=0;j<p->request.node_count;j++) authorized|=!strcmp(node,p->request.nodes[j]);
        if (!authorized) return 0;
    }
    for (uint32_t i=0;i<p->route.count;i++)
        if (!memcmp(p->route.replicas[i].allocation,selected->allocation,32))
            return api_route_plan_matches(&p->route,i,selected);
    return 0;
}
static int api_recovery_wait(int fd,int access,const char *tracker,const LmbApiUser *user,
    const LmbModelRoute *original,const uint8_t key[32],uint64_t until,uint32_t max_new,
    const uint8_t *excluded,uint32_t excluded_count,int seeded,Engine *engine,int *permit,
    LmbResidentPlan *selected,int *client_closed,const char **reason) {
    LmbElasticPolicy *p=calloc(1,sizeof *p); int status=503;
    uint64_t prior=lmb_read_deadline_ms,next_notice=0,next_open=0;
    if (prior && prior<until) until=prior;
    lmb_read_deadline_ms=until;
    *reason="Recovery wait expired; response remains incomplete";
    while (p && !api_stopping) {
        uint64_t now=lmb_io_monotonic_ms();
        if (!now || now>=until) break;
        struct pollfd client={fd,POLLIN,0};
        int rc=poll(&client,1,0);
        if (rc<0 && errno==EINTR) continue;
        if (rc) { *client_closed=1; break; }
        if (!api_recovery_user(access,user,original->id)) { *reason="Access revoked during recovery"; break; }
        LmbModelRoute route;
        if (api_recovery_policy_load(access,original->id,p) ||
            api_route_read(access,original->id,tracker,&route) ||
            !lmb_recovery_policy_route(p,key,original,&route)) {
            *reason="Recovery policy or model changed; response remains incomplete"; break;
        }
        if (now>=next_notice) {
            if (api_event(fd,"recovering","{\"phase\":\"waiting\",\"message\":\"Waiting for approved replacement capacity; Stop cancels this response\"}")) {
                *client_closed=1; break;
            }
            next_notice=now+5000;
        }
        const char *error=NULL;
        if (now>=next_open && lmb_recovery_policy_route(p,key,original,&route)==1) {
            next_open=now+1000;
            status=api_model_open_ex(access,tracker,original->id,max_new,engine,permit,selected,&error,
                &route,excluded,excluded_count,seeded,NULL);
        }
        if (!status) {
            /* Opening a host may block. Recheck authority before any text
             * is sent; a published route alone cannot widen this turn. */
            if (api_recovery_policy_load(access,original->id,p) ||
                api_route_read(access,original->id,tracker,&route) ||
                lmb_recovery_policy_route(p,key,original,&route)!=1 || !api_recovery_selected(p,selected) ||
                !api_recovery_user(access,user,original->id) || lmb_io_monotonic_ms()>=until) {
                engine_stop(engine); if (*permit>=0) close(*permit); *permit=-1;
                *reason="Recovery authority changed or expired; response remains incomplete"; status=503;
            }
            break;
        }
        if (status!=503 && status!=429) { *reason="Replacement capacity could not be opened"; break; }
        rc=poll(&client,1,250);
        if ((rc<0 && errno!=EINTR) || rc>0) { *client_closed=1; break; }
    }
    free(p); lmb_read_deadline_ms=prior; return status ? status : 0;
}
#endif
