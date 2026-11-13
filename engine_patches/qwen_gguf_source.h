/* Per-engine ownership of qwen36's native GGUF context. Only generated
 * adapter copies include this file; the upstream checkout is not modified.
 * The production loader uses globals, including flags read by OpenMP teams.
 * Serialize activation for every operation which can observe those globals.
 * Do not use thread-local flags: worker threads would see a different mode. */
#ifndef LMB_QWEN_GGUF_SOURCE_H
#define LMB_QWEN_GGUF_SOURCE_H
typedef struct {
    gguf_ctx gg;
    int enabled, dry, plain, a_direct, tiled, rope_perm;
} LmbQwenSource;
static pthread_mutex_t lmb_qwen_source_lock = PTHREAD_MUTEX_INITIALIZER;

static LmbQwenSource lmb_qwen_source_get(void) {
    return (LmbQwenSource){g_gg,g_gguf,g_gguf_dry,g_gguf_plain,
        g_ssm_a_direct,g_dn_tiled,g_q36_rope_perm};
}
static void lmb_qwen_source_set(const LmbQwenSource *s) {
    g_gg=s->gg; g_gguf=s->enabled; g_gguf_dry=s->dry;
    g_gguf_plain=s->plain; g_ssm_a_direct=s->a_direct;
    g_dn_tiled=s->tiled; g_q36_rope_perm=s->rope_perm;
}
static LmbQwenSource lmb_qwen_source_enter(const LmbQwenSource *s) {
    pthread_mutex_lock(&lmb_qwen_source_lock);
    LmbQwenSource previous=lmb_qwen_source_get();
    lmb_qwen_source_set(s); return previous;
}
static void lmb_qwen_source_leave(LmbQwenSource *s,const LmbQwenSource *previous) {
    *s=lmb_qwen_source_get(); lmb_qwen_source_set(previous);
    pthread_mutex_unlock(&lmb_qwen_source_lock);
}
static int lmb_qwen_source_open(LmbQwenSource *s,const char *dir,char *err,size_t cap) {
    memset(s,0,sizeof *s);
    char path[4096],buf[8192];
    int len=snprintf(path,sizeof path,"%s/gguf.json",dir);
    if(len<0 || (size_t)len>=sizeof path) goto invalid;
    int fd=open(path,O_RDONLY);
    if(fd<0) {if(errno==ENOENT)return 0;goto invalid;}
    size_t n=0;int bad=0;
    while(n<sizeof buf-1) {
        int64_t got=read(fd,buf+n,sizeof buf-1-n);
        if(got<0 && errno==EINTR)continue;
        if(got<0){bad=1;break;}if(!got)break;n+=(size_t)got;
    }
    char extra; int64_t got;
    do {got=read(fd,&extra,1);} while(got<0 && errno==EINTR);
    close(fd);
    if(bad || got!=0 || !n || memchr(buf,0,n))goto invalid;
    buf[n]=0;jval *j=json_parse_checked(buf);
    const char *ref=j?jstr(j,"gguf"):NULL;
    const char *format=j?jstr(j,"format"):NULL;
    /* Imported references are relative to the signed checkpoint root. */
    int valid=format && !strcmp(format,"colibri-gguf-v1") && ref && *ref && ref[0]!='/' && !strchr(ref,'\\') && !strchr(ref,':');
    for(const char *p=valid?ref:"";valid && *p;) {
        const char *e=strchr(p,'/');size_t size=e?(size_t)(e-p):strlen(p);
        if(!size || (size==1 && p[0]=='.') || (size==2 && p[0]=='.' && p[1]=='.') || (e && !e[1]))valid=0;
        p=e?e+1:p+size;
    }
    len=valid?snprintf(path,sizeof path,"%s/%s",dir,ref):-1;
    json_free(j);
    if(len<0 || (size_t)len>=sizeof path)goto invalid;
    if(gguf_open(&s->gg,path)) {
        if(err && cap)snprintf(err,cap,"Qwen GGUF: %s",s->gg.err);
        gguf_close(&s->gg);return -1;
    }
    s->enabled=1;s->a_direct=1;s->tiled=1;return 0;
invalid:
    if(err && cap)snprintf(err,cap,"invalid or unreadable Qwen GGUF checkpoint reference");
    return -1;
}
#endif
