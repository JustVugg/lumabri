#include "src/runtime/lumabri_image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    LmbImageRules rules={.patch=16,.merge=2,.temporal=2,.min_pixels=65536,.max_pixels=16777216,.max_tokens=256,
        .mean={.5f,.5f,.5f},.std={.5f,.5f,.5f}};
    uint32_t h,w; LmbImagePatches result;
    assert(!lmb_image_grid(256,256,&rules,&h,&w) && h==256 && w==256);
    assert(lmb_image_grid(1,8192,&rules,&h,&w));
    assert(lmb_image_grid(8192,8192,&rules,&h,&w));
    assert(lmb_image_prepare("not a png",9,LMB_IMAGE_PNG,&rules,&result));
    rules.std[0]=0; assert(lmb_image_grid(256,256,&rules,&h,&w)); rules.std[0]=.5f;
    if (argc==1) { puts("IMAGE LIMITS PASS"); return 0; }
    assert(argc==4);
    FILE *file=fopen(argv[1],"rb"); assert(file); assert(!fseek(file,0,SEEK_END));
    long bytes=ftell(file); assert(bytes>0 && bytes<(long)LMB_IMAGE_ENCODED_MAX); rewind(file);
    unsigned char *encoded=malloc((size_t)bytes); assert(encoded);
    assert(fread(encoded,1,(size_t)bytes,file)==(size_t)bytes); assert(!fclose(file));
    int format=!strcmp(argv[2],"png") ? LMB_IMAGE_PNG : LMB_IMAGE_JPEG;
    if (!strcmp(argv[3],"--mutate")) {
        unsigned char *mutated=malloc((size_t)bytes); assert(mutated);
        uint32_t state=0x12345678u;
        for(unsigned i=0;i<128;i++) {
            memcpy(mutated,encoded,(size_t)bytes); state=state*1664525u+1013904223u;
            mutated[state%(uint32_t)bytes]^=(unsigned char)(1u<<(i%8));
            size_t length=i&1 ? (size_t)bytes : state%(uint32_t)bytes;
            if(!lmb_image_prepare(mutated,length,format,&rules,&result)) lmb_image_patches_free(&result);
        }
        free(mutated); free(encoded); puts("IMAGE MALFORMED INPUT PASS"); return 0;
    }
    assert(!lmb_image_prepare(encoded,(size_t)bytes,format,&rules,&result));
    assert(lmb_image_prepare(encoded,(size_t)bytes,format==LMB_IMAGE_PNG ? LMB_IMAGE_JPEG : LMB_IMAGE_PNG,&rules,&(LmbImagePatches){0}));
    file=fopen(argv[3],"wb"); assert(file);
    assert(fwrite(result.values,sizeof(float),result.count,file)==result.count); assert(!fclose(file));
    printf("{\"grid_h\":%u,\"grid_w\":%u,\"tokens\":%u,\"values\":%zu}\n",result.grid_h,result.grid_w,result.tokens,result.count);
    lmb_image_patches_free(&result); free(encoded); return 0;
}
