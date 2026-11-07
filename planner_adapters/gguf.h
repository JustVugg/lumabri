/* Bounded GGUF header inventory. Uses positioned descriptors so a CAS mirror
 * can fetch header blocks without materializing the weight payload. Values
 * retain little-endian wire bytes, including length-prefixed string arrays. */
#ifndef LMB_PLAN_GGUF_H
#define LMB_PLAN_GGUF_H
#include "gguf_types.h"
#include "tensors.h"
#define LMB_GGUF_BUDGET (128u*1024u*1024u)
typedef struct {
    char key[256];
    uint32_t type, element_type;
    uint64_t count, bytes;
    unsigned char *data;
} LmbGgufValue;
typedef struct {
    char name[512];
    uint32_t type, rank, file;
    uint64_t shape[4], offset, bytes;
} LmbGgufTensor;
typedef struct {
    LmbGgufValue *values; uint32_t nvalues;
    LmbGgufTensor *tensors; uint32_t ntensors;
    uint64_t header_bytes, file_bytes;
    char **files; uint32_t nfiles; /* portable paths, populated by a model inventory */
} LmbGguf;
typedef int (*LmbGgufReadAt)(void *,void *,size_t,uint64_t);
typedef struct {
    int fd; uint64_t pos, size, budget;
    LmbGgufReadAt read_at; void *context;
} LmbGgufReader;

