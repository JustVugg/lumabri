/* Deliberately small HTTP/1.1 subset: one request, Content-Length framing,
 * bounded headers/body, then Connection: close. No chunking, upgrades,
 * ambiguous duplicate framing or cross-origin browser authority. */
#ifndef LMB_API_HTTP_H
#define LMB_API_HTTP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define LMB_API_HEADER_MAX 8192u
#define LMB_API_BODY_MAX (1u << 20)
typedef struct {
    char method[8], path[128], host[128], authorization[128], origin[160];
    uint32_t length;
    int has_length, json, has_origin;
} LmbApiHttp;
static inline int lmb_api_ascii_equal(const char *a, const char *b) {
    for (; *a && *b; a++,b++) {
        unsigned x=(unsigned char)*a, y=(unsigned char)*b;
        if (x>='A' && x<='Z') x+=32;
        if (y>='A' && y<='Z') y+=32;
        if (x!=y) return 0;
    }
    return *a==*b;
}
static inline int lmb_api_copy(char *dst, size_t cap, const char *value) {
    size_t n=strlen(value); if (n>=cap) return -1;
    memcpy(dst,value,n+1); return 0;
}
static inline int lmb_api_http_parse(LmbApiHttp *r, const void *bytes, size_t length) {
    if (!r || !bytes || length<4 || length>LMB_API_HEADER_MAX ||
        memcmp((const char *)bytes+length-4,"\r\n\r\n",4) || memchr(bytes,0,length)) return -1;
    char header[LMB_API_HEADER_MAX+1]; memcpy(header,bytes,length); header[length]=0;
    memset(r,0,sizeof *r);
    for (size_t i=0; i<length; i++) {
        unsigned char c=(unsigned char)header[i];
        if ((c<32 && c!='\r' && c!='\n' && c!='\t') || c==127) return -1;
        if (c=='\n' && (!i || header[i-1]!='\r')) return -1;
        if (c=='\r' && (i+1==length || header[i+1]!='\n')) return -1;
    }
    char *line=header, *end=strstr(line,"\r\n");
    if (!end) return -1;
    *end=0;
    char *path=strchr(line,' '), *version=path ? strchr(path+1,' ') : NULL;
    if (!path || !version || strcmp(version+1,"HTTP/1.1")) return -1;
    *path++=0; *version=0;
    if ((strcmp(line,"GET") && strcmp(line,"POST") && strcmp(line,"DELETE")) ||
        lmb_api_copy(r->method,sizeof r->method,line) || lmb_api_copy(r->path,sizeof r->path,path) || *path!='/') return -1;
    for (const char *p=path; *p; p++) {
        unsigned c=(unsigned char)*p;
        if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='/' || c=='-' || c=='_' || c=='.')) return -1;
    }
    unsigned seen=0, lines=0;
    line=end+2;
    while (*line) {
        end=strstr(line,"\r\n");
        if (!end || ++lines>64) return -1;
        if (end==line) return end+2==header+length && (seen&1) ? 0 : -1;
        *end=0;
        char *colon=strchr(line,':'); if (!colon || colon==line) return -1;
        *colon=0;
        for (const char *p=line; *p; p++) {
            unsigned c=(unsigned char)*p;
            if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-')) return -1;
        }
        char *v=colon+1;
        while (*v==' ' || *v=='\t') v++;
        char *last=end; while (last>v && (last[-1]==' ' || last[-1]=='\t')) *--last=0;
        if (lmb_api_ascii_equal(line,"Transfer-Encoding") || lmb_api_ascii_equal(line,"Expect") ||
            lmb_api_ascii_equal(line,"Upgrade")) return -1;
        unsigned bit=0;
        if (lmb_api_ascii_equal(line,"Host")) {
            bit=1; if (!*v || lmb_api_copy(r->host,sizeof r->host,v)) return -1;
        } else if (lmb_api_ascii_equal(line,"Authorization")) {
            bit=2; if (lmb_api_copy(r->authorization,sizeof r->authorization,v)) return -1;
        } else if (lmb_api_ascii_equal(line,"Content-Length")) {
            bit=4; if (!*v) return -1;
            uint32_t n=0;
            for (const char *p=v; *p; p++) {
                unsigned c=(unsigned char)*p;
                if (c<'0' || c>'9' || n>(LMB_API_BODY_MAX-(c-'0'))/10) return -1;
                n=n*10+c-'0';
            }
            r->has_length=1; r->length=n;
        } else if (lmb_api_ascii_equal(line,"Content-Type")) {
            bit=8;
            r->json=lmb_api_ascii_equal(v,"application/json") ||
                    lmb_api_ascii_equal(v,"application/json; charset=utf-8");
        } else if (lmb_api_ascii_equal(line,"Origin")) {
            bit=16; if (!*v || lmb_api_copy(r->origin,sizeof r->origin,v)) return -1;
            r->has_origin=1;
        }
        if (bit && (seen&bit)) return -1;
        seen|=bit; line=end+2;
    }
    return -1;
}
static inline int lmb_api_http_origin(const LmbApiHttp *r, unsigned port) {
    char numeric[64], local[64], origin[160];
    snprintf(numeric,sizeof numeric,"127.0.0.1:%u",port);
    snprintf(local,sizeof local,"localhost:%u",port);
    if (strcmp(r->host,numeric) && strcmp(r->host,local)) return -1;
    snprintf(origin,sizeof origin,"http://%s",r->host);
    return r->has_origin && strcmp(r->origin,origin) ? -1 : 0;
}
#endif
