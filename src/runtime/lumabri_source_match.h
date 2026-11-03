/* Filename patterns used by --include. Case-sensitive, '/' is ordinary, and
 * backslash escapes metacharacters, matching fnmatch(pattern, path, 0). */
#ifndef LUMABRI_SOURCE_MATCH_H
#define LUMABRI_SOURCE_MATCH_H
#ifndef _WIN32
#include <fnmatch.h>
static int lmb_source_match(const char *pattern,const char *text) {return fnmatch(pattern,text,0)==0;}
#else
#include <ctype.h>
#include <string.h>
static int lmb_source_class(const char *name,size_t n,unsigned char c) {
#define CLASS(s,f) if (n==sizeof(s)-1 && !memcmp(name,s,n)) return f(c)!=0
    CLASS("alnum",isalnum);CLASS("alpha",isalpha);CLASS("blank",isblank);
    CLASS("cntrl",iscntrl);CLASS("digit",isdigit);CLASS("graph",isgraph);
    CLASS("lower",islower);CLASS("print",isprint);CLASS("punct",ispunct);
    CLASS("space",isspace);CLASS("upper",isupper);CLASS("xdigit",isxdigit);
#undef CLASS
    return 0;
}
static int lmb_source_atom(const char **pattern,unsigned char c) {
    const char *p=*pattern;
    if (*p=='?') {*pattern=p+1;return 1;}
    if (*p=='[') {
        const char *q=p+1;int negate=*q=='!'||*q=='^',matched=0,first=1;
        if (negate) q++;
        while (*q && (*q!=']'||first)) {
            first=0;
            if (q[0]=='[' && q[1]==':') {
                const char *end=strstr(q+2,":]");
                if (end) {matched|=lmb_source_class(q+2,(size_t)(end-q-2),c);q=end+2;continue;}
            }
            if (*q=='\\' && q[1]) q++;
            unsigned char lo=(unsigned char)*q++,hi=lo;
            if (*q=='-' && q[1] && q[1]!=']') {
                q++;if (*q=='\\' && q[1]) q++;hi=(unsigned char)*q++;
            }
            matched|=c>=lo && c<=hi;
        }
        if (*q==']') {*pattern=q+1;return negate?!matched:matched;}
    }
    if (*p=='\\') {p++;if (!*p) return 0;}
    *pattern=p+1;return (unsigned char)*p==c;
}
static int lmb_source_match(const char *pattern,const char *text) {
    const char *star=NULL,*retry=NULL;
    while (*text) {
        if (*pattern=='*') {while (*pattern=='*') pattern++;star=pattern;retry=text;}
        if (*pattern) {
            const char *next=pattern;
            if (lmb_source_atom(&next,(unsigned char)*text)) {pattern=next;text++;continue;}
        }
        if (!star) return 0;
        pattern=star;text=++retry;
    }
    while (*pattern=='*') pattern++;
    return !*pattern;
}
#endif
#endif
