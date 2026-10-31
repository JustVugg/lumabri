/* Explicit executable roles, independent of detected hardware and family
 * table ordering. Empty known lists mean unavailable, not all adapters. */
#ifndef LMB_RUNTIME_ROLES_H
#define LMB_RUNTIME_ROLES_H
#include <stdint.h>
#include <string.h>
typedef struct {
    uint32_t known;
    char segment[256],edge[256],hybrid[256]; /* comma-separated exact adapter IDs */
} LmbRuntimeRoles;
static inline int lmb_runtime_role_list_valid(const char *s) {
    if (!*s) return 1;
    unsigned length=0;
    for (;*s;s++) {
        if (*s==',') { if (!length) return 0;length=0; }
        else if ((*s>='a' && *s<='z') || (*s>='0' && *s<='9') || *s=='_') {
            if (++length>63) return 0;
        } else return 0;
    }
    return length!=0;
}
static inline int lmb_runtime_roles_valid(const LmbRuntimeRoles *r) {
    return r->known<=1 && (r->known || (!r->segment[0] && !r->edge[0] && !r->hybrid[0])) &&
        lmb_runtime_role_list_valid(r->segment) && lmb_runtime_role_list_valid(r->edge) &&
        lmb_runtime_role_list_valid(r->hybrid);
}
static inline int lmb_runtime_role_has(const char *list,const char *id) {
    if (!id || !*id) return 0;
    size_t size=strlen(id);
    for (const char *s=list;*s;) {
        size_t n=strcspn(s,",");if (size==n && !memcmp(id,s,n)) return 1;
        s+=n;if (*s) s++;
    }
    return 0;
}
static inline int lmb_runtime_role_allows(const LmbRuntimeRoles *r,const char *id,int edge,int hybrid) {
    /* Legacy peers remain subject to allocation-time admission. Explicit
     * negative capability must never become permission through a fallback. */
    return !r->known || (lmb_runtime_role_has(r->segment,id) &&
        (!edge || lmb_runtime_role_has(r->edge,id)) && (!hybrid || lmb_runtime_role_has(r->hybrid,id)));
}
#endif
