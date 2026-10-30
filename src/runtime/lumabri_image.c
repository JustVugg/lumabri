#include "lumabri_image.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef union { max_align_t alignment; size_t size; } ImageAllocation;
static _Thread_local size_t image_allocated;
static void *image_alloc(size_t size) {
    if (!size || size>LMB_IMAGE_WORK_BYTES-image_allocated) return NULL;
    ImageAllocation *p=malloc(sizeof *p+size); if (!p) return NULL;
    p->size=size; image_allocated+=size; return p+1;
}
static void image_free(void *data) {
    if (!data) return;
    ImageAllocation *p=(ImageAllocation *)data-1;
    image_allocated-=p->size; free(p);
}
static void *image_resize(void *data,size_t old_size,size_t size) {
    (void)old_size;
    if (!data) return image_alloc(size);
    ImageAllocation *p=(ImageAllocation *)data-1;
    if (!size || size>LMB_IMAGE_WORK_BYTES-(image_allocated-p->size)) return NULL;
    size_t old=p->size;
    ImageAllocation *next=realloc(p,sizeof *p+size); if (!next) return NULL;
    next->size=size; image_allocated=image_allocated-old+size; return next+1;
}
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(n) image_alloc(n)
#define STBI_REALLOC_SIZED(p,old,n) image_resize(p,old,n)
#define STBI_FREE(p) image_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "../vendor/stb_image.h"

static int image_rules(const LmbImageRules *r) {
    if (!r || !r->patch || r->patch>32 || !r->merge || r->merge>4 || !r->temporal || r->temporal>4 ||
        !r->min_pixels || r->max_pixels<r->min_pixels || r->max_pixels>16777216 ||
        !r->max_tokens || r->max_tokens>LMB_IMAGE_TOKENS_MAX) return -1;
    for (unsigned c=0;c<3;c++) if (!isfinite(r->mean[c]) || fabsf(r->mean[c])>1 ||
        !isfinite(r->std[c]) || r->std[c]<1e-6f || r->std[c]>1) return -1;
    return 0;
}
static uint32_t image_round_even(double n) {
    uint32_t floor_n=(uint32_t)n; double part=n-floor_n;
    return floor_n+(part>.5 || (part==.5 && (floor_n&1)));
}
int lmb_image_grid(uint32_t height,uint32_t width,const LmbImageRules *r,uint32_t *out_h,uint32_t *out_w) {
    if (!out_h || !out_w || image_rules(r) || !height || !width || height>8192 || width>8192 ||
        (uint64_t)height*width>LMB_IMAGE_PIXELS_MAX || height>(uint64_t)width*200 || width>(uint64_t)height*200) return -1;
    uint32_t factor=r->patch*r->merge;
    uint64_t ceiling=(uint64_t)r->max_tokens*factor*factor;
    uint32_t max=r->max_pixels<ceiling ? r->max_pixels : (uint32_t)ceiling;
    uint32_t min=r->min_pixels<max ? r->min_pixels : max;
    uint32_t h=image_round_even((double)height/factor)*factor,w=image_round_even((double)width/factor)*factor;
    if (!h) h=factor;
    if (!w) w=factor;
    if ((uint64_t)h*w>max) {
        double ratio=sqrt((double)height*width/max);
        h=(uint32_t)floor(height/ratio/factor)*factor; w=(uint32_t)floor(width/ratio/factor)*factor;
        if (!h) h=factor;
        if (!w) w=factor;
    } else if ((uint64_t)h*w<min) {
        double ratio=sqrt((double)min/((double)height*width));
        h=(uint32_t)ceil(height*ratio/factor)*factor; w=(uint32_t)ceil(width*ratio/factor)*factor;
    }
    /* Extremely narrow aspect ratios can round up beyond the token ceiling.
     * Refuse rather than silently allocate an unbudgeted attention matrix. */
    if ((uint64_t)h*w>ceiling || h>8192 || w>8192) return -1;
    *out_h=h; *out_w=w; return 0;
}
static double image_cubic(double x) {
    x=fabs(x);
    if (x<1) return ((1.5*x-2.5)*x)*x+1;
    if (x<2) return ((-.5*x+2.5)*x-4)*x+2;
    return 0;
}
static unsigned char image_u8(double x) {
    return x<=0 ? 0 : x>=255 ? 255 : (unsigned char)floor(x+.5);
}
/* Separable antialiased bicubic, pixel-centre coordinates. Round each pass to
 * RGB8, matching the model's RGB image preprocessing rather than resampling
 * already-normalized floats. Boundary taps are renormalized, never wrapped. */
