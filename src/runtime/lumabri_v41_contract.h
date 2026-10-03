#ifndef LMB_V41_CONTRACT_H
#define LMB_V41_CONTRACT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#define LMB_V41_SCHEMA "lmb-v41-text-delta1-v1"
#define LMB_V41_NUMERIC "v41/f32/single-row-cpu-v1"

/* Negotiated schema, never a model-name substring. Header values are bounded
 * exact integers; copying opaque feedback is the coordinator's only model-
 * specific duty. The adapter validates its full contents before mutation. */
static inline int lmb_v41_feedback_slice(const void *row,size_t bytes,size_t *offset,size_t *length) {
    if(!row || bytes<8 || bytes%4 || bytes/4>1048576)return -1;
    float h[2];memcpy(h,row,sizeof h);
    if(!isfinite(h[0])||!isfinite(h[1])||h[0]<2||h[1]<1||
       h[0]>1048576||h[1]>1048576||truncf(h[0])!=h[0]||truncf(h[1])!=h[1])return -1;
    size_t a=(size_t)h[0]*4,b=(size_t)h[1]*4;
    if(a>bytes||b!=bytes-a)return -1;
    *offset=a;*length=b;return 0;
}
#endif
