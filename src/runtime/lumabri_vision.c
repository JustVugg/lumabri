#include "lumabri_vision.h"
#include "lumabri_media.h"
#include <stdlib.h>
#include <math.h>
static LmbVisionBinding binding;
static float *image_rows;
static uint32_t row_count;
static int32_t *image_map;
static size_t map_count;
void lmb_vision_clear(void) {
    free(image_rows); free(image_map); image_rows=NULL; image_map=NULL; row_count=0; map_count=0;
}
int lmb_vision_register(const LmbVisionBinding *b) {
    if (!b || !b->owner || !b->forward || binding.owner || !b->hidden || b->hidden>65536 ||
        !b->copies || b->copies>16) return -1;
    binding=*b; return 0;
}
void lmb_vision_unregister(void *owner) {
    if (binding.owner==owner) { lmb_vision_clear(); memset(&binding,0,sizeof binding); }
}
int lmb_vision_available(void) { return binding.owner!=NULL; }
int lmb_vision_prepare(const void *frame,size_t bytes,char **prompt,size_t *prompt_bytes) {
    LmbMedia media; LmbImagePatches patches={0}; char *text=NULL;
    lmb_vision_clear(); *prompt=NULL; *prompt_bytes=0;
    if (!binding.owner || lmb_media_parse(frame,bytes,&media)) return -1;
    size_t max_tokens=(size_t)LMB_MEDIA_IMAGES*binding.rules.max_tokens;
    image_rows=calloc(max_tokens*binding.hidden,sizeof(float));
    size_t capacity=media.text_bytes+max_tokens*strlen("<|image_pad|>")+media.count*64u+1;
    text=malloc(capacity);
    if (!image_rows || !text) goto fail;
    size_t at=0,from=0;
    for (unsigned i=0;i<media.count;i++) {
        const LmbMediaImage *im=&media.image[i];
        if (lmb_image_prepare(im->data,im->bytes,(int)im->format,&binding.rules,&patches)) goto fail;
        if (binding.forward(binding.owner,patches.values,patches.grid_h,patches.grid_w,
                             image_rows+(size_t)row_count*binding.hidden)!=(int)patches.tokens) goto fail;
        size_t chunk=im->offset-from; memcpy(text+at,media.text+from,chunk); at+=chunk;
        const char *start="<|vision_start|>",*pad="<|image_pad|>",*end="<|vision_end|>";
        memcpy(text+at,start,strlen(start)); at+=strlen(start);
        for (unsigned r=0;r<patches.tokens;r++) { memcpy(text+at,pad,strlen(pad)); at+=strlen(pad); }
        memcpy(text+at,end,strlen(end)); at+=strlen(end);
        row_count+=patches.tokens; from=im->offset+im->span;
        lmb_image_patches_free(&patches);
    }
    for (size_t i=0;i<(size_t)row_count*binding.hidden;i++) if (!isfinite(image_rows[i])) goto fail;
    memcpy(text+at,media.text+from,media.text_bytes-from); at+=media.text_bytes-from; text[at]=0;
    *prompt=text; *prompt_bytes=at; return 0;
fail:
    lmb_image_patches_free(&patches); free(text); lmb_vision_clear(); return -1;
}
int lmb_vision_tokens(const int32_t *ids,size_t count) {
    if (!image_rows) return 0;
    if (!count || count>262144) return -1;
    free(image_map); image_map=malloc(count*sizeof *image_map); if (!image_map) return -1;
    unsigned n=0;
    for (size_t i=0;i<count;i++) image_map[i]=ids[i]==(int32_t)binding.image_token ? (int32_t)n++ : -1;
    if (n!=row_count) { free(image_map); image_map=NULL; return -1; }
    map_count=count; return 0;
}
int lmb_vision_overlay(size_t offset,uint32_t rows,void *output,uint32_t width) {
    if (!image_map) return 0;
    if (width!=binding.hidden*binding.copies || !output) return -1;
    for (size_t i=0;i<rows;i++) {
        if (offset+i>=map_count || image_map[offset+i]<0) continue;
        float *dst=(float *)output+i*width;
        const float *src=image_rows+(size_t)image_map[offset+i]*binding.hidden;
        for (unsigned c=0;c<binding.copies;c++) memcpy(dst+c*binding.hidden,src,binding.hidden*sizeof(float));
    }
    return 0;
}
