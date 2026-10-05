/* Operator-owned logical model identities. A route references exact,
 * independently approved allocations; it never authorizes model preparation.
 * Source content identity is distinct from each allocation's routing root. */
#ifndef LMB_MODEL_ROUTES_H
#define LMB_MODEL_ROUTES_H
#include "lumabri_api_access.h"
#include <dirent.h>

#define LMB_ROUTE_MAX 32u
#define LMB_ROUTE_REPLICAS 8u
#define LMB_ROUTE_BYTES 4096u
enum { LMB_ROUTE_ORDERED=0, LMB_ROUTE_OBSERVED_DECODE=1, LMB_ROUTE_DECLARED_COST=2 };
static inline const char *lmb_route_policy_name(uint32_t policy) {
    return policy==LMB_ROUTE_ORDERED ? "ordered" : policy==LMB_ROUTE_OBSERVED_DECODE ? "observed-decode" :
        policy==LMB_ROUTE_DECLARED_COST ? "declared-cost" : "invalid";
}
typedef struct {
    uint8_t allocation[32], root[32], host_key[32];
} LmbModelReplica;
typedef struct {
    uint8_t id[32], content[32];
    char name[33], tracker[256], adapter[32], numeric_class[97];
    uint32_t revision, numeric_abi, context, max_new, greedy_only, count;
    uint32_t policy;
    LmbModelReplica replicas[LMB_ROUTE_REPLICAS];
} LmbModelRoute;
enum { LMB_ROUTE_OK=0, LMB_ROUTE_MISSING, LMB_ROUTE_CONFLICT, LMB_ROUTE_QUOTA, LMB_ROUTE_UNSAFE, LMB_ROUTE_BUSY };

