#ifndef LUMABRI_VISION_H
#define LUMABRI_VISION_H
#include "lumabri_image.h"
typedef struct {
    void *owner;
    LmbImageRules rules;
    uint32_t hidden, copies, image_token;
    int (*forward)(void *,const float *,uint32_t,uint32_t,float *);
} LmbVisionBinding;
/* One Edge tokenizer/vision tower per engine process, as required by Colibri.
 * The adapter registers concrete model-owned callbacks, never an ABI cast. */
int lmb_vision_register(const LmbVisionBinding *binding);
void lmb_vision_unregister(void *owner);
int lmb_vision_available(void);
int lmb_vision_prepare(const void *frame,size_t bytes,char **prompt,size_t *prompt_bytes);
int lmb_vision_tokens(const int32_t *ids,size_t count);
int lmb_vision_overlay(size_t offset,uint32_t rows,void *output,uint32_t width);
void lmb_vision_clear(void);
#endif
