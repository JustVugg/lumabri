/* Independent reference: no Lumabri range or resident-engine patches. */
#define _GNU_SOURCE
#define main upstream_cli_main
#include "deepseek_v41.c"
#undef main
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    Model models[2]={{0}};Cfg c;cfg_load(&c,argv[1]);
    for(int i=0;i<2;i++)model_load(&models[i],argv[1],c.n_routed,32);
    FILE *file=fopen(argv[2],"wb");if(!file)return 2;
    float *logits=malloc((size_t)c.vocab*sizeof(float));if(!logits)return 2;
    int tokens[2]={177,28};
    for(int step=0;step<48;step++)for(int chat=0;chat<2;chat++) {
        forward(&models[chat],&tokens[chat],1,logits);
        if(fwrite(logits,sizeof(float),(size_t)c.vocab,file)!=(size_t)c.vocab)return 2;
        tokens[chat]=argmax(logits,c.vocab);
    }
    free(logits);return fclose(file)?2:0;
}
