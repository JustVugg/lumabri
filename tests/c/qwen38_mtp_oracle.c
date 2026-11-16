/* What the MTP head proposes in the unmodified pinned Colibri engine, with
 * the whole model in one process. The household's Edge head must propose
 * exactly these tokens at the same positions (tests/integration/drafts_test.py).
 *
 *   qwen38-mtp-oracle MODEL_DIR PROMPT_COUNT id,id,id,...
 * prints, for every token after the prompt, "position draft draft draft":
 * the head's proposals for the tokens after the one fed at `position`. */
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
    if(argc!=4)return 2;
    char path[4096];snprintf(path,sizeof path,"%s/gguf.json",argv[1]);FILE *f=fopen(path,"rb");
    if(!f)return 2;
    char buf[8192];size_t n=fread(buf,1,sizeof buf-1,f);fclose(f);buf[n]=0;
    char *arena=NULL;jval *root=json_parse(buf,&arena);const char *source=root?jstr(root,"gguf"):NULL;
    if(!source)return 2;
    gguf_path_join(path,sizeof path,argv[1],source);json_free(root);free(arena);
    if(gguf_open(&g_q38gg,path))return 2;
    g_q38_gguf=g_q38_a_direct=g_q38_dn_tiled=1;
    int ids[512],count=0,prompt=atoi(argv[2]);
    for(char *p=argv[3];*p&&count<512;){char *end;long v=strtol(p,&end,10);if(end==p)return 2;ids[count++]=(int)v;p=*end?end+1:end;}
    if(prompt<1||prompt>=count)return 2;
    setenv("Q38_MTP","1",1);
    Model m={0};model_init(&m,argv[1],8,0);
    q38_mtp_attach(&m,m.c.experts);
    if(!m.mtp)return 1;
    m.max_t=512;ensure_kv(&m);
    float *hidden=falloc(m.c.hc_width);
    free(step(&m,ids,prompt,0));
    for(int pos=prompt;pos<count;pos++) {
        int d[3];
        d[0]=q38_mtp_draft(&m,ids[pos],pos,hidden);
        if(d[0]<0)return 1;
        d[1]=q38_mtp_draft_more(&m,hidden,d[0],pos);
        d[2]=q38_mtp_draft_more(&m,hidden,d[1],pos+1);
        printf("%d %d %d %d\n",pos,d[0],d[1],d[2]);
        free(step(&m,ids+pos,1,pos));
    }
    free(hidden);q38_model_free(&m);gguf_close(&g_q38gg);return 0;
}
