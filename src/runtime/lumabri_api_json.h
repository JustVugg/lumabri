/* Bounded JSON syntax/UTF-8 reader for the managed API. No file/network I/O.
 * Tokens reference the caller-owned input, which must outlive the document.
 * Decoding rejects NUL (our public text interface uses C strings), malformed
 * UTF-8 and unpaired surrogates; it never repairs a request silently. */
#ifndef LMB_API_JSON_H
#define LMB_API_JSON_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { LMB_JSON_OBJECT=1, LMB_JSON_ARRAY, LMB_JSON_STRING, LMB_JSON_NUMBER,
       LMB_JSON_TRUE, LMB_JSON_FALSE, LMB_JSON_NULL };
typedef struct { size_t begin, end; unsigned next, children, kind; } LmbJsonToken;
typedef struct {
    const unsigned char *text;
    size_t size, at;
    LmbJsonToken *tokens;
    unsigned count, capacity;
} LmbJson;

/* Return one scalar's byte length, or zero for invalid/incomplete UTF-8. */
static inline unsigned lmb_json_utf8(const unsigned char *p, size_t n) {
    if (!n || !*p) return 0;
    if (*p < 0x80) return 1;
    unsigned k = *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 :
                 *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
    if (!k || n < k) return 0;
    for (unsigned i=1; i<k; i++) if ((p[i]&0xc0) != 0x80) return 0;
    if ((*p==0xe0 && p[1]<0xa0) || (*p==0xed && p[1]>=0xa0) ||
        (*p==0xf0 && p[1]<0x90) || (*p==0xf4 && p[1]>=0x90)) return 0;
    return k;
}
static inline int lmb_json_hex4(const unsigned char *p, uint32_t *value) {
    uint32_t v=0;
    for (unsigned i=0; i<4; i++) {
        unsigned c=p[i], d=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 :
                          c>='A' && c<='F' ? c-'A'+10 : 16;
        if (d==16) return -1;
        v=v*16+d;
    }
    *value=v; return 0;
}
/* Decode one string body. NULL output validates and counts without allocating. */
static inline int lmb_json_decode(const unsigned char *p, size_t n, char *out, size_t cap, size_t *written) {
    size_t at=0, used=0;
    while (at<n) {
        unsigned char bytes[4]; unsigned length=1;
        unsigned c=p[at++];
        if (c<0x20 || c=='"') return -1;
        if (c=='\\') {
            if (at==n) return -1;
            c=p[at++];
            if (c=='"' || c=='\\' || c=='/') bytes[0]=(unsigned char)c;
            else if (c=='b') bytes[0]='\b';
            else if (c=='f') bytes[0]='\f';
            else if (c=='n') bytes[0]='\n';
            else if (c=='r') bytes[0]='\r';
            else if (c=='t') bytes[0]='\t';
            else if (c=='u') {
                uint32_t scalar;
                if (n-at<4 || lmb_json_hex4(p+at,&scalar)) return -1;
                at+=4;
                if (scalar>=0xd800 && scalar<=0xdbff) {
                    uint32_t low;
                    if (n-at<6 || p[at]!='\\' || p[at+1]!='u' || lmb_json_hex4(p+at+2,&low) ||
                        low<0xdc00 || low>0xdfff) return -1;
                    scalar=0x10000+((scalar-0xd800)<<10)+(low-0xdc00); at+=6;
                } else if (scalar>=0xdc00 && scalar<=0xdfff) return -1;
                if (!scalar) return -1;
                if (scalar<0x80) bytes[0]=(unsigned char)scalar;
                else if (scalar<0x800) {
                    length=2; bytes[0]=(unsigned char)(0xc0|(scalar>>6)); bytes[1]=(unsigned char)(0x80|(scalar&63));
                } else if (scalar<0x10000) {
                    length=3; bytes[0]=(unsigned char)(0xe0|(scalar>>12));
                    bytes[1]=(unsigned char)(0x80|((scalar>>6)&63)); bytes[2]=(unsigned char)(0x80|(scalar&63));
                } else {
                    length=4; bytes[0]=(unsigned char)(0xf0|(scalar>>18)); bytes[1]=(unsigned char)(0x80|((scalar>>12)&63));
                    bytes[2]=(unsigned char)(0x80|((scalar>>6)&63)); bytes[3]=(unsigned char)(0x80|(scalar&63));
                }
            } else return -1;
        } else if (c>=0x80) {
            length=lmb_json_utf8(p+at-1,n-at+1);
            if (!length) return -1;
            memcpy(bytes,p+at-1,length); at+=length-1;
        } else bytes[0]=(unsigned char)c;
        if (length>SIZE_MAX-used) return -1;
        if (out) {
            if (used>=cap || length>=cap-used) return -1;
            memcpy(out+used,bytes,length);
        }
        used+=length;
    }
    if (out) { if (used>=cap) return -1; out[used]=0; }
    if (written) *written=used;
    return 0;
}
static inline void lmb_json_space(LmbJson *j) {
    while (j->at<j->size && (j->text[j->at]==' ' || j->text[j->at]=='\n' ||
           j->text[j->at]=='\r' || j->text[j->at]=='\t')) j->at++;
}
static inline int lmb_json_value(LmbJson *j, unsigned depth) {
    lmb_json_space(j);
    if (depth>16 || j->at>=j->size || j->count>=j->capacity) return -1;
    unsigned index=j->count++, c=j->text[j->at++];
    LmbJsonToken *t=&j->tokens[index]; memset(t,0,sizeof *t); t->begin=j->at-1;
    if (c=='"') {
        t->kind=LMB_JSON_STRING; t->begin=j->at;
        while (j->at<j->size && j->text[j->at]!='"') {
            if (j->text[j->at++]=='\\') { if (j->at==j->size) return -1; j->at++; }
        }
        if (j->at==j->size || lmb_json_decode(j->text+t->begin,j->at-t->begin,NULL,0,NULL)) return -1;
        t->end=j->at++;
    } else if (c=='{' || c=='[') {
        unsigned end=c=='{' ? '}' : ']'; t->kind=c=='{' ? LMB_JSON_OBJECT : LMB_JSON_ARRAY;
        lmb_json_space(j);
        if (j->at<j->size && j->text[j->at]==end) j->at++;
        else for (;;) {
            if (c=='{') {
                lmb_json_space(j);
                if (j->at==j->size || j->text[j->at]!='"' || lmb_json_value(j,depth+1)) return -1;
                t->children++; lmb_json_space(j);
                if (j->at==j->size || j->text[j->at++]!=':') return -1;
            }
            if (lmb_json_value(j,depth+1)) return -1;
            t->children++; lmb_json_space(j);
            if (j->at==j->size) return -1;
            unsigned sep=j->text[j->at++];
            if (sep==end) break;
            if (sep!=',') return -1;
        }
        t->end=j->at;
    } else if (c=='t' || c=='f' || c=='n') {
        const char *literal=c=='t' ? "true" : c=='f' ? "false" : "null";
        size_t n=strlen(literal);
        if (j->size-t->begin<n || memcmp(j->text+t->begin,literal,n)) return -1;
        j->at=t->begin+n; t->end=j->at;
        t->kind=c=='t' ? LMB_JSON_TRUE : c=='f' ? LMB_JSON_FALSE : LMB_JSON_NULL;
    } else {
        t->kind=LMB_JSON_NUMBER;
        size_t p=t->begin;
        if (j->text[p]=='-') p++;
        if (p==j->size) return -1;
        if (j->text[p]=='0') p++;
        else {
            if (j->text[p]<'1' || j->text[p]>'9') return -1;
            do { p++; } while (p<j->size && j->text[p]>='0' && j->text[p]<='9');
        }
        if (p<j->size && j->text[p]=='.') {
            size_t start=++p;
            while (p<j->size && j->text[p]>='0' && j->text[p]<='9') p++;
            if (p==start) return -1;
        }
        if (p<j->size && (j->text[p]=='e' || j->text[p]=='E')) {
            p++; if (p<j->size && (j->text[p]=='+' || j->text[p]=='-')) p++;
            size_t start=p;
            while (p<j->size && j->text[p]>='0' && j->text[p]<='9') p++;
            if (p==start) return -1;
        }
        j->at=t->end=p;
    }
    t->next=j->count; return 0;
}
static inline int lmb_json_parse(LmbJson *j, const void *text, size_t n, LmbJsonToken *tokens, unsigned cap) {
    if (!j || !text || !tokens || !cap) return -1;
    *j=(LmbJson){.text=text,.size=n,.tokens=tokens,.capacity=cap};
    if (lmb_json_value(j,0)) return -1;
    lmb_json_space(j); return j->at==n ? 0 : -1;
}
static inline int lmb_api_json_string(const LmbJson *j, unsigned index, char *out, size_t cap) {
    if (index>=j->count || j->tokens[index].kind!=LMB_JSON_STRING) return -1;
    const LmbJsonToken *t=&j->tokens[index];
    return lmb_json_decode(j->text+t->begin,t->end-t->begin,out,cap,NULL);
}
static inline int lmb_json_uint(const LmbJson *j, unsigned index, uint32_t *value) {
    if (!value || index>=j->count || j->tokens[index].kind!=LMB_JSON_NUMBER) return -1;
    const LmbJsonToken *t=&j->tokens[index]; uint32_t n=0;
    for (size_t p=t->begin; p<t->end; p++) {
        unsigned c=j->text[p];
        if (c<'0' || c>'9' || n>(UINT32_MAX-(c-'0'))/10) return -1;
        n=n*10+c-'0';
    }
    *value=n; return 0;
}
/* API object keys are small; reject oversized keys, unknown fields and
 * duplicate decoded names at the schema boundary, including escaped aliases.
 * Output slots correspond to allowed[] and remain -1 for missing fields. */
static inline int lmb_json_fields(const LmbJson *j, unsigned index, const char *const *allowed, unsigned count, int *fields) {
    if (index>=j->count || j->tokens[index].kind!=LMB_JSON_OBJECT) return -1;
    for (unsigned i=0; i<count; i++) fields[i]=-1;
    for (unsigned p=index+1; p<j->tokens[index].next;) {
        char key[64]; if (lmb_api_json_string(j,p,key,sizeof key)) return -1;
        unsigned value=p+1, k=0;
        while (k<count && strcmp(allowed[k],key)) k++;
        if (k==count || fields[k]>=0 || value>=j->count) return -1;
        fields[k]=(int)value; p=j->tokens[value].next;
    }
    return 0;
}
#endif
