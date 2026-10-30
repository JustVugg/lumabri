#ifndef LUMABRI_MEDIA_H
#define LUMABRI_MEDIA_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LMB_MEDIA_IMAGES 4u
#define LMB_MEDIA_MARKER 48u
#define LMB_MEDIA_TEXT_MAX 65536u
#define LMB_MEDIA_BYTES_MAX (1024u * 1024u)
/* Explicit versioned frame. Text is followed by ordered records containing
 * replacement byte offset/length, format, encoded size and PNG/JPEG bytes.
 * No paths, remote URLs, native structs or floats cross this boundary. */
static inline uint32_t lmb_media_u32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static inline void lmb_media_put(unsigned char *p,uint32_t n) {
    p[0]=(unsigned char)(n>>24); p[1]=(unsigned char)(n>>16);
    p[2]=(unsigned char)(n>>8); p[3]=(unsigned char)n;
}
typedef struct {
    uint32_t offset, span, format, bytes;
    const unsigned char *data;
} LmbMediaImage;
typedef struct {
    const char *text; uint32_t text_bytes,count;
    LmbMediaImage image[LMB_MEDIA_IMAGES];
} LmbMedia;
static inline int lmb_media_parse(const void *data,size_t bytes,LmbMedia *out) {
    const unsigned char *p=data;
    memset(out,0,sizeof *out);
    if (!p || bytes<16 || bytes>LMB_MEDIA_BYTES_MAX || memcmp(p,"LMBIMG01",8)) return -1;
    out->text_bytes=lmb_media_u32(p+8); out->count=lmb_media_u32(p+12);
    if (!out->text_bytes || out->text_bytes>LMB_MEDIA_TEXT_MAX || !out->count || out->count>LMB_MEDIA_IMAGES ||
        out->text_bytes>bytes-16) return -1;
    out->text=(const char *)p+16;
    if (memchr(out->text,0,out->text_bytes)) return -1;
    size_t at=16+out->text_bytes; uint32_t end=0;
    for (unsigned i=0;i<out->count;i++) {
        if (bytes-at<16) return -1;
        LmbMediaImage *im=&out->image[i];
        im->offset=lmb_media_u32(p+at); im->span=lmb_media_u32(p+at+4);
        im->format=lmb_media_u32(p+at+8); im->bytes=lmb_media_u32(p+at+12); at+=16;
        if (im->offset<end || im->offset>out->text_bytes || !im->span || im->span>out->text_bytes-im->offset ||
            (im->format!=1 && im->format!=2) || !im->bytes || im->bytes>bytes-at) return -1;
        im->data=p+at; at+=im->bytes; end=im->offset+im->span;
    }
    return at==bytes ? 0 : -1;
}
#endif