static inline uint64_t lmb_gguf_le(const unsigned char *p,unsigned n) {
    uint64_t v=0;for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i);return v;
}
static inline int lmb_gguf_at(const LmbGgufReader *r,void *p,size_t n,uint64_t at) {
    return r->read_at ? r->read_at(r->context,p,n,at) : lmb_plan_read_at(r->fd,p,n,at);
}
static inline int lmb_gguf_read(LmbGgufReader *r,void *p,size_t n) {
    if (r->pos>r->size || n>r->size-r->pos || n>r->budget ||
        lmb_gguf_at(r,p,n,r->pos)) return -1;
    r->pos+=n;r->budget-=n;return 0;
}
static inline int lmb_gguf_u(LmbGgufReader *r,unsigned bytes,uint64_t *out) {
    unsigned char data[8];if(bytes>8 || lmb_gguf_read(r,data,bytes)) return -1;
    *out=lmb_gguf_le(data,bytes);return 0;
}
static inline int lmb_gguf_string(LmbGgufReader *r,char *dst,size_t cap) {
    uint64_t n;if(lmb_gguf_u(r,8,&n) || !n || n>=cap || lmb_gguf_read(r,dst,(size_t)n)) return -1;
    if(memchr(dst,0,(size_t)n)) return -1;
    for(size_t i=0;i<(size_t)n;i++) if((unsigned char)dst[i]<32) return -1;
    dst[n]=0;return 0;
}
static inline unsigned lmb_gguf_scalar_bytes(uint32_t type) {
    static const unsigned char sizes[]={1,1,2,2,4,4,4,1,0,0,8,8,8};
    return type<sizeof sizes ? sizes[type] : 0;
}
static inline void lmb_gguf_free(LmbGguf *g) {
    for(uint32_t i=0;i<g->nvalues;i++) free(g->values[i].data);
    for(uint32_t i=0;i<g->nfiles;i++)free(g->files[i]);
    free(g->files);
    free(g->values);free(g->tensors);memset(g,0,sizeof *g);
}
static inline int lmb_gguf_value_cmp(const void *a,const void *b) {
    return strcmp(((const LmbGgufValue *)a)->key,((const LmbGgufValue *)b)->key);
}
static inline int lmb_gguf_tensor_cmp(const void *a,const void *b) {
    return strcmp(((const LmbGgufTensor *)a)->name,((const LmbGgufTensor *)b)->name);
}
static inline const LmbGgufValue *lmb_gguf_get(const LmbGguf *g,const char *key) {
    uint32_t lo=0,hi=g->nvalues;
    while(lo<hi) {uint32_t mid=lo+(hi-lo)/2;int d=strcmp(g->values[mid].key,key);
        if(!d) return &g->values[mid];
        if(d<0)lo=mid+1;else hi=mid;}
    return NULL;
}
static inline int lmb_gguf_integer(const LmbGgufValue *v,uint64_t *out) {
    if(!v || (v->type!=0 && v->type!=1 && v->type!=2 && v->type!=3 && v->type!=4 &&
              v->type!=5 && v->type!=7 && v->type!=10 && v->type!=11)) return -1;
    if((v->type==1 || v->type==3 || v->type==5 || v->type==11) && (v->data[v->bytes-1]&0x80))return -1;
    *out=lmb_gguf_le(v->data,(unsigned)v->bytes);return 0;
}
static inline const char *lmb_gguf_text(const LmbGgufValue *v) {
    return v && v->type==8 && !memchr(v->data,0,(size_t)v->bytes) ? (const char *)v->data : NULL;
}
/* The caller owns the open file. No weight byte is read, even at EOF. */
static inline int lmb_gguf_parse_reader(LmbGgufReader r,LmbGguf *out) {
    LmbGguf g={0};
    unsigned char magic[4];uint64_t version,nt,nv,alignment=32;
    if(lmb_gguf_read(&r,magic,4) || memcmp(magic,"GGUF",4) ||
       lmb_gguf_u(&r,4,&version) || version<2 || version>3 || lmb_gguf_u(&r,8,&nt) ||
       lmb_gguf_u(&r,8,&nv) || nt>131072 || nv>65536 || !nt) goto fail;
    uint64_t allocations=nt*sizeof *g.tensors+nv*sizeof *g.values;
    if(allocations>r.budget) goto fail;
    r.budget-=allocations;
    g.values=calloc((size_t)nv+1,sizeof *g.values);g.tensors=calloc((size_t)nt,sizeof *g.tensors);
    if(!g.values || !g.tensors) goto fail;
    for(uint32_t i=0;i<nv;i++) {
        LmbGgufValue *v=&g.values[g.nvalues++];uint64_t kind,bytes=0;
        if(lmb_gguf_string(&r,v->key,sizeof v->key) || lmb_gguf_u(&r,4,&kind) || kind>12) goto fail;
        v->type=(uint32_t)kind;v->count=1;v->element_type=v->type;
        if(kind==9) {
            if(lmb_gguf_u(&r,4,&kind) || kind>12 || kind==9 || lmb_gguf_u(&r,8,&v->count)) goto fail;
            v->element_type=(uint32_t)kind;
        }
        if(kind==8 && v->type==9) {
            uint64_t start=r.pos;
            /* Validate the encoded array with bounded seeks; read it once below. */
            if(v->count>r.budget/8 || v->count>(r.size-r.pos)/8) goto fail;
            for(uint64_t j=0;j<v->count;j++) {
                uint64_t n;
                if(lmb_gguf_u(&r,8,&n) || n>r.budget || n>r.size-r.pos) goto fail;
                r.pos+=n;r.budget-=n;
            }
            bytes=r.pos-start;
            if(bytes>r.budget) goto fail; /* payload allocation, additional to the read budget */
            v->data=malloc((size_t)bytes+1);if(!v->data) goto fail;
            r.budget-=bytes;
            if(lmb_gguf_at(&r,v->data,(size_t)bytes,start)) goto fail;
        } else {
            if(kind==8) {if(lmb_gguf_u(&r,8,&bytes)) goto fail;}
            else {unsigned size=lmb_gguf_scalar_bytes((uint32_t)kind);
                if(!size || v->count>UINT64_MAX/size)goto fail;
                bytes=v->count*size;}
            if(bytes>r.budget/2 || bytes>r.size-r.pos) goto fail;
            v->data=malloc((size_t)bytes+1);if(!v->data)goto fail;
            r.budget-=bytes;
            if(lmb_gguf_read(&r,v->data,(size_t)bytes))goto fail;
        }
        v->bytes=bytes;v->data[bytes]=0;
        if(!strcmp(v->key,"general.alignment") &&
           (v->type!=4 || lmb_gguf_integer(v,&alignment))) goto fail;
    }
    if(!alignment || alignment>(1u<<20) || (alignment&(alignment-1)))goto fail;
    qsort(g.values,g.nvalues,sizeof *g.values,lmb_gguf_value_cmp);
    for(uint32_t i=1;i<g.nvalues;i++)if(!strcmp(g.values[i-1].key,g.values[i].key))goto fail;
    for(uint32_t i=0;i<nt;i++) {
        LmbGgufTensor *t=&g.tensors[g.ntensors++];uint64_t rank,type;
        if(lmb_gguf_string(&r,t->name,sizeof t->name) || lmb_gguf_u(&r,4,&rank) || !rank || rank>4)goto fail;
        t->rank=(uint32_t)rank;uint64_t rows=1;
        for(unsigned d=0;d<rank;d++) {
            if(lmb_gguf_u(&r,8,&t->shape[d]) || !t->shape[d] || t->shape[d]>INT64_MAX) goto fail;
            if(d) {if(rows>INT64_MAX/t->shape[d]) goto fail;rows*=t->shape[d];}
        }
        if(lmb_gguf_u(&r,4,&type) || type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES ||
           lmb_gguf_u(&r,8,&t->offset) || t->offset%alignment)goto fail;
        const LmbGgufType *q=&LMB_GGUF_TYPES[type];t->type=(uint32_t)type;
        if(!q->block || t->shape[0]%q->block)goto fail;
        uint64_t blocks=t->shape[0]/q->block;
        if(blocks>INT64_MAX/q->bytes || blocks*q->bytes>INT64_MAX/rows)goto fail;
        t->bytes=blocks*q->bytes*rows;
    }
    if(r.pos>UINT64_MAX-(alignment-1))goto fail;
    g.header_bytes=(r.pos+alignment-1)&~(alignment-1);g.file_bytes=r.size;
    if(g.header_bytes>r.size)goto fail;
    for(uint32_t i=0;i<g.ntensors;i++) {
        LmbGgufTensor *t=&g.tensors[i];
        if(t->offset>r.size-g.header_bytes || t->bytes>r.size-g.header_bytes-t->offset)goto fail;
        t->offset+=g.header_bytes;
    }
    qsort(g.tensors,g.ntensors,sizeof *g.tensors,lmb_gguf_tensor_cmp);
    for(uint32_t i=1;i<g.ntensors;i++)if(!strcmp(g.tensors[i-1].name,g.tensors[i].name))goto fail;
    *out=g;return 0;
fail:lmb_gguf_free(&g);memset(out,0,sizeof *out);return -1;
}
static inline int lmb_gguf_parse(int fd,LmbGguf *out) {
    LmbGgufReader r={.fd=fd,.budget=LMB_GGUF_BUDGET};
    if(lmb_plan_file_size(fd,&r.size)){memset(out,0,sizeof *out);return -1;}
    return lmb_gguf_parse_reader(r,out);
}
#endif
