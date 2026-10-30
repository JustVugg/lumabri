#ifndef LUMABRI_PLAN_QWEN38_VISION_H
#define LUMABRI_PLAN_QWEN38_VISION_H
#include "src/runtime/lumabri_image.h"
#include "src/runtime/lumabri_media.h"
typedef struct {
    uint32_t depth,h,heads,inter,patch,merge,temporal,positions,out;
    uint16_t blocks[1024],globals;
    uint64_t weights,largest;
} LmbQ38Vision;
static int lmb_q38_vision_config(const char *whole,uint32_t hidden,uint32_t vocab,LmbQ38Vision *v) {
    const char *j=lmb_json_member(whole,"vision_config");
    if (!j) return 0;
    char activation[64];
    if(lmb_json_member(j,"hidden_act") &&
        (lmb_json_string(j,"hidden_act",activation,sizeof activation) || strcmp(activation,"gelu_pytorch_tanh"))) return -1;
    uint32_t channels,image;
    if (*j!='{' || lmb_plan_u32(j,"depth",1,1024,&v->depth) ||
        lmb_plan_u32(j,"hidden_size",1,65536,&v->h) || lmb_plan_u32(j,"num_heads",1,1024,&v->heads) ||
        lmb_plan_u32(j,"intermediate_size",1,262144,&v->inter) ||
        lmb_plan_optional(j,"patch_size",16,1,32,&v->patch) ||
        lmb_plan_optional(j,"spatial_merge_size",2,1,4,&v->merge) ||
        lmb_plan_optional(j,"temporal_patch_size",2,1,4,&v->temporal) ||
        lmb_plan_optional(j,"in_channels",3,3,3,&channels) ||
        lmb_plan_optional(j,"out_hidden_size",hidden,hidden,hidden,&v->out) ||
        lmb_plan_u32(j,"num_position_embeddings",1,1u<<20,&v->positions) ||
        lmb_plan_u32(whole,"image_token_id",0,vocab-1,&image) || v->h%v->heads || (v->h/v->heads)%4) return -1;
    unsigned side=1; while ((uint64_t)side*side<v->positions) side++;
    return side*side==v->positions ? 0 : -1;
}
/* Return 1 for a visual tensor, 0 for a language tensor, -1 for an invalid
 * or unknown visual layout. Count the F32 allocations actually used by Edge. */
static int lmb_q38_vision_tensor(const LmbPlanTensor *t,LmbQ38Vision *v) {
    const char *n=t->name;
    if (!strncmp(n,"model.language_model.visual.",28)) n+=28;
    else if (!strncmp(n,"model.visual.",13)) n+=13;
    else return 0;
    if (!v->depth || (strcmp(t->dtype,"BF16") && strcmp(t->dtype,"F16") && strcmp(t->dtype,"F32"))) return -1;
    uint64_t H=v->h,W=H*v->merge*v->merge, features=3u*v->temporal*v->patch*v->patch;
    uint64_t count=0; uint16_t *seen=&v->globals; unsigned bit=0;
    static const char *const globals[]={"patch_embed.proj.weight","patch_embed.proj.bias","pos_embed.weight",
        "merger.norm.weight","merger.norm.bias","merger.linear_fc1.weight","merger.linear_fc1.bias",
        "merger.linear_fc2.weight","merger.linear_fc2.bias"};
    uint64_t sizes[]={H*features,H,(uint64_t)v->positions*H,H,H,W*W,W,(uint64_t)v->out*W,v->out};
    if (!strncmp(n,"blocks.",7)) {
        uint64_t layer; const char *field=lmb_plan_uint(n+7,&layer);
        if (!field || *field++!='.' || layer>=v->depth) return -1;
        seen=&v->blocks[layer];
        static const char *const names[]={"norm1.weight","norm1.bias","norm2.weight","norm2.bias",
            "attn.qkv.weight","attn.qkv.bias","attn.proj.weight","attn.proj.bias",
            "mlp.linear_fc1.weight","mlp.linear_fc1.bias","mlp.linear_fc2.weight","mlp.linear_fc2.bias"};
        uint64_t elements[]={H,H,H,H,3*H*H,3*H,H*H,H,H*v->inter,v->inter,H*v->inter,H};
        for (;bit<12;bit++) if (!strcmp(field,names[bit])) { count=elements[bit]; break; }
    } else {
        for (;bit<9;bit++) if (!strcmp(n,globals[bit])) { count=sizes[bit]; break; }
    }
    if (!count || t->elements!=count || (*seen&(1u<<bit))) return -1;
    *seen|=(uint16_t)(1u<<bit);
    uint64_t bytes=lmb_size_mul(count,4);
    v->weights=lmb_size_add(v->weights,bytes); if (bytes>v->largest) v->largest=bytes;
    return v->weights==UINT64_MAX ? -1 : 1;
}
static int lmb_q38_vision_finish(LmbQ38Vision *v,LmbModelShape *m) {
    if (!v->weights) return 0; /* native Colibri permits text-only exports */
    if (v->globals!=511) return -1;
    for (unsigned i=0;i<v->depth;i++) if (v->blocks[i]!=4095) return -1;
    uint64_t N=LMB_IMAGE_TOKENS_MAX*v->merge*v->merge,H=v->h,W=H*v->merge*v->merge;
    uint64_t patches=N*3*v->temporal*v->patch*v->patch;
    uint64_t forward=N*(6*H+v->inter+2*H/v->heads+1)+LMB_IMAGE_TOKENS_MAX*(2*W+v->out);
    uint64_t retained=(uint64_t)LMB_MEDIA_IMAGES*LMB_IMAGE_TOKENS_MAX*v->out;
    uint64_t work=lmb_size_add(LMB_IMAGE_WORK_BYTES,lmb_size_mul(patches+forward+retained,4));
    work=lmb_size_add(work,262144u*4u); /* token-to-image row map */
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,v->weights);
    m->edge_scratch_fixed_bytes=lmb_size_add(m->edge_scratch_fixed_bytes,lmb_size_add(work,v->largest));
    return m->edge_resident_bytes==UINT64_MAX || m->edge_scratch_fixed_bytes==UINT64_MAX ? -1 : 0;
}
#endif
