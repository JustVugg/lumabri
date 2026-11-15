/* Bounded JSON emission shared by GGUF metadata and its planner contracts. */
#ifndef LMB_GGUF_JSON_H
#define LMB_GGUF_JSON_H
#include <stdarg.h>
#include "src/runtime/lumabri_api_json.h"
typedef struct { char *data; size_t length,capacity; int bad; } LmbGgufJson;

static inline void lmb_gguf_json_add(LmbGgufJson *j,const void *data,size_t n) {
    if(j->bad || n>LMB_GGUF_BUDGET-j->length){j->bad=1;return;}
    if(j->length+n+1>j->capacity) {
        size_t cap=(j->length+n+1)*2;
        if(cap>LMB_GGUF_BUDGET+1)cap=LMB_GGUF_BUDGET+1;
        char *p=realloc(j->data,cap);
        if(!p){j->bad=1;return;}j->data=p;j->capacity=cap;
    }
    memcpy(j->data+j->length,data,n);j->length+=n;j->data[j->length]=0;
}
static inline void lmb_gguf_json_printf(LmbGgufJson *j,const char *fmt,...) {
    char line[2048];va_list ap;va_start(ap,fmt);int n=vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
    if(n<0 || (size_t)n>=sizeof line){j->bad=1;return;}lmb_gguf_json_add(j,line,(size_t)n);
}
static inline void lmb_gguf_json_quote(LmbGgufJson *j,const unsigned char *p,size_t n) {
    lmb_gguf_json_add(j,"\"",1);
    for(size_t i=0;i<n;) {
        unsigned k=lmb_json_utf8(p+i,n-i);
        if(!k){j->bad=1;return;}
        if(p[i]=='"' || p[i]=='\\') {lmb_gguf_json_add(j,"\\",1);lmb_gguf_json_add(j,p+i,1);}
        else if(p[i]<32)lmb_gguf_json_printf(j,"\\u%04x",p[i]);
        else lmb_gguf_json_add(j,p+i,k);
        i+=k;
    }
    lmb_gguf_json_add(j,"\"",1);
}
#endif
