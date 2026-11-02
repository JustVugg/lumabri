/* Shared household settings data and text format. Persistence and terminal
 * ownership belong to the platform boundary, not this codec. */
#ifndef LMB_HOME_SETTINGS_H
#define LMB_HOME_SETTINGS_H
#include "src/runtime/lumabri_session_limits.h"
#include <math.h>
typedef struct {
    char tracker[256], token[LMB_TOKEN_MAX+1], models[512], ram[32], sessions[12], context[12];
    int owner;
} HomeSettings;

static void home_settings_defaults(HomeSettings *s,const char *models) {
    memset(s,0,sizeof *s);
    snprintf(s->models,sizeof s->models,"%s",models);
    strcpy(s->ram,"4");strcpy(s->sessions,"1");
    snprintf(s->context,sizeof s->context,"%u",LMB_HOME_DEFAULT_CONTEXT);
}

static int home_settings_line(HomeSettings *s,char *line) {
    char *value=strchr(line,'=');if (!value) return -1;
    *value++=0;value[strcspn(value,"\r\n")]=0;
    char *target=NULL;size_t cap=0;
#define HOME_FIELD(name) if (!strcmp(line,#name)) {target=s->name;cap=sizeof s->name;}
    HOME_FIELD(tracker) else HOME_FIELD(token) else HOME_FIELD(models) else HOME_FIELD(ram)
    else if (!strcmp(line,"owner")) {s->owner=!strcmp(value,"1");return 0;}
    else if (!strcmp(line,"sessions")) {
        if (strlen(value)!=1 || value[0]<'1' || value[0]>'8') return -1;
        target=s->sessions;cap=sizeof s->sessions;
    } else if (!strcmp(line,"context")) {
        uint32_t context;if (lmb_context_limit_parse(value,&context)) return -1;
        snprintf(s->context,sizeof s->context,"%u",context);return 0;
    }
#undef HOME_FIELD
    if (!target) return 1; /* preserve compatibility with future optional fields */
    if (strlen(value)>=cap) return -1;
    memcpy(target,value,strlen(value)+1);return 0;
}

static int home_settings_format(const HomeSettings *s,char *text,size_t cap) {
    uint32_t context;
    if (lmb_context_limit_parse(s->context,&context)) return -1;
    const char *fields[]={s->tracker,s->token,s->models,s->ram,s->sessions};
    const size_t sizes[]={sizeof s->tracker,sizeof s->token,sizeof s->models,sizeof s->ram,sizeof s->sessions};
    for (unsigned i=0;i<5;i++) if (strnlen(fields[i],sizes[i])==sizes[i] || strpbrk(fields[i],"\r\n")) return -1;
    int n=snprintf(text,cap,"tracker=%s\ntoken=%s\nmodels=%s\nram=%s\nowner=%d\nsessions=%s\ncontext=%u\n",
        s->tracker,s->token,s->models,s->ram,s->owner,s->sessions[0] ? s->sessions : "1",context);
    return n<0 || (size_t)n>=cap ? -1 : n;
}
#endif
