/* Included only in Lumabri's disposable Qwen3.8 Edge build. Colibri's vision
 * arithmetic and the upstream checkout remain unchanged. */
#include "src/runtime/lumabri_vision.h"
static int lmb_q38_vision_forward(void *owner,const float *patches,uint32_t h,uint32_t w,float *rows) {
    Model *m=owner;
    return q38_vision_forward(&m->vis,patches,(int)h,(int)w,rows);
}
static int lmb_q38_processor(const char *dir,LmbImageRules *rules) {
    char path[4096],json[65537];
    int n=snprintf(path,sizeof path,"%s/preprocessor_config.json",dir);
    if(n<0 || (size_t)n>=sizeof path) return -1;
    int fd=open(path,O_RDONLY); if(fd<0) return errno==ENOENT ? 0 : -1;
    size_t used=0; int failed=0;
    while(used<sizeof json-1) {
        ssize_t got=read(fd,json+used,sizeof json-1-used);
        if(got<0 && errno==EINTR) continue;
        if(got<0) { failed=1; break; }
        if(!got) break;
        used+=(size_t)got;
    }
    close(fd); json[used]=0;
    if(failed || !used || used==sizeof json-1 || memchr(json,0,used)) return -1;
    jval *root=json_parse_checked(json); if(!root || root->t!=J_OBJ) {json_free(root);return -1;}
    const char *const geometry[]={"patch_size","merge_size","temporal_patch_size"};
    uint32_t expected[]={rules->patch,rules->merge,rules->temporal};
    for(unsigned i=0;i<3;i++) { jval *v=json_get(root,geometry[i]);
        if(v && (v->t!=J_NUM || v->num!=expected[i])) failed=1; }
    const char *const arrays[]={"image_mean","image_std"}; float *values[]={rules->mean,rules->std};
    for(unsigned i=0;i<2;i++) { jval *v=json_get(root,arrays[i]); if(!v) continue;
        if(v->t!=J_ARR || v->len!=3) {failed=1;continue;}
        for(unsigned k=0;k<3;k++) { if(v->kids[k]->t!=J_NUM) failed=1; else values[i][k]=(float)v->kids[k]->num; }
    }
    jval *size=json_get(root,"size");
    if(size && size->t!=J_OBJ) failed=1;
    if(size && size->t==J_OBJ) {
        const char *const keys[]={"shortest_edge","longest_edge"}; uint32_t *values2[]={&rules->min_pixels,&rules->max_pixels};
        for(unsigned i=0;i<2;i++) { jval *v=json_get(size,keys[i]); if(!v) continue;
            if(v->t!=J_NUM || v->num<1 || v->num>16777216 || v->num!=(uint32_t)v->num) failed=1;
            else *values2[i]=(uint32_t)v->num;
        }
    }
    const char *const flags[]={"do_resize","do_rescale","do_normalize","do_convert_rgb"};
    for(unsigned i=0;i<4;i++) { jval *v=json_get(root,flags[i]); if(v && (v->t!=J_BOOL || !v->boolean)) failed=1; }
    jval *resample=json_get(root,"resample"),*scale=json_get(root,"rescale_factor");
    if(resample && (resample->t!=J_NUM || resample->num!=3)) failed=1;
    if(scale && (scale->t!=J_NUM || fabs(scale->num-1.0/255.0)>1e-12)) failed=1;
    json_free(root); uint32_t h,w;
    return failed || lmb_image_grid(256,256,rules,&h,&w) ? -1 : 0;
}
static int lmb_q38_vision_open(Model *m,const char *model_dir) {
    q38_load_vision(m);
    if (!m->vis_ready) return 0;
    Cfg *c=&m->c;
    if (c->vis_in_ch!=3 || c->vis_patch>32 || c->vis_merge>4 || c->vis_temporal>4) return -1;
    LmbVisionBinding b={.owner=m,.hidden=(uint32_t)c->hidden,.copies=(uint32_t)c->hc_count,
        .image_token=(uint32_t)c->image_token,.forward=lmb_q38_vision_forward,
        .rules={.patch=(uint32_t)c->vis_patch,.merge=(uint32_t)c->vis_merge,.temporal=(uint32_t)c->vis_temporal,
            .min_pixels=65536,.max_pixels=16777216,.max_tokens=LMB_IMAGE_TOKENS_MAX,
            .mean={.5f,.5f,.5f},.std={.5f,.5f,.5f}}};
    return lmb_q38_processor(model_dir,&b.rules) || lmb_vision_register(&b) ? -1 : 0;
}
static void lmb_q38_vis_linear_free(Q38Linear *l) { free((void *)l->w); free((void *)l->b); }
static void lmb_q38_vis_norm_free(Q38Norm *n) { free((void *)n->w); free((void *)n->b); }
static void lmb_q38_vision_close(Model *m) {
    lmb_vision_unregister(m);
    Q38Vision *v=&m->vis;
    for (int i=0;i<v->depth && v->blocks;i++) {
        Q38VBlock *b=&v->blocks[i];
        lmb_q38_vis_norm_free(&b->norm1); lmb_q38_vis_norm_free(&b->norm2);
        lmb_q38_vis_linear_free(&b->qkv); lmb_q38_vis_linear_free(&b->proj);
        lmb_q38_vis_linear_free(&b->fc1); lmb_q38_vis_linear_free(&b->fc2);
    }
    lmb_q38_vis_linear_free(&v->patch_embed); free((void *)v->pos_embed); free(v->blocks);
    lmb_q38_vis_norm_free(&v->merger_norm);
    lmb_q38_vis_linear_free(&v->merger_fc1); lmb_q38_vis_linear_free(&v->merger_fc2);
    memset(v,0,sizeof *v); m->vis_ready=0;
}
