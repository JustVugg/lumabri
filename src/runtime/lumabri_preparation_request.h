/* Interface-independent, bounded intent. No client-supplied paths, commands,
 * addresses, backend overrides, credentials or donor approvals. */
#ifndef LMB_PREPARATION_REQUEST_H
#define LMB_PREPARATION_REQUEST_H
#include "lumabri_api_json.h"
#define LMB_PREPARATION_MODELS 8u
#define LMB_PREPARATION_NODES 32u
typedef struct {
    char action[16], operation[65], review[65];
    char models[LMB_PREPARATION_MODELS][64], nodes[LMB_PREPARATION_NODES][65];
    uint32_t model_count, node_count, context, sessions, max_new;
} LmbPreparationRequest;
static inline int lmb_preparation_hex(const char *s) {
    if (strlen(s)!=64) return 0;
    unsigned nonzero=0;
    for (unsigned i=0;i<64;i++) {
        if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return 0;
        nonzero|=s[i]!='0';
    }
    return nonzero!=0;
}
static inline int lmb_preparation_parse(const char *text,size_t size,LmbPreparationRequest *r) {
    memset(r,0,sizeof *r); LmbJsonToken tokens[128]; LmbJson j; int f[8];
    const char *const keys[]={"action","operation","review","models","nodes","context","sessions","max_new"};
    if (!text || !size || size>8192 || lmb_json_parse(&j,text,size,tokens,128) ||
        lmb_json_fields(&j,0,keys,8,f) || f[0]<0 || lmb_api_json_string(&j,(unsigned)f[0],r->action,sizeof r->action)) return -1;
    int start=!strcmp(r->action,"start"), query=!strcmp(r->action,"status") || !strcmp(r->action,"cancel");
    if (!start && !query && strcmp(r->action,"preview")) return -1;
    if (start || query) {
        if (f[1]<0 || lmb_api_json_string(&j,(unsigned)f[1],r->operation,sizeof r->operation) ||
            !lmb_preparation_hex(r->operation)) return -1;
    } else if (f[1]>=0) return -1;
    if (query) {
        for (unsigned i=2;i<8;i++) if (f[i]>=0) return -1;
        return 0;
    }
    if (start) {
        if (f[2]<0 || lmb_api_json_string(&j,(unsigned)f[2],r->review,sizeof r->review) ||
            !lmb_preparation_hex(r->review)) return -1;
    } else if (f[2]>=0) return -1;
    for (unsigned i=3;i<8;i++) if (f[i]<0) return -1;
    if (lmb_json_uint(&j,(unsigned)f[5],&r->context) || !r->context || r->context>131072 ||
        lmb_json_uint(&j,(unsigned)f[6],&r->sessions) || !r->sessions || r->sessions>8 ||
        lmb_json_uint(&j,(unsigned)f[7],&r->max_new) || !r->max_new || r->max_new>16384 || r->max_new>r->context) return -1;
    const LmbJsonToken *list=&tokens[f[3]];
    if (list->kind!=LMB_JSON_ARRAY || !list->children || list->children>LMB_PREPARATION_MODELS) return -1;
    for (unsigned at=(unsigned)f[3]+1;at<list->next;at=tokens[at].next) {
        char *name=r->models[r->model_count];
        if (lmb_api_json_string(&j,at,name,64) || !*name || !strcmp(name,".") || !strcmp(name,"..")) return -1;
        for (const unsigned char *p=(const unsigned char *)name;*p;p++) if (*p<32 || *p==127 || *p=='/' || *p=='\\') return -1;
        for (uint32_t i=0;i<r->model_count;i++) if (!strcmp(name,r->models[i])) return -1;
        r->model_count++;
    }
    list=&tokens[f[4]];
    if (list->kind!=LMB_JSON_ARRAY || !list->children || list->children>LMB_PREPARATION_NODES) return -1;
    for (unsigned at=(unsigned)f[4]+1;at<list->next;at=tokens[at].next) {
        char *id=r->nodes[r->node_count];
        if (lmb_api_json_string(&j,at,id,65) || !lmb_preparation_hex(id)) return -1;
        for (uint32_t i=0;i<r->node_count;i++) if (!strcmp(id,r->nodes[i])) return -1;
        r->node_count++;
    }
    return 0;
}
#endif
