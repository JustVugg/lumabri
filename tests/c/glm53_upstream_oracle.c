/* Independent monolithic GLM5-Next oracle, using the pinned upstream source. */
#define GLM53_NO_MAIN
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "glm53.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    FILE *out=fopen(argv[2],"w");if(!out)return 2;
    GModel m={0};model_load(&m,argv[1]);GSession *session=session_open(&m,128);
    int tokens[40]={1,2,3,4,5,6,7,8};
    for(int pos=8;pos<40;pos++) {
        int n=pos==8?8:1;
        float *logits=forward_span(&m,session,pos==8?tokens:tokens+pos-1,n,NULL,0);
        if(!logits)return 1;
        float *last=logits+(size_t)(n-1)*m.c.vocab;int best=0;
        for(int i=0;i<m.c.vocab;i++){if(!isfinite(last[i]))return 1;if(last[i]>last[best])best=i;}
        tokens[pos]=best;free(logits);
    }
    fprintf(out,"{\"prompt_ids\":[1,2,3,4,5,6,7,8],\"full_ids\":[");
    for(int i=0;i<40;i++)fprintf(out,"%s%d",i?",":"",tokens[i]);
    fputs("]}\n",out);session_close(&m,session);model_release(&m);return fclose(out)?1:0;
}
