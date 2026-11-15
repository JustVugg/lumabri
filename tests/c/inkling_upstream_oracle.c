/* Monolithic oracle from the unmodified pinned Colibri engine. */
#define INKLING_NO_MAIN
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "inkling.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    FILE *out=fopen(argv[2],"w");if(!out)return 2;
    Model m={0};model_init(&m,argv[1],8,0);kv_alloc(&m,128);
    int tokens[40]={1,2,3,4,5,6,7,8};
    for(int pos=8;pos<40;pos++) {
        float *logits=step(&m,pos==8?tokens:tokens+pos-1,pos==8?8:1,pos==8?0:pos-1,NULL);
        if(!logits)return 1;
        int best=0;
        for(int i=0;i<m.c.unpad_vocab;i++){if(!isfinite(logits[i]))return 1;if(logits[i]>logits[best])best=i;}
        tokens[pos]=best;free(logits);
    }
    fprintf(out,"{\"prompt_ids\":[1,2,3,4,5,6,7,8],\"full_ids\":[");
    for(int i=0;i<40;i++)fprintf(out,"%s%d",i?",":"",tokens[i]);
    fputs("]}\n",out);return fclose(out)?1:0;
}
