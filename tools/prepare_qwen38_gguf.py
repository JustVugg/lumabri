"""Model-owned Qwen3.8 GGUF state in disposable Colibri adapter sources."""
from pathlib import Path
import re
from prepare_qwen_variants import once


def prepare(root):
    root=Path(root);path=root/'qwen38_core.h';s=path.read_text()
    s=once(s,'static gguf_ctx g_q38gg, g_q38gg_mtp;\nstatic int g_q38_gguf, g_q38_a_direct, g_q38_dn_tiled;\nstatic int g_q38_gg_mtp_layer=-1;',
           '/* GGUF ownership is per Model, including the arithmetic mode. */')
    s=once(s,'    Cfg c;\n    shards S;','''    Cfg c;
    shards S;
    gguf_ctx lmb_gg, lmb_gg_mtp;
    int lmb_gguf, lmb_mtp_layer;
    uint8_t *lmb_ple_data, *lmb_ple_parts[Q38_MAX_PLE_PARTS];''')
    replacements={'g_q38gg':'m->lmb_gg','g_q38gg_mtp':'m->lmb_gg_mtp',
                  'g_q38_gguf':'m->lmb_gguf','g_q38_a_direct':'m->lmb_gguf',
                  'g_q38_dn_tiled':'m->lmb_gguf','g_q38_gg_mtp_layer':'m->lmb_mtp_layer'}
    for old,new in replacements.items():s=re.sub(r'\b'+old+r'\b',new,s)
    # Carry the owning model through the metadata and read helpers. A global
    # context switch would serialize unrelated models and break OpenMP reads.
    helpers=['q38_gg_layer','q38_gg_name','q38_gg_ctx_of','q38_gg_find','q38_gg','q38_gg_read','q38_gg_read_f32']
    for name in helpers:
        s=re.sub(r'\b'+name+r'\(',name+'(m,',s)
        s=once(s,name+'(m,const ',name+'(Model *m,const ')
    for name,arg in [('q38_vk_src','Slot *ex'),('q38_vk_note','int layer')]:
        s=re.sub(r'\b'+name+r'\(',name+'(m,',s)
        s=once(s,name+'(m,'+arg,name+'(Model *m,'+arg)
    anchor='static void model_init_range(Model *m,const char *snap,int cap,int bits,'
    s=once(s,anchor,'''static void lmb_q38_source_open(Model *m,const char *snap) {
    m->lmb_mtp_layer=-1;
    char path[4096];snprintf(path,sizeof path,"%s/gguf.json",snap);
    int fd=open(path,O_RDONLY);
    if(fd<0) { if(errno==ENOENT)return;perror(path);exit(1); }
    char buf[8192];size_t n=0;int bad=0;
    while(n<sizeof buf-1) {
        ssize_t got=read(fd,buf+n,sizeof buf-1-n);
        if(got<0 && errno==EINTR)continue;
        if(got<0){bad=1;break;}if(!got)break;n+=(size_t)got;
    }
    char extra;ssize_t more=read(fd,&extra,1);close(fd);buf[n]=0;
    Q38_NEED(!bad && more==0 && !memchr(buf,0,n),"invalid GGUF wrapper");
    char *arena=NULL;jval *r=json_parse(buf,&arena);
    const char *file=r?jstr(r,"gguf"):NULL;
    Q38_NEED(file && *file,"GGUF wrapper has no source");
    gguf_path_join(path,sizeof path,snap,file);json_free(r);free(arena);
    Q38_NEED(!gguf_open(&m->lmb_gg,path),"cannot open GGUF: %s",m->lmb_gg.err);
    const char *arch=gguf_get_str(&m->lmb_gg,"general.architecture");
    Q38_NEED(arch && !strcmp(arch,"qwen4exp"),"not a qwen4exp GGUF");
    m->lmb_gguf=1;
}

'''+anchor)
    s=once(s,'    (void)bits; memset(m,0,sizeof(*m)); double t0=now_s();',
           '    (void)bits; memset(m,0,sizeof(*m)); double t0=now_s();\n    lmb_q38_source_open(m,snap);')
    s=once(s,'        m->ple_gg=t; m->ple_part_count=1;',
           '''        m->lmb_ple_data=malloc((size_t)t->nbytes);
        Q38_NEED(m->lmb_ple_data,"cannot allocate resident PLE table");
        q38_gg_read(m,t,m->lmb_ple_data,0,t->nbytes);
        m->resident_weight_bytes+=(uint64_t)t->nbytes;
        m->ple_gg=t; m->ple_part_count=1;''')
    s=once(s,'        q38_gg_read(m,t,raw,row*rb,rb);','        memcpy(raw,m->lmb_ple_data+row*rb,(size_t)rb);')
    a=s.index('static void q38_load_ple(');b=s.index('\nstatic void q38_alloc_state(',a)
    part=s[a:b];end=part.rfind('\n}')
    part=part[:end]+'''
    for(int part=0;part<m->ple_part_count;part++) {
        st_tensor *t=m->ple_parts[part];
        Q38_NEED(t && (t->dtype==0 || t->dtype==1 || t->dtype==2 || t->dtype==4),"unsupported resident PLE table");
        m->lmb_ple_parts[part]=malloc((size_t)t->nbytes);
        Q38_NEED(m->lmb_ple_parts[part],"cannot allocate resident PLE table");
        st_read_range_raw_cap(&m->S,t->fd,t->off,t->nbytes,m->lmb_ple_parts[part],t->nbytes,1,"prepare resident PLE");
        m->resident_weight_bytes+=(uint64_t)t->nbytes;
    }
'''+part[end:];s=s[:a]+part+s[b:]
    a=s.index('static void q38_ple_row(');b=s.index('\nstatic int64_t q38_hash_row(',a)
    part=s[a:b];start=part.index('    if(t->dtype==4){')
    part=part[:start]+'''    const uint8_t *bytes=m->lmb_ple_parts[p];
    Q38_NEED(bytes,"resident PLE table is unavailable");
    int64_t at=local*c->ngram_head_dim;
    if(t->dtype==4)for(int d=0;d<c->ngram_head_dim;d++)out[d]=e4m3_decode(bytes[at+d])*m->ple_weight_scale;
    else if(t->dtype==2)memcpy(out,bytes+at*4,(size_t)c->ngram_head_dim*4);
    else for(int d=0;d<c->ngram_head_dim;d++) {
        uint16_t x;memcpy(&x,bytes+(at+d)*2,2);
        out[d]=t->dtype==0?bf16_to_f32(x):f16_to_f32(x);
    }
}
''';part=part.replace(' st_tensor *t=m->ple_parts[p]; const char *nm=m->ple_part_names[p];',' st_tensor *t=m->ple_parts[p];')
    s=s[:a]+part+s[b:]
    # Preparation-only temporary: never retain an unowned TLS buffer after an
    # engine has been unloaded, especially with a locked allocator.
    s=once(s,'''            static __thread uint8_t *raw; static __thread int64_t cap;
            if(cap<eb){free(raw);raw=(uint8_t*)malloc((size_t)eb);cap=raw?eb:0;if(!raw){fprintf(stderr,"OOM GGUF expert\\n");exit(1);}}''',
           '''            uint8_t *raw=(uint8_t*)malloc((size_t)eb);
            if(!raw){fprintf(stderr,"OOM GGUF expert\\n");exit(1);}''')
    s=once(s,'            gq_r16_pack(st,(uint8_t*)w->data,raw,os[k],is[k]);',
           '            gq_r16_pack(st,(uint8_t*)w->data,raw,os[k],is[k]);free(raw);')
    s=once(s,'    free(m->ple_history); free(m->PLE_conv_state); free(m->c.is_attn); st_destroy(&m->S);',
           '''    free(m->ple_history); free(m->PLE_conv_state); free(m->c.is_attn); st_destroy(&m->S);
    if(m->ehit){for(int i=0;i<rows;i++)free(m->ehit[i]);free(m->ehit);}
    free(m->lmb_ple_data);for(int i=0;i<Q38_MAX_PLE_PARTS;i++)free(m->lmb_ple_parts[i]);
    gguf_close(&m->lmb_gg);gguf_close(&m->lmb_gg_mtp);''')
    path.write_text(s)
    path=root/'qwen38.c';s=path.read_text()
    a=s.index('#endif /* !QWEN38_NO_MAIN || QWEN38_TEST_SERVE */')
    tail=s[a:]
    for old,new in replacements.items():tail=re.sub(r'\b'+old+r'\b',new,tail)
    tail=re.sub(r'\bq38_vk_src\(', 'q38_vk_src(m,',tail)
    s=s[:a]+tail
    anchor='    Cfg *c=&m->c;uint64_t range_bytes=0,range_scales=0;unsigned kinds=0;'
    s=once(s,anchor,anchor+'''
    if(m->lmb_gguf) {
        for(uint32_t layer=begin;layer<end;layer++) {
            const gguf_tensor *t[3];q38_gg_expert_tensors(m,(int)layer,t);
            for(int k=0;k<3;k++) {
                int cols=k==2?c->inter:c->hidden,rows=k==2?c->hidden:c->inter;
                int type=q38_gg_held_type(t[k]->type,rows,cols);
                int64_t rb=gq_row_bytes(type,cols);
                if(rb<=0 || q38_u64_add(&range_bytes,(uint64_t)rb*(uint64_t)rows))return -1;
            }
        }
        *bytes_per_capacity=range_bytes;*fixed_scale_bytes=0;*numeric_kinds=128;return 0;
    }''')
    s=once(s,'        case Q38_EXPERT_FP8_BLOCK:',
           '        case 128: known="qwen38/gguf-hyper-deltanet-tiled/cpu-v1";break;\n        case Q38_EXPERT_FP8_BLOCK:')
    a=s.index('static void q38_segment_numeric_class(');b=s.index('\nstatic int q38_segment_cache_resize(',a)
    body=s[a:b].replace('unsigned numeric_kinds){','unsigned numeric_kinds,const Model *model){')
    body=once(body,'                     numeric_kinds);return;','                     numeric_kinds);break;')
    body=once(body,'    snprintf(out,capacity,"%s",known);','''    if(known)snprintf(out,capacity,"%s",known);
    size_t used=strlen(out);
    if(used<capacity)snprintf(out+used,capacity-used,"/b%d-g%d-k%d",
        model->native_bf16,model->lmb_gguf?gq_exact_mode():0,q38_kv16());''')
    s=s[:a]+body+s[b:]
    # Both public adapters must announce the same settings from their own model.
    for kind,model in [('segment','&e->model'),('edge','model')]:
        a=s.index('static int qwen38_'+kind+'_engine_open(');b=s.index('\n}',a)
        part=s[a:b]
        part=once(part,'sizeof(capabilities->numeric_class),numeric_kinds);',
                  'sizeof(capabilities->numeric_class),numeric_kinds,'+model+');')
        s=s[:a]+part+s[b:]
    s=once(s,'        for (int part=0; part<e->model.ple_part_count; part++) {',
           '        for (int part=0; !e->model.lmb_gguf && part<e->model.ple_part_count; part++) {')
    s=once(s,'''            if (!table || lmb_resident_retain(table->fd,table->off,table->nbytes,
                                              options->memory_limit_bytes)) {''',
           '''            if (!table || !e->model.lmb_ple_parts[part]) {''')
    a=s.index('static void qwen38_edge_engine_destroy(')
    s=s[:a]+once(s[a:],'    st_destroy(&model->S);',
        '    st_destroy(&model->S);gguf_close(&model->lmb_gg);gguf_close(&model->lmb_gg_mtp);')
    a=s.index('static int qwen38_edge_engine_open(');b=s.index('    Cfg *config=&model->c;',a)
    part=s[a:b];start=part.index('    st_init(&model->S,options->model_dir);')
    part=part[:start]+'''    lmb_q38_source_open(model,options->model_dir);
    if(model->lmb_gguf)snprintf(model->prefix,sizeof model->prefix,"model");
    else {
'''+part[start:]+'''    }
'''
    s=s[:a]+part+s[b:];path.write_text(s)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--copy',required=True,type=Path)
    prepare(p.parse_args().copy)
