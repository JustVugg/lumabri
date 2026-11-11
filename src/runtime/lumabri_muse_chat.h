/* Muse Spark text framing. Contract: Colibri v2.1.0 openai_server.py;
 * attribution and the pinned source are recorded in docs/THIRD_PARTY.md.
 * Requires Cap/cap_add/cap_str. No model or transport ownership. */
#ifndef LMB_MUSE_CHAT_H
#define LMB_MUSE_CHAT_H
#include <time.h>

static inline int lmb_muse_text(Cap *out,const char *text,int instructions) {
    const char *from[]={"<|","<image>","<video>","<audio>","Reasoning effort","Reasoning Effort","reasoning effort","REASONING EFFORT"};
    const char *to[]={"< |","< image>","< video>","< audio>","Reasoning strength","Reasoning Strength","reasoning strength","REASONING STRENGTH"};
    while(*text) {
        unsigned i=0;for(;i<(instructions?8u:4u);i++)if(!strncmp(text,from[i],strlen(from[i])))break;
        if(i<(instructions?8u:4u)) {if(cap_str(out,to[i]))return -1;text+=strlen(from[i]);}
        else if(cap_add(out,text++,1))return -1;
    }
    return 0;
}
static inline int lmb_muse_system(Cap *out,const char *instructions,const char *date) {
    char today[11];Cap escaped={0};
    if(!date) {
        time_t now=time(NULL);struct tm calendar;
#ifdef _WIN32
        if(localtime_s(&calendar,&now))return -1;
#else
        if(!localtime_r(&now,&calendar))return -1;
#endif
        if(!strftime(today,sizeof today,"%Y-%m-%d",&calendar))return -1;
        date=today;
    }
    if(lmb_muse_text(&escaped,instructions?instructions:"",1)) {free(escaped.p);return -1;}
    const char *p=escaped.p?escaped.p:"";int strength=0;
    for(const char *s=p;*s;s++) {
        const char *needle="reasoning strength:";size_t i=0;
        while(needle[i] && s[i] && (s[i]==needle[i] || (s[i]>='A' && s[i]<='Z' && s[i]+32==needle[i])))i++;
        if(!needle[i]) {strength=1;break;}
    }
    int bad=cap_str(out,"<|begin_of_text|><|start|>system<|message|>You are a helpful AI assistant.\n\nKnowledge cutoff: 2026-01-04.") ||
        (*date && (cap_str(out,"\n\nCurrent date: ") || cap_str(out,date) || cap_str(out,"."))) ||
        (*p && (cap_str(out,"\n\n") || cap_str(out,p))) ||
        (!strength && cap_str(out,"\n\nReasoning strength: 512.")) ||
        cap_str(out,"\n\nUse the appropriate recipient for each message:\n\n"
            "- \"self\": private reasoning and tool planning.\n"
            "- \"commentary\": user-visible intermediate updates while the assistant will continue working, including messages sent to the user before or between tool calls.\n"
            "- \"user\": messages that end the assistant turn, such as a completed response or clarification question that waits for the user's reply; do not use for pre-tool updates or partial responses.\n"
            "\n# Valid recipients: \"self\", \"commentary\", \"user\".<|eot|>");
    free(escaped.p);return bad?-1:0;
}

/* A generated header may straddle any DATA boundary. Header buffering is
 * bounded; bodies stream through. Plain completions from reduced fixtures
 * remain plain when no Muse header is present at the beginning. */
typedef struct {
    char pending[1024];size_t used;
    unsigned body,self,visible,gap,started,done,plain;
} LmbMuseStream;
static inline int lmb_muse_body(LmbMuseStream *s,Cap *out,const char *data,size_t n) {
    if(!n)return 0;
    if(!s->self) {
        if(s->gap && cap_str(out,"\n\n"))return -1;
        s->gap=0;s->visible=1;
    }
    return cap_add(out,data,n);
}
static inline int lmb_muse_feed(LmbMuseStream *s,Cap *out,const void *input,size_t bytes) {
    const char *data=input;
    const char *markers[]={"<|eom|>","<|eot|>","<|end_of_text|>","<|start|>"};
    for(size_t i=0;i<bytes && !s->done;i++) {
        if(s->plain) return cap_add(out,data+i,bytes-i);
        if(!data[i] || s->used+1>=sizeof s->pending)return -1;
        s->pending[s->used++]=data[i];s->pending[s->used]=0;
        if(!s->body) {
            const char *end=strstr(s->pending,"<|message|>");
            if(end) {
                size_t len=(size_t)(end-s->pending);s->pending[len]=0;
                const char *to=strstr(s->pending,"to=");
                if(to) {to+=3;size_t n=strcspn(to," \r\n\t");s->self=n==4 && !memcmp(to,"self",4);}
                else s->self=0;
                if(s->self) {if(cap_str(out,"<think>"))return -1;}
                else if(s->visible)s->gap=1;
                s->used=0;s->body=1;s->started=1;continue;
            }
            if(!s->started) {
                const char *p=s->pending;while(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')p++;
                size_t n=strlen(p);
                int possible=!n || !strncmp(p,"to=",n<3?n:3) ||
                    !strncmp(p,"<|message|>",n<11?n:11) || !strncmp(p,"<|start|>assistant",n<18?n:18);
                if(!possible) {s->plain=1;if(cap_add(out,s->pending,s->used))return -1;s->used=0;}
            }
            continue;
        }
        for(;;) {
            int hold=0,matched=-1;
            for(unsigned j=0;j<4;j++) {
                size_t n=strlen(markers[j]);
                if(s->used<=n && !memcmp(s->pending,markers[j],s->used)) {
                    hold=1;if(s->used==n)matched=(int)j;break;
                }
            }
            if(matched>=0) {
                if(s->self && cap_str(out,"</think>"))return -1;
                s->self=0;s->body=0;s->used=0;
                if(matched==1 || matched==2)s->done=1;
                break;
            }
            if(hold || !s->used)break;
            if(lmb_muse_body(s,out,s->pending,1))return -1;
            memmove(s->pending,s->pending+1,--s->used);s->pending[s->used]=0;
        }
    }
    return 0;
}
static inline int lmb_muse_finish(LmbMuseStream *s,Cap *out) {
    if(s->done)return 0;
    int bad=0;
    if(s->body)bad=lmb_muse_body(s,out,s->pending,s->used) || (s->self && cap_str(out,"</think>"));
    else if(!s->started)bad=cap_add(out,s->pending,s->used);
    s->done=1;s->used=0;return bad?-1:0;
}
#endif
