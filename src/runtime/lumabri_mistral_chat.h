/* Mistral Large 4 text markers at the shared chat boundary. Cap is supplied
 * by lumabri_chat_format.h. The streaming buffer is bounded independently
 * of reply length, including when a marker is split across token chunks. */
#ifndef LMB_MISTRAL_CHAT_H
#define LMB_MISTRAL_CHAT_H
typedef struct { char pending[9]; size_t used; } LmbMistralStream;
static inline int lmb_mistral_feed(LmbMistralStream *s,Cap *out,const void *data,size_t bytes) {
    const char *p=data;
    const char *marks[]={"[THINK]","[/THINK]"},*translated[]={"<think>","</think>"};
    for(size_t i=0;i<bytes;i++) {
        s->pending[s->used++]=p[i];
        while(s->used) {
            int prefix=0,complete=-1;
            for(unsigned k=0;k<2;k++) {
                size_t n=strlen(marks[k]);
                if(s->used<=n && !memcmp(s->pending,marks[k],s->used)) {
                    prefix=1;if(s->used==n)complete=(int)k;
                }
            }
            if(complete>=0) {
                if(cap_str(out,translated[complete]))return -1;
                s->used=0;break;
            }
            if(prefix)break;
            if(cap_add(out,s->pending,1))return -1;
            s->used--;memmove(s->pending,s->pending+1,s->used);
        }
    }
    return 0;
}
static inline int lmb_mistral_finish(LmbMistralStream *s,Cap *out) {
    int rc=cap_add(out,s->pending,s->used);s->used=0;return rc;
}
static inline int lmb_mistral_history(Cap *out,const char *reply) {
    /* The transport normalizes reasoning for the common UI. Reconstruct the
     * model's native markers when that reply becomes conversation history. */
    const char *p=reply;
    while(*p) {
        if(!strncmp(p,"<think>",7)){if(cap_str(out,"[THINK]"))return -1;p+=7;}
        else if(!strncmp(p,"</think>",8)){if(cap_str(out,"[/THINK]"))return -1;p+=8;}
        else {if(cap_add(out,p++,1))return -1;}
    }
    return 0;
}
#endif
