#ifndef LMB_GGUF_INVENTORY_H
#define LMB_GGUF_INVENTORY_H
#include "olmoe_gguf.h"

typedef struct {const LmbGguf *g;uint8_t *seen;uint64_t largest;} LmbGgufInventory;
static int lmb_gguf_inventory_item(const LmbGgufValue *v,uint32_t i,uint64_t *out) {
    if(!v || v->type!=9 || i>=v->count)return -1;
    unsigned width=lmb_gguf_scalar_bytes(v->element_type);
    if(!width || (uint64_t)(i+1)*width>v->bytes)return -1;
    LmbGgufValue item={.type=v->element_type,.bytes=width,.data=v->data+(size_t)i*width};
    return lmb_gguf_integer(&item,out);
}
static int lmb_gguf_inventory_weight(LmbGgufInventory *v,const char *name,uint32_t rank,
                          uint64_t x,uint64_t y,uint64_t z,int matrix,uint64_t *cost) {
    const LmbGgufTensor *t=lmb_gguf_tensor(v->g,name);
    if(!t || t->rank!=rank || t->shape[0]!=x || (rank>1 && t->shape[1]!=y) || (rank>2 && t->shape[2]!=z) ||
       !LMB_GGUF_TYPES[t->type].decodable || v->seen[t-v->g->tensors])return -1;
    v->seen[t-v->g->tensors]=1;
    uint64_t elements=lmb_size_mul(x,lmb_size_mul(y,z));
    uint64_t bytes=lmb_size_mul(elements,4);
    if(matrix) {
        LmbGgufTensor flat=*t;flat.rank=2;flat.shape[1]=lmb_size_mul(y,z);
        bytes=lmb_gguf_held_bytes(&flat,1,1);
        if(t->type==0 || t->type==1 || t->type==30) {
            /* Also cover float-source quantization choices and row scales. */
            uint64_t floating=lmb_size_mul(elements,5);if(floating>bytes)bytes=floating;
        }
    }
    if(bytes==UINT64_MAX)return -1;
    if(bytes>v->largest)v->largest=bytes;
    *cost=lmb_size_add(*cost,bytes);return *cost==UINT64_MAX?-1:0;
}
#endif
