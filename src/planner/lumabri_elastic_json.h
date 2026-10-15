/* Bounded owner intent. Machine identities, never arbitrary addresses,
 * paths, commands, credentials, or implicit authorization for all nodes. */
#ifndef LMB_ELASTIC_JSON_H
#define LMB_ELASTIC_JSON_H
#include "lumabri_elastic_policy.h"
static inline int lmb_elastic_parse(const char *text,size_t size,LmbElasticPolicy *p,int *configure) {
    memset(p,0,sizeof *p); LmbJsonToken tokens[256]; LmbJson j; int f[18];
    const char *const keys[]={"action","id","revision","route_revision","enabled","min_replicas","max_replicas",
        "cooldown_seconds","idle_seconds","up_rejections","ttft_ms","max_reserved_bytes","source_model",
        "nodes","context","sessions","max_new","adopt_allocations"};
    char action[16],id[65];
    if (!text || size>8192 || lmb_json_parse(&j,text,size,tokens,256) || lmb_json_fields(&j,0,keys,18,f) ||
        f[0]<0 || f[1]<0 || f[2]<0 || lmb_api_json_string(&j,(unsigned)f[0],action,sizeof action) ||
        lmb_api_json_string(&j,(unsigned)f[1],id,sizeof id) || !lmb_preparation_hex(id) ||
        lmb_unhex(p->route.id,id,32) || lmb_json_uint(&j,(unsigned)f[2],&p->revision)) return -1;
    if (!strcmp(action,"disable") || !strcmp(action,"resume") || !strcmp(action,"reconcile")) {
        for (unsigned i=3;i<18;i++) if (f[i]>=0) return -1;
        *configure=!strcmp(action,"reconcile") ? 3 : !strcmp(action,"resume") ? 2 : 0;
        return p->revision && p->revision<UINT32_MAX ? 0 : -1;
    }
    if (strcmp(action,"configure") || p->revision==UINT32_MAX) return -1;
    for (unsigned i=3;i<18;i++) if (f[i]<0) return -1;
    uint32_t *numbers[]={&p->route.revision,&p->min_replicas,&p->max_replicas,&p->cooldown,&p->idle_seconds,
        &p->up_rejections,&p->ttft_ms,&p->request.context,&p->request.sessions,&p->request.max_new};
    const unsigned positions[]={3,5,6,7,8,9,10,14,15,16};
    for (unsigned i=0;i<10;i++) if (lmb_json_uint(&j,(unsigned)f[positions[i]],numbers[i])) return -1;
    if (!p->route.revision || p->route.revision==UINT32_MAX ||
        (tokens[f[4]].kind!=LMB_JSON_TRUE && tokens[f[4]].kind!=LMB_JSON_FALSE)) return -1;
    p->enabled=tokens[f[4]].kind==LMB_JSON_TRUE;
    char amount[24];
    if (lmb_api_json_string(&j,(unsigned)f[11],amount,sizeof amount) || !*amount) return -1;
    for (unsigned i=0;amount[i];i++) {
        if (amount[i]<'0' || amount[i]>'9' || p->max_reserved_bytes>(UINT64_MAX-(unsigned)(amount[i]-'0'))/10) return -1;
        p->max_reserved_bytes=p->max_reserved_bytes*10+(unsigned)(amount[i]-'0');
    }
    if (lmb_api_json_string(&j,(unsigned)f[12],p->request.models[0],64)) return -1;
    strcpy(p->request.action,"start"); p->request.model_count=1;
    for (unsigned which=0;which<2;which++) {
        unsigned field=(unsigned)f[which ? 17 : 13]; const LmbJsonToken *list=&tokens[field];
        if (list->kind!=LMB_JSON_ARRAY || list->children>(which ? LMB_ROUTE_REPLICAS : LMB_PREPARATION_NODES)) return -1;
        for (unsigned at=field+1;at<list->next;at=tokens[at].next) {
            if (lmb_api_json_string(&j,at,id,sizeof id) || !lmb_preparation_hex(id)) return -1;
            if (which) {
                for (uint32_t i=0;i<p->route.count;i++) { char prior[65]; lmb_hex(prior,p->route.replicas[i].allocation,32); if (!strcmp(prior,id)) return -1; }
                if (lmb_unhex(p->route.replicas[p->route.count++].allocation,id,32)) return -1;
            } else strcpy(p->request.nodes[p->request.node_count++],id);
        }
    }
    *configure=1; return lmb_elastic_settings_valid(p) && p->route.count<=p->max_replicas ? 0 : -1;
}
#endif