static unsigned char *image_resample(const unsigned char *src,uint32_t sw,uint32_t sh,uint32_t dw,uint32_t dh) {
    size_t temporary=(size_t)dw*sh*3, output=(size_t)dw*dh*3;
    unsigned char *tmp=image_alloc(temporary),*out=image_alloc(output);
    if (!tmp || !out) { image_free(tmp); image_free(out); return NULL; }
    double sx=(double)sw/dw,sy=(double)sh/dh,kx=sx>1 ? sx : 1,ky=sy>1 ? sy : 1;
    for (uint32_t x=0;x<dw;x++) {
        double centre=(x+.5)*sx; int low=(int)floor(centre-2*kx+.5),high=(int)floor(centre+2*kx+.5);
        if (low<0) low=0;
        if (high>(int)sw) high=(int)sw;
        for (uint32_t y=0;y<sh;y++) {
            double total=0,value[3]={0};
            for (int i=low;i<high;i++) {
                double weight=image_cubic((i+.5-centre)/kx); total+=weight;
                for (unsigned c=0;c<3;c++) value[c]+=weight*src[((size_t)y*sw+(unsigned)i)*3+c];
            }
            if (!total) { image_free(tmp); image_free(out); return NULL; }
            for (unsigned c=0;c<3;c++) tmp[((size_t)y*dw+x)*3+c]=image_u8(value[c]/total);
        }
    }
    for (uint32_t y=0;y<dh;y++) {
        double centre=(y+.5)*sy; int low=(int)floor(centre-2*ky+.5),high=(int)floor(centre+2*ky+.5);
        if (low<0) low=0;
        if (high>(int)sh) high=(int)sh;
        for (uint32_t x=0;x<dw;x++) {
            double total=0,value[3]={0};
            for (int i=low;i<high;i++) {
                double weight=image_cubic((i+.5-centre)/ky); total+=weight;
                for (unsigned c=0;c<3;c++) value[c]+=weight*tmp[((size_t)(unsigned)i*dw+x)*3+c];
            }
            if (!total) { image_free(tmp); image_free(out); return NULL; }
            for (unsigned c=0;c<3;c++) out[((size_t)y*dw+x)*3+c]=image_u8(value[c]/total);
        }
    }
    image_free(tmp); return out;
}
int lmb_image_prepare(const void *encoded,size_t bytes,int format,const LmbImageRules *rules,LmbImagePatches *patches) {
    if (!patches) return -1;
    memset(patches,0,sizeof *patches);
    if (!encoded || bytes<8 || bytes>LMB_IMAGE_ENCODED_MAX || image_rules(rules)) return -1;
    const unsigned char *data=encoded;
    if (format==LMB_IMAGE_PNG ? memcmp(data,"\x89PNG\r\n\x1a\n",8) :
        format==LMB_IMAGE_JPEG ? data[0]!=0xff || data[1]!=0xd8 || data[2]!=0xff : 1) return -1;
    int width=0,height=0,channels=0; uint32_t target_h,target_w;
    if (!stbi_info_from_memory(data,(int)bytes,&width,&height,&channels) || width<=0 || height<=0 ||
        lmb_image_grid((uint32_t)height,(uint32_t)width,rules,&target_h,&target_w)) return -1;
    unsigned char *rgb=stbi_load_from_memory(data,(int)bytes,&width,&height,&channels,3);
    if (!rgb) return -1;
    unsigned char *canvas=rgb;
    if ((uint32_t)width!=target_w || (uint32_t)height!=target_h) {
        canvas=image_resample(rgb,(uint32_t)width,(uint32_t)height,target_w,target_h);
        image_free(rgb); if (!canvas) return -1;
    }
    uint32_t gh=target_h/rules->patch,gw=target_w/rules->patch;
    size_t count=(size_t)target_h*target_w*3*rules->temporal;
    if (count>(LMB_IMAGE_WORK_BYTES-image_allocated)/sizeof(float)) { image_free(canvas); return -1; }
    float *values=malloc(count*sizeof *values);
    if (!values) { image_free(canvas); return -1; }
    size_t at=0;
    for (uint32_t bh=0;bh<gh;bh+=rules->merge) for (uint32_t bw=0;bw<gw;bw+=rules->merge)
        for (uint32_t mh=0;mh<rules->merge;mh++) for (uint32_t mw=0;mw<rules->merge;mw++)
            for (unsigned c=0;c<3;c++) for (uint32_t t=0;t<rules->temporal;t++)
                for (uint32_t y=0;y<rules->patch;y++) for (uint32_t x=0;x<rules->patch;x++) {
                    uint32_t iy=(bh+mh)*rules->patch+y,ix=(bw+mw)*rules->patch+x;
                    values[at++]=((float)canvas[((size_t)iy*target_w+ix)*3+c]/255.f-rules->mean[c])/rules->std[c];
                }
    image_free(canvas);
    if (at!=count) { free(values); return -1; }
    patches->values=values; patches->count=count; patches->grid_h=gh; patches->grid_w=gw;
    patches->tokens=gh*gw/(rules->merge*rules->merge); return 0;
}
void lmb_image_patches_free(LmbImagePatches *patches) {
    if (!patches) return;
    free(patches->values); memset(patches,0,sizeof *patches);
}
