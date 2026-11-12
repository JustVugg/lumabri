/* Public monolithic API linked to the unmodified pinned V4 object units. */
#include "deepseek_v4.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tok.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
typedef struct {int ids[128],count;} Tokens;
static int record(void *data,int token,float logit,int position,int ordinal) {
    (void)logit;(void)position;(void)ordinal;Tokens *t=data;
    if(t->count>=128)return 1;
    t->ids[t->count++]=token;return 0;
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    const char *prompt="abcdefgh";char path[4096],error[1024]={0};Tok tok={0};Tokens result={0};
    snprintf(path,sizeof path,"%s/tokenizer.json",argv[1]);tok_load(&tok,path);
    int np=tok_encode(&tok,prompt,strlen(prompt),result.ids,128);tok_free(&tok);
    if(np<1 || np>64)return 2;
    result.count=np;
    ColiV4Engine *engine=NULL;ColiV4Session *session=NULL;int rc=1;
    ColiV4EngineOpenOptions eo={.target_model_dir=argv[1],.memory_limit_bytes=UINT64_C(4)<<30,
        .context_tokens=128,.pin_slots_per_layer=0,.no_dspark=1};
    ColiV4SessionCreateOptions so={.max_prompt_tokens=64,.max_new_tokens_cap=32};
    ColiV4SessionGenerateOptions go={.max_new_tokens=32,.no_dspark=1};ColiV4SessionGenerateStats stats={0};
    if(coli_v4_engine_open(&engine,&eo,error,sizeof error) ||
       coli_v4_session_create(&session,engine,&so,error,sizeof error) ||
       coli_v4_session_generate(session,prompt,strlen(prompt),&go,record,&result,&stats,error,sizeof error))goto done;
    if(stats.prompt_tokens!=np || result.count!=np+32){snprintf(error,sizeof error,"reference stopped before 32 tokens");goto done;}
    FILE *out=fopen(argv[2],"w");if(!out)goto done;
    fputs("{\"prompt_ids\":[",out);
    for(int i=0;i<np;i++)fprintf(out,"%s%d",i?",":"",result.ids[i]);
    fputs("],\"full_ids\":[",out);
    for(int i=0;i<result.count;i++)fprintf(out,"%s%d",i?",":"",result.ids[i]);
    fputs("]}\n",out);rc=fclose(out)?1:0;
done:
    if(rc)fprintf(stderr,"V4 reference: %s\n",error);
    coli_v4_session_destroy(session);coli_v4_engine_destroy(engine);return rc;
}
