/* Bounded public configuration, no file paths, machine addresses, executable
 * commands or donor approvals. Money is a decimal micro-unit string. */
#ifndef LMB_PORTFOLIO_POLICY_JSON_H
#define LMB_PORTFOLIO_POLICY_JSON_H
#include "lumabri_portfolio_policy.h"
#include "src/runtime/lumabri_api_json.h"
static inline int lmb_policy_money(const LmbJson *j,int index,uint64_t max,uint64_t *out) {
    char s[24]; if (index<0 || lmb_api_json_string(j,(unsigned)index,s,sizeof s) || !*s) return -1;
    uint64_t n=0;
    for (unsigned i=0;s[i];i++) {
        if (s[i]<'0' || s[i]>'9' || n>max/10 || (n==max/10 && (uint64_t)(s[i]-'0')>max%10)) return -1;
        n=n*10+(unsigned)(s[i]-'0');
    }
    *out=n; return 0;
}
/* Return configure=1 or disable=0. revision is the expected POLICY revision;
 * model revisions separately fence every route. Runtime fields stay zero. */
static inline int lmb_policy_parse(const char *body,size_t size,LmbPortfolioPolicy *p,int *configure) {
    memset(p,0,sizeof *p); LmbJsonToken tokens[160]; LmbJson j; int fields[15];
    const char *const keys[]={"action","revision","records","current","models","ttft_ms","gap_ms",
        "max_age_seconds","cooldown_seconds","horizon_seconds","min_saving_bps","currency",
        "ceiling_micro_per_hour","switch_cost_micro","enabled"};
    char action[16];
    if (size>4096 || lmb_json_parse(&j,body,size,tokens,160) || lmb_json_fields(&j,0,keys,15,fields) ||
        fields[0]<0 || fields[1]<0 || lmb_api_json_string(&j,(unsigned)fields[0],action,sizeof action) ||
        lmb_json_uint(&j,(unsigned)fields[1],&p->revision)) return -1;
    if (!strcmp(action,"disable")) {
        for (unsigned i=2;i<15;i++) if (fields[i]>=0) return -1;
        *configure=0; return p->revision ? 0 : -1;
    }
    if (strcmp(action,"configure") || p->revision==UINT32_MAX) return -1;
    for (unsigned i=2;i<15;i++) if (fields[i]<0) return -1;
    uint32_t *numbers[]={&p->current,&p->ttft_ms,&p->gap_ms,&p->max_age,&p->cooldown,&p->horizon,&p->min_saving_bps};
    const unsigned positions[]={3,5,6,7,8,9,10};
    for (unsigned i=0;i<7;i++) if (lmb_json_uint(&j,(unsigned)fields[positions[i]],numbers[i])) return -1;
    if (lmb_api_json_string(&j,(unsigned)fields[11],p->currency,sizeof p->currency) ||
        lmb_policy_money(&j,fields[12],UINT64_C(32000000000000),&p->ceiling_micro_per_hour) ||
        lmb_policy_money(&j,fields[13],UINT64_C(768000000000000),&p->switch_cost_micro)) return -1;
    if (tokens[fields[14]].kind!=LMB_JSON_TRUE && tokens[fields[14]].kind!=LMB_JSON_FALSE) return -1;
    p->enabled=tokens[fields[14]].kind==LMB_JSON_TRUE;
    const LmbJsonToken *list=&tokens[fields[2]];
    if (list->kind!=LMB_JSON_ARRAY || list->children<2 || list->children>LMB_CAPACITY_MODELS) return -1;
    for (unsigned at=(unsigned)fields[2]+1;at<list->next;at=tokens[at].next) {
        if (lmb_api_json_string(&j,at,p->names[p->candidates],33) || !lmb_api_username(p->names[p->candidates])) return -1;
        p->candidates++;
    }
    list=&tokens[fields[4]];
    if (list->kind!=LMB_JSON_ARRAY || !list->children || list->children>LMB_CAPACITY_MODELS) return -1;
    for (unsigned at=(unsigned)fields[4]+1;at<list->next;at=tokens[at].next) {
        const char *const mk[]={"id","revision"}; int f[2]; char id[65];
        if (lmb_json_fields(&j,at,mk,2,f) || f[0]<0 || f[1]<0 ||
            lmb_api_json_string(&j,(unsigned)f[0],id,sizeof id) || strlen(id)!=64 ||
            lmb_unhex(p->model_ids[p->models],id,32) || lmb_json_uint(&j,(unsigned)f[1],&p->route_revisions[p->models]) ||
            !p->route_revisions[p->models] || p->route_revisions[p->models]==UINT32_MAX) return -1;
        p->models++;
    }
    *configure=1; return 0;
}
#endif
