/* A portable checkpoint references GGUF parts inside its own signed root.
 * Absolute host paths are normalized by import, never sent to another node. */
#ifndef LMB_PLAN_GGUF_MODEL_H
#define LMB_PLAN_GGUF_MODEL_H
#include "gguf.h"
#include <math.h>
static inline int lmb_gguf_number(const LmbGgufValue *v,double fallback,double *out) {
    if(!v){*out=fallback;return 0;}
    if(v->type==6){uint32_t u=(uint32_t)lmb_gguf_le(v->data,4);float f;memcpy(&f,&u,4);*out=f;}
    else if(v->type==12){uint64_t u=lmb_gguf_le(v->data,8);memcpy(out,&u,8);}
    else {uint64_t u;if(lmb_gguf_integer(v,&u))return -1;*out=(double)u;}
    return isfinite(*out)?0:-1;
}
static inline const LmbGgufTensor *lmb_gguf_tensor(const LmbGguf *g,const char *name) {
    uint32_t begin=0,end=g->ntensors;
    while(begin<end) {
        uint32_t mid=begin+(end-begin)/2;int cmp=strcmp(g->tensors[mid].name,name);
        if(cmp<0)begin=mid+1;else if(cmp>0)end=mid;else return &g->tensors[mid];
    }
    return NULL;
}
static inline int lmb_gguf_relative(const char *p) {
    if(!p || !*p || *p=='/' || strchr(p,'\\') || strchr(p,':'))return 0;
    for(const char *s=p;*s;) {
        const char *e=strchr(s,'/');size_t n=e?(size_t)(e-s):strlen(s);
        if(!n || (n==1 && *s=='.') || (n==2 && s[0]=='.' && s[1]=='.'))return 0;
        s=e?e+1:s+n;
        if(e && !*s)return 0;
    }
    return 1;
}
static inline int lmb_gguf_digits(const char *p,unsigned n,unsigned *value) {
    unsigned v=0;for(unsigned i=0;i<n;i++) {if(p[i]<'0'||p[i]>'9')return -1;v=v*10+(unsigned)(p[i]-'0');}
    *value=v;return 0;
}
/* 1 absent, 0 valid, -1 invalid. Header data is bounded across the whole split. */
static inline int lmb_gguf_load(const char *root,const char *ref,LmbGguf *out) {
    memset(out,0,sizeof *out);
    if(!lmb_gguf_relative(ref) || strlen(ref)>=1024)return -1;
    char path[1536];int n;
    unsigned parts=1,first=1;size_t len=strlen(ref),number=0;
    /* Fixed-width split suffix, not an arbitrary '-of-' elsewhere in the path. */
    if(len>=21 && !strcmp(ref+len-5,".gguf") && ref[len-20]=='-' &&
       !memcmp(ref+len-14,"-of-",4)) {
        number=len-19;
        if(lmb_gguf_digits(ref+number,5,&first) || lmb_gguf_digits(ref+len-10,5,&parts) ||
           first!=1 || !parts || parts>256)return -1;
    }
    LmbGguf g={0};uint64_t budget=0,expected_tensors=0;
    g.files=calloc(parts,sizeof *g.files);if(!g.files)return -1;
    g.nfiles=parts;
    for(unsigned part=0;part<parts;part++) {
        char file[1024];strcpy(file,ref);
        if(number) {char digits[16];snprintf(digits,sizeof digits,"%05u",part+1);memcpy(file+number,digits,5);}
        g.files[part]=strdup(file);if(!g.files[part])goto fail;
        n=snprintf(path,sizeof path,"%s/%s",root,file);
        if(n<0 || (size_t)n>=sizeof path)goto fail;
        int fd=lmb_plan_open(path);if(fd<0)goto fail;
        LmbGguf one={0};int rc=lmb_gguf_parse(fd,&one);close(fd);if(rc)goto fail;
        const LmbGgufValue *split=lmb_gguf_get(&one,"split.count");uint64_t value;
        if(split && (lmb_gguf_integer(split,&value) || value!=parts))rc=-1;
        split=lmb_gguf_get(&one,"split.no");
        if(split && (lmb_gguf_integer(split,&value) || value!=part))rc=-1;
        split=lmb_gguf_get(&one,"split.tensors.count");
        if(split) {
            if(lmb_gguf_integer(split,&value) || !value || value>131072 || (expected_tensors && expected_tensors!=value))rc=-1;
            else expected_tensors=value;
        }
        const char *arch=lmb_gguf_text(lmb_gguf_get(&one,"general.architecture"));
        if(!part && !arch)rc=-1;
        if(part && arch) {const char *base=lmb_gguf_text(lmb_gguf_get(&g,"general.architecture"));if(!base || strcmp(base,arch))rc=-1;}
        budget=lmb_size_add(budget,lmb_size_add(one.header_bytes,(uint64_t)one.ntensors*sizeof *one.tensors));
        if(budget>LMB_GGUF_BUDGET || g.ntensors>131072-one.ntensors)rc=-1;
        if(rc){lmb_gguf_free(&one);goto fail;}
        if(!part) {g.values=one.values;g.nvalues=one.nvalues;one.values=NULL;one.nvalues=0;}
        LmbGgufTensor *more=realloc(g.tensors,(size_t)(g.ntensors+one.ntensors)*sizeof *g.tensors);
        if(!more){lmb_gguf_free(&one);goto fail;}g.tensors=more;
        for(uint32_t j=0;j<one.ntensors;j++){one.tensors[j].file=part;g.tensors[g.ntensors++]=one.tensors[j];}
        g.header_bytes=lmb_size_add(g.header_bytes,one.header_bytes);g.file_bytes=lmb_size_add(g.file_bytes,one.file_bytes);
        lmb_gguf_free(&one);
    }
    if(expected_tensors && expected_tensors!=g.ntensors)goto fail;
    qsort(g.tensors,g.ntensors,sizeof *g.tensors,lmb_gguf_tensor_cmp);
    for(uint32_t i=1;i<g.ntensors;i++)if(!strcmp(g.tensors[i-1].name,g.tensors[i].name))goto fail;
    *out=g;return 0;
fail:lmb_gguf_free(&g);return -1;
}
/* 1 absent, 0 valid, -1 invalid. A sidecar cannot escape the signed root. */
static inline int lmb_gguf_model(const char *root,LmbGguf *out) {
    memset(out,0,sizeof *out);
    char path[1536],ref[1024],config[8192];
    int n=snprintf(path,sizeof path,"%s/gguf.json",root);
    if(n<0 || (size_t)n>=sizeof path)return -1;
    FILE *fp=lmb_plan_fopen(path);if(!fp)return errno==ENOENT?1:-1;
    size_t got=fread(config,1,sizeof config-1,fp);int bad=ferror(fp)||fgetc(fp)!=EOF;fclose(fp);
    if(bad || memchr(config,0,got))return -1;
    config[got]=0;
    const char *end=lmb_plan_object_end(lmb_plan_space(config));char fmt[64];
    if(!end || *lmb_plan_space(end) || lmb_json_string(config,"format",fmt,sizeof fmt) ||
       strcmp(fmt,"colibri-gguf-v1") || lmb_json_string(config,"gguf",ref,sizeof ref))return -1;
    return lmb_gguf_load(root,ref,out);
}
#endif
