/* Lumabri-owned V4.1 single-row shared-state codec. Included after the pinned
 * engine types. All indices are exactly representable binary32 integers;
 * tensor values retain their original binary32 bits. No native pointers on wire.
 * Full KV remains session-local; each owner sends at most ONE newly closed row.
 */
#ifndef LMB_V41_BOUNDARY_H
#define LMB_V41_BOUNDARY_H

static size_t lmb_v41_aux_width(const Cfg *c) {
    size_t n=1 + 3 + (size_t)c->candidate_topk_blocks + 2 + (size_t)c->index_topk;
    for(int i=0;i<c->n_layers;i++) if(c->kv_source[i]) n+=1+(size_t)c->head_dim+c->index_head_dim;
    return n;
}

static int lmb_v41_wire_int(float f, int lo, int hi, int *out) {
    if(!isfinite(f) || f<(float)lo || f>(float)hi || truncf(f)!=f) return -1;
    *out=(int)f; return 0;
}

static int lmb_v41_aux_write(Model *m, int counts[V41_MAX_LAYERS], float *out, size_t size) {
    Cfg *c=&m->c;
    if(size!=lmb_v41_aux_width(c) || m->candidate_rows>1 || m->shared_topk_rows>1 ||
       m->shared_topk_width>c->index_topk || m->candidate_width>c->max_positions) return -1;
    *out++=m->published_index_k ? (float)(m->published_index_layer+1) : 0;
    for(int i=0;i<c->n_layers;i++) if(c->kv_source[i]) {
        if(!m->range_enabled || (i>=m->range_begin && i<m->range_end)) counts[i]=m->pos/c->compress_ratio[i];
        *out++=(float)counts[i];
        size_t kv=(size_t)c->head_dim, ik=(size_t)c->index_head_dim;
        if(counts[i]) {
            memcpy(out,m->L[i].ckv+(size_t)(counts[i]-1)*kv,kv*sizeof(float)); out+=kv;
            memcpy(out,m->L[i].ikey+(size_t)(counts[i]-1)*ik,ik*sizeof(float)); out+=ik;
        } else {memset(out,0,(kv+ik)*sizeof(float));out+=kv+ik;}
    }
    *out++=(float)m->candidate_width; *out++=(float)m->candidate_rows;
    float *count=out++; *count=0;
    memset(out,0,(size_t)c->candidate_topk_blocks*sizeof(float));
    if(m->candidate_rows) {
        if(c->candidate_block_size<=0) return -1;
        for(int j=0;j<m->candidate_width;j+=c->candidate_block_size) {
            if(!m->candidates[j]) continue;
            int k=(int)*count;
            if(k>=c->candidate_topk_blocks) return -1;
            out[k]=(float)(j/c->candidate_block_size+1); *count=(float)(k+1);
        }
    }
    out+=c->candidate_topk_blocks;
    *out++=(float)m->shared_topk_rows; *out++=(float)m->shared_topk_width;
    for(int j=0;j<c->index_topk;j++) *out++=m->shared_topk_rows && j<m->shared_topk_width ? (float)m->shared_topk[j] : -1.f;
    return 0;
}

static int lmb_v41_aux_read(Model *m, int counts[V41_MAX_LAYERS], const float *in, size_t size) {
    Cfg *c=&m->c; const float *p=in; int pub, incoming[V41_MAX_LAYERS]={0};
    int width,rows,blocks,toprows,topwidth;
    if(size!=lmb_v41_aux_width(c) || lmb_v41_wire_int(*p++,0,c->n_layers,&pub) ||
       (pub && !c->kv_source[pub-1])) return -1;
    for(int i=0;i<c->n_layers;i++) if(c->kv_source[i]) {
        int n;
        int expected=(m->pos+(i<m->range_begin ? 1 : 0))/c->compress_ratio[i];
        if(lmb_v41_wire_int(*p++,0,c->max_positions/c->compress_ratio[i],&n) ||
           n!=expected || n<counts[i] || n>counts[i]+1) return -1;
        incoming[i]=n;
        for(int j=0;j<c->head_dim+c->index_head_dim;j++) if(!isfinite(*p++)) return -1;
    }
    if(lmb_v41_wire_int(*p++,0,c->max_positions,&width) ||
       lmb_v41_wire_int(*p++,0,1,&rows) ||
       lmb_v41_wire_int(*p++,0,c->candidate_topk_blocks,&blocks) ||
       (!rows && (width || blocks)) || (rows && c->candidate_block_size<=0)) return -1;
    int previous=0;
    for(int j=0;j<c->candidate_topk_blocks;j++) {
        int block;
        if(lmb_v41_wire_int(*p++,0,c->max_positions,&block) ||
           (j<blocks ? block<=previous || !width || (uint64_t)(block-1)*c->candidate_block_size>=(uint64_t)width : block!=0)) return -1;
        previous=block;
    }
    if(lmb_v41_wire_int(*p++,0,1,&toprows) ||
       lmb_v41_wire_int(*p++,0,c->index_topk,&topwidth) || (!toprows && topwidth)) return -1;
    for(int j=0;j<c->index_topk;j++) {
        int at;
        if(lmb_v41_wire_int(*p++,-1,c->max_positions+c->window,&at) || (j>=topwidth && at!=-1)) return -1;
    }
    /* Validate and allocate the whole frame before changing live state. */
    uint8_t *mask=calloc(width ? (size_t)width : 1,1);
    int *top=malloc(topwidth ? (size_t)topwidth*sizeof(int) : sizeof(int));
    if(!mask || !top) {free(mask);free(top);return -1;}
    p=in+1;
    for(int i=0;i<c->n_layers;i++) if(c->kv_source[i]) {
        p++; size_t kv=(size_t)c->head_dim, ik=(size_t)c->index_head_dim;
        if(incoming[i]) {
            memcpy(m->L[i].ckv+(size_t)(incoming[i]-1)*kv,p,kv*sizeof(float));
            memcpy(m->L[i].ikey+(size_t)(incoming[i]-1)*ik,p+kv,ik*sizeof(float));
        }
        p+=kv+ik; counts[i]=incoming[i];
    }
    m->published_index_layer=pub ? pub-1 : 0;
    m->published_index_k=pub ? m->L[pub-1].ikey : NULL;
    p+=3;
    free(m->candidates); m->candidates=mask;
    for(int j=0;j<blocks;j++) {
        int first=((int)p[j]-1)*c->candidate_block_size;
        int last=first+c->candidate_block_size; if(last>width) last=width;
        memset(mask+first,1,(size_t)(last-first));
    }
    m->candidate_width=width; m->candidate_rows=rows;
    p+=c->candidate_topk_blocks+2;
    free(m->shared_topk); m->shared_topk=top;
    for(int j=0;j<topwidth;j++) top[j]=(int)p[j];
    m->shared_topk_rows=toprows; m->shared_topk_width=topwidth;
    return 0;
}
#endif
