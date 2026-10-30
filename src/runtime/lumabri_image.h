#ifndef LUMABRI_IMAGE_H
#define LUMABRI_IMAGE_H
#include <stddef.h>
#include <stdint.h>

/* Encoded files are request data, never paths. Limits also bound decoder
 * scratch and decompression expansion before a vision model is entered. */
#define LMB_IMAGE_ENCODED_MAX (768u * 1024u)
#define LMB_IMAGE_PIXELS_MAX (4u * 1024u * 1024u)
#define LMB_IMAGE_TOKENS_MAX 256u
#define LMB_IMAGE_WORK_BYTES (64u * 1024u * 1024u)
enum { LMB_IMAGE_PNG=1, LMB_IMAGE_JPEG=2 };
typedef struct {
    uint32_t patch, merge, temporal, min_pixels, max_pixels, max_tokens;
    float mean[3], std[3];
} LmbImageRules;
typedef struct {
    float *values;
    size_t count;
    uint32_t grid_h, grid_w, tokens;
} LmbImagePatches;
int lmb_image_grid(uint32_t height,uint32_t width,const LmbImageRules *rules,uint32_t *out_h,uint32_t *out_w);
int lmb_image_prepare(const void *encoded,size_t bytes,int format,const LmbImageRules *rules,LmbImagePatches *patches);
void lmb_image_patches_free(LmbImagePatches *patches);
#endif