static inline int lmb_route_nonzero(const uint8_t value[32]) {
    unsigned yes=0; for (unsigned i=0; i<32; i++) yes|=value[i]; return yes!=0;
}
static inline int lmb_route_text(const char *s, size_t cap) {
    size_t n=strnlen(s,cap); if (!n || n==cap) return 0;
    for (size_t i=0; i<n; i++) if ((unsigned char)s[i]<33 || (unsigned char)s[i]>126) return 0;
    return 1;
}
static inline int lmb_route_valid(const LmbModelRoute *r) {
    if (!r || !lmb_route_nonzero(r->id) || !lmb_route_nonzero(r->content) ||
        !lmb_api_username(r->name) || !lmb_route_text(r->tracker,sizeof r->tracker) ||
        !lmb_route_text(r->adapter,sizeof r->adapter) || !lmb_route_text(r->numeric_class,sizeof r->numeric_class) ||
        !r->revision || !r->numeric_abi || !r->context || r->context>(1u<<20) ||
        !r->max_new || r->max_new>(1u<<20) || r->greedy_only>1 || !r->count || r->count>LMB_ROUTE_REPLICAS ||
        r->policy>LMB_ROUTE_DECLARED_COST) return 0;
    for (uint32_t i=0; i<r->count; i++) {
        const LmbModelReplica *p=&r->replicas[i];
        if (!lmb_route_nonzero(p->allocation) || !lmb_route_nonzero(p->root) || !lmb_route_nonzero(p->host_key)) return 0;
        for (uint32_t k=0; k<i; k++) if (!memcmp(p->allocation,r->replicas[k].allocation,32)) return 0;
    }
    return 1;
}
static inline int lmb_route_dir(int access, int create) {
    if (create && mkdirat(access,"models",0700) && errno!=EEXIST) return -1;
    int fd=openat(access,"models",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); errno=EPERM; return -1; }
    return fd;
}
static inline int lmb_route_string(LmbCur *c, char *text, size_t cap) {
    size_t start=c->off;
    if (lmb_cur_str(c,text,cap)) return -1;
    return strlen(text)==c->off-start-2 ? 0 : -1;
}
static inline int lmb_route_encode(const LmbModelRoute *r, LmbBuf *b) {
    if (!lmb_route_valid(r) || lmb_buf_reserve(b,LMB_ROUTE_BYTES)) return -1;
    return lmb_buf_bytes(b,"LMBROUT2",8) || lmb_buf_bytes(b,r->id,32) || lmb_buf_bytes(b,r->content,32) ||
        lmb_buf_u32(b,r->revision) || lmb_buf_str(b,r->name) || lmb_buf_str(b,r->tracker) ||
        lmb_buf_str(b,r->adapter) || lmb_buf_str(b,r->numeric_class) || lmb_buf_u32(b,r->numeric_abi) ||
        lmb_buf_u32(b,r->context) || lmb_buf_u32(b,r->max_new) || lmb_buf_u32(b,r->greedy_only) || lmb_buf_u32(b,r->count) ||
        lmb_buf_bytes(b,r->replicas,(size_t)r->count*sizeof *r->replicas) || lmb_buf_u32(b,r->policy) ? -1 : 0;
}
static inline int lmb_route_decode(const void *bytes, size_t n, LmbModelRoute *r) {
    memset(r,0,sizeof *r);
    if (n<8 || n>LMB_ROUTE_BYTES || (memcmp(bytes,"LMBROUT1",8) && memcmp(bytes,"LMBROUT2",8))) return -1;
    int version2=!memcmp(bytes,"LMBROUT2",8);
    LmbCur c={bytes,n,8};
    if (lmb_cur_bytes(&c,r->id,32) || lmb_cur_bytes(&c,r->content,32) ||
        lmb_cur_u32(&c,&r->revision) || lmb_route_string(&c,r->name,sizeof r->name) ||
        lmb_route_string(&c,r->tracker,sizeof r->tracker) || lmb_route_string(&c,r->adapter,sizeof r->adapter) ||
        lmb_route_string(&c,r->numeric_class,sizeof r->numeric_class) || lmb_cur_u32(&c,&r->numeric_abi) ||
        lmb_cur_u32(&c,&r->context) || lmb_cur_u32(&c,&r->max_new) || lmb_cur_u32(&c,&r->greedy_only) || lmb_cur_u32(&c,&r->count) ||
        r->count>LMB_ROUTE_REPLICAS || lmb_cur_bytes(&c,r->replicas,(size_t)r->count*sizeof *r->replicas) ||
        (version2 && lmb_cur_u32(&c,&r->policy)) ||
        c.off!=n || !lmb_route_valid(r)) return -1;
    return 0;
}
static inline int lmb_route_load(int dir, const uint8_t id[32], LmbModelRoute *route) {
    char hex[65]; lmb_hex(hex,id,32);
    int fd=openat(dir,hex,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? LMB_ROUTE_MISSING : LMB_ROUTE_UNSAFE;
    unsigned char bytes[LMB_ROUTE_BYTES]; struct stat st;
    if (lmb_api_private_fd(fd,0) || fstat(fd,&st) || st.st_size<8 || st.st_size>LMB_ROUTE_BYTES) {
        close(fd); return LMB_ROUTE_UNSAFE;
    }
    size_t size=(size_t)st.st_size;
    if (size>sizeof bytes) { close(fd); return LMB_ROUTE_UNSAFE; }
    int bad=lmb_read_full(fd,bytes,size); close(fd);
    return bad || lmb_route_decode(bytes,size,route) || memcmp(route->id,id,32) ? LMB_ROUTE_UNSAFE : LMB_ROUTE_OK;
}
static inline int lmb_route_list(int dir, uint8_t ids[LMB_ROUTE_MAX][32], size_t *count) {
    *count=0; int fd=openat(dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return LMB_ROUTE_UNSAFE;
    DIR *stream=fdopendir(fd); if (!stream) { close(fd); return LMB_ROUTE_UNSAFE; }
    int rc=LMB_ROUTE_OK; struct dirent *entry; errno=0;
    while ((entry=readdir(stream))) {
        uint8_t id[32];
        if (strlen(entry->d_name)!=64 || lmb_unhex(id,entry->d_name,32)) continue;
        char canonical[65]; lmb_hex(canonical,id,32);
        if (strcmp(canonical,entry->d_name) || *count==LMB_ROUTE_MAX) { rc=LMB_ROUTE_UNSAFE; break; }
        memcpy(ids[(*count)++],id,32); errno=0;
    }
    if (!entry && errno) rc=LMB_ROUTE_UNSAFE;
    closedir(stream); return rc;
}
/* Revision zero creates. An update can change the approved replica set and
 * lower/raise limits after validation, but never silently change checkpoint,
 * adapter or numeric semantics behind existing grants and conversation IDs. */
static inline int lmb_route_save(int dir, const LmbModelRoute *r, uint32_t expected) {
    if (!lmb_route_valid(r) || expected==UINT32_MAX || r->revision!=expected+1) return LMB_ROUTE_UNSAFE;
    int lock=lmb_api_access_lock(dir); if (lock<0) return LMB_ROUTE_BUSY;
    LmbModelRoute old; int rc=lmb_route_load(dir,r->id,&old);
    if (rc==LMB_ROUTE_OK) {
        rc=old.revision==expected && !memcmp(old.content,r->content,32) && !strcmp(old.tracker,r->tracker) &&
            !strcmp(old.adapter,r->adapter) && !strcmp(old.numeric_class,r->numeric_class) &&
            old.numeric_abi==r->numeric_abi && old.greedy_only==r->greedy_only ?
            LMB_ROUTE_OK : LMB_ROUTE_CONFLICT;
    } else if (rc==LMB_ROUTE_MISSING) rc=expected ? LMB_ROUTE_MISSING : LMB_ROUTE_OK;
    if (rc) goto done;
    uint8_t ids[LMB_ROUTE_MAX][32]; size_t count;
    rc=lmb_route_list(dir,ids,&count); if (rc) goto done;
    if (!expected && count==LMB_ROUTE_MAX) { rc=LMB_ROUTE_QUOTA; goto done; }
    for (size_t i=0; i<count; i++) {
        LmbModelRoute other; rc=lmb_route_load(dir,ids[i],&other); if (rc) goto done;
        if (memcmp(other.id,r->id,32) && !strcmp(other.name,r->name)) { rc=LMB_ROUTE_CONFLICT; goto done; }
    }
    struct stat st;
    if (!fstatat(dir,".transaction",&st,AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 ||
            unlinkat(dir,".transaction",0)) { rc=LMB_ROUTE_UNSAFE; goto done; }
    } else if (errno!=ENOENT) { rc=LMB_ROUTE_UNSAFE; goto done; }
    LmbBuf b={0};
    if (lmb_route_encode(r,&b)) { free(b.p); rc=LMB_ROUTE_UNSAFE; goto done; }
    int fd=openat(dir,".transaction",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600), bad=fd<0;
    for (size_t at=0; !bad && at<b.len;) {
        ssize_t n=write(fd,b.p+at,b.len-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    free(b.p);
    if (fd>=0) { if (!bad && fsync(fd)) bad=1; if (close(fd)) bad=1; }
    char hex[65]; lmb_hex(hex,r->id,32);
    if (!bad && renameat(dir,".transaction",dir,hex)) bad=1;
    if (bad) (void)unlinkat(dir,".transaction",0); else if (fsync(dir)) bad=1;
    rc=bad ? LMB_ROUTE_UNSAFE : LMB_ROUTE_OK;
done:
    close(lock); return rc;
}
static inline int lmb_route_remove(int dir, const uint8_t id[32], uint32_t revision) {
    int lock=lmb_api_access_lock(dir); if (lock<0) return LMB_ROUTE_BUSY;
    LmbModelRoute route; int rc=lmb_route_load(dir,id,&route);
    if (!rc && route.revision!=revision) rc=LMB_ROUTE_CONFLICT;
    if (!rc) { char hex[65]; lmb_hex(hex,id,32); if (unlinkat(dir,hex,0) || fsync(dir)) rc=LMB_ROUTE_UNSAFE; }
    close(lock); return rc;
}
#endif
