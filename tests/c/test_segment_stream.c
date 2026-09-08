/* Exercise the real streaming state machine with deterministic decoder
 * boundary failures. No model arithmetic is mocked by the separate oracles. */
#define main segment_program_main
#define coli_edge_detokenize stream_fixture_detokenize
#include "../../segment_chat.c"
#undef main
#undef coli_edge_detokenize

static int fixture;
int stream_fixture_detokenize(ColiEdgeEngine *engine,const int32_t *tokens,size_t n,
                              char *out,size_t cap,size_t *bytes,char *error,size_t error_size) {
    (void)engine;(void)tokens;
    const char *text="";
    if(fixture==0) { /* an adapter reports incomplete UTF-8 as an error */
        if(n==2) {snprintf(error,error_size,"incomplete byte prefix");return -1;}
        text=n==1 ? "A" : n==3 ? "A\xc3\xa9" : "A\xc3\xa9!";
    } else if(fixture==1) { /* another replaces every incomplete suffix */
        text=n<4 ? "A\xef\xbf\xbd" : "A\xe2\x82\xac!";
    } else if(fixture==2) {
        snprintf(error,error_size,"permanent decoder failure");return -1;
    } else text=n==1 ? "A" : n==2 ? "A!" : "B!";
    *bytes=strlen(text);
    if(out) {if(cap<=*bytes)return -1;memcpy(out,text,*bytes+1);}
    return 0;
}
typedef struct {char text[128];size_t n;} StreamCapture;
static int capture(void *opaque,GenerationEventKind kind,size_t current,size_t total,
                    const void *text,size_t n) {
    (void)current;(void)total;
    StreamCapture *c=opaque;
    if(kind!=GEN_EVENT_DATA || n>=sizeof c->text-c->n) return -1;
    memcpy(c->text+c->n,text,n);c->n+=n;c->text[c->n]=0;return 0;
}
int main(void) {
    for(fixture=0;fixture<4;fixture++) {
        int32_t tokens[4]={1,2,3,4};char error[128]="",*previous=NULL;
        size_t bytes=0,emitted=0;StreamCapture c={0};int rc=0;
        unsigned count=fixture==3 ? 3 : 4;
        for(unsigned n=1;n<=count;n++) {
            rc=generation_stream_prefix(NULL,tokens,n,-1,0,capture,&c,&previous,&bytes,&emitted,error,sizeof error);
            if(rc) break;
        }
        if(fixture==3) {
            if(!rc || strcmp(c.text,"A")) {free(previous);return 1;}
        } else {
            if(rc) {free(previous);return 2;}
            rc=generation_stream_prefix(NULL,tokens,4,-1,1,capture,&c,&previous,&bytes,&emitted,error,sizeof error);
            if(fixture==2) {
                if(!rc || c.n) {free(previous);return 3;}
            } else if(rc || strcmp(c.text,fixture==0 ? "A\xc3\xa9!" : "A\xe2\x82\xac!")) {
                free(previous);return 4;
            }
        }
        free(previous);
    }
    puts("SEGMENT STREAM: PASS (deferred UTF-8, final validation, immutable emitted prefix)");
    return 0;
}
