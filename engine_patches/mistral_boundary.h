/* Lumabri-owned per-token GLM/Mistral boundary state. Included after
 * Colibri's Model type. KV history stays on its owner; only the current
 * cross-layer latent row and the sparse selection travel with activations. */
#ifndef LMB_MISTRAL_BOUNDARY_H
#define LMB_MISTRAL_BOUNDARY_H

static int lmb_ml4_source(const Cfg *c,int source) {
    if(!c->ml4)return 0;
    for(int i=source+1;i<c->n_layers;i++)if(c->kv_src[i]==source)return 1;
    return 0;
}
static int lmb_glm_boundary(const Cfg *c) {
    return c->ml4 || (c->index_topk>0 && c->index_nh>0 && c->index_hd>0);
}
static size_t lmb_ml4_width(const Cfg *c) {
    size_t n=(size_t)c->hidden;
    if(!lmb_glm_boundary(c))return n;
    for(int i=0;i<c->n_layers;i++)if(lmb_ml4_source(c,i))
        n+=1u+(size_t)c->kv_lora+(c->rope_on[i]?(size_t)c->qk_rope:0u);
    return n+2u+(size_t)c->index_topk;
}
static int lmb_ml4_integer(float f,int lo,int hi,int *out) {
    if(!isfinite(f) || f<(float)lo || f>(float)hi || truncf(f)!=f)return -1;
    *out=(int)f;return 0;
}
static int lmb_ml4_last_index(const Cfg *c,int end) {
    int result=0;
    for(int i=0;i<end;i++)if(c->idx_type[i])result=i+1;
    return result;
}
static int lmb_ml4_validate(Model *m,const float *row,int begin,int position) {
    const Cfg *c=&m->c;const float *p=row+c->hidden;int value;
    for(int i=0;i<c->n_layers;i++)if(lmb_ml4_source(c,i)) {
        if(lmb_ml4_integer(*p++,0,position+1,&value) || value!=(i<begin?position+1:0))return -1;
        int n=c->kv_lora+(c->rope_on[i]?c->qk_rope:0);
        for(int j=0;j<n;j++)if(!isfinite(*p++))return -1;
    }
    int owner=m->has_dsa?lmb_ml4_last_index(c,begin):0,count;
    if(lmb_ml4_integer(*p++,0,c->n_layers,&value) || value!=owner ||
       lmb_ml4_integer(*p++,0,c->index_topk,&count) || (!owner && count) || count>position+1)return -1;
    for(int i=0;i<c->index_topk;i++) {
        if(lmb_ml4_integer(*p++,0,position+1,&value) || (i<count?!value:value!=0))return -1;
    }
    return 0;
}
static int lmb_ml4_input(Model *m,const float *row,int begin) {
    const Cfg *c=&m->c;const float *p=row+c->hidden;
    memset(m->lmb_ml4_cross,0,sizeof m->lmb_ml4_cross);
    for(int i=0;i<c->n_layers;i++)if(lmb_ml4_source(c,i)) {
        p++;if(i<begin)m->lmb_ml4_cross[i]=p;
        p+=(size_t)c->kv_lora+(c->rope_on[i]?(size_t)c->qk_rope:0u);
    }
    if(m->has_dsa) {
        if(m->dsa_scap<c->index_topk) {
            int *selection=calloc((size_t)c->index_topk,sizeof(int));
            int *count=calloc(1,sizeof(int));
            if(!selection || !count){free(selection);free(count);return -1;}
            free(m->dsa_sel);free(m->dsa_nsel);
            m->dsa_sel=selection;m->dsa_nsel=count;m->dsa_scap=c->index_topk;
        }
        m->dsa_nsel[0]=(int)p[1];p+=2;
        for(int i=0;i<c->index_topk;i++)m->dsa_sel[i]=(int)p[i]-1;
    }
    return 0;
}
static void lmb_ml4_output(Model *m,float *row,int begin,int end,int position) {
    const Cfg *c=&m->c;float *p=row+c->hidden;
    for(int i=0;i<c->n_layers;i++)if(lmb_ml4_source(c,i)) {
        int n=c->kv_lora+(c->rope_on[i]?c->qk_rope:0);
        if(i>=begin && i<end) {
            *p=(float)(position+1);
            memcpy(p+1,m->kv->Lc[i]+(size_t)position*c->kv_lora,(size_t)c->kv_lora*sizeof(float));
            if(c->rope_on[i])memcpy(p+1+c->kv_lora,m->kv->Rc[i]+(size_t)position*c->qk_rope,(size_t)c->qk_rope*sizeof(float));
        }
        p+=1+n;
    }
    *p++=m->has_dsa?(float)lmb_ml4_last_index(c,end):0.f;
    int count=m->has_dsa && m->dsa_nsel?m->dsa_nsel[0]:0;
    *p++=(float)count;
    for(int i=0;i<c->index_topk;i++)*p++=i<count?(float)(m->dsa_sel[i]+1):0.f;
    memset(m->lmb_ml4_cross,0,sizeof m->lmb_ml4_cross);
}
#endif
