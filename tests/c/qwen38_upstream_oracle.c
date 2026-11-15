/* Reference uses the unmodified pinned Colibri monolithic forward pass. */
#define QWEN38_NO_MAIN
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "qwen38.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    char path[4096];snprintf(path,sizeof path,"%s/gguf.json",argv[1]);FILE *f=fopen(path,"rb");
    if(f) {
        char buf[8192];size_t n=fread(buf,1,sizeof buf-1,f);fclose(f);buf[n]=0;
        char *arena=NULL;jval *root=json_parse(buf,&arena);const char *source=root?jstr(root,"gguf"):NULL;
        if(!source)return 2;
        gguf_path_join(path,sizeof path,argv[1],source);json_free(root);free(arena);
        if(gguf_open(&g_q38gg,path))return 2;
        g_q38_gguf=g_q38_a_direct=g_q38_dn_tiled=1;
    }
    Model m={0};model_init(&m,argv[1],8,0);m.max_t=128;ensure_kv(&m);
    int tokens[40]={1,2,3,4,5,6,7,8};
    for(int pos=8;pos<40;pos++) {
        float *logits=step(&m,pos==8?tokens:tokens+pos-1,pos==8?8:1,pos==8?0:pos-1);
        if(!logits)return 1;
        int best=0;
        for(int i=0;i<m.c.vocab;i++){if(!isfinite(logits[i]))return 1;if(logits[i]>logits[best])best=i;}
        tokens[pos]=best;free(logits);
    }
    FILE *out=fopen(argv[2],"w");if(!out)return 2;
    fprintf(out,"{\"prompt_ids\":[1,2,3,4,5,6,7,8],\"full_ids\":[");
    for(int i=0;i<40;i++)fprintf(out,"%s%d",i?",":"",tokens[i]);
    fputs("]}\n",out);q38_model_free(&m);gguf_close(&g_q38gg);return fclose(out)?1:0;
}
