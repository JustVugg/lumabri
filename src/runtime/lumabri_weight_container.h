#ifndef LMB_WEIGHT_CONTAINER_H
#define LMB_WEIGHT_CONTAINER_H
#include <string.h>
/* Container classification only; execution still requires tensor validation. */
enum { LMB_WEIGHT_NONE=0, LMB_WEIGHT_SAFETENSORS=1, LMB_WEIGHT_GGUF=2 };
static inline int lmb_weight_container(const char *path) {
    const char *ext=path?strrchr(path,'.'):NULL;
    return ext && !strcmp(ext,".safetensors") ? LMB_WEIGHT_SAFETENSORS :
           ext && !strcmp(ext,".gguf") ? LMB_WEIGHT_GGUF : LMB_WEIGHT_NONE;
}
#endif
