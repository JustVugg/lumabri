/* One immutable registry snapshot per publication. Multi-model changes must
 * not be a series of independently visible route writes. All writers share
 * access.lock; readers see an old or new complete snapshot through rename.
 * Legacy individual records are read only until the first successful write.
 * An empty published registry is authoritative too: no removed route revives.
 * Requires model_routes' codec and legacy readers. */
#ifndef LMB_ROUTE_REGISTRY_H
#define LMB_ROUTE_REGISTRY_H
#include "lumabri_sha.h"
#define LMB_ROUTE_REGISTRY_BYTES (44u+LMB_ROUTE_MAX*(4u+LMB_ROUTE_BYTES))
typedef struct { uint32_t count; LmbModelRoute routes[LMB_ROUTE_MAX]; } LmbRouteRegistry;
static inline int lmb_route_registry_valid(const LmbRouteRegistry *s) {
    if (!s || s->count>LMB_ROUTE_MAX) return 0;
    for (uint32_t i=0;i<s->count;i++) {
        if (!lmb_route_valid(&s->routes[i])) return 0;
        for (uint32_t j=0;j<i;j++) if (!memcmp(s->routes[i].id,s->routes[j].id,32) ||
            !strcmp(s->routes[i].name,s->routes[j].name)) return 0;
    }
    return 1;
}
static inline int lmb_route_registry_encode(const LmbRouteRegistry *s,LmbBuf *b) {
    if (b->len || !lmb_route_registry_valid(s) || lmb_buf_bytes(b,"LMBREG01",8) || lmb_buf_u32(b,s->count)) return -1;
    for (uint32_t i=0;i<s->count;i++) {
        LmbBuf route={0}; int bad=lmb_route_encode(&s->routes[i],&route) ||
            lmb_buf_u32(b,(uint32_t)route.len) || lmb_buf_bytes(b,route.p,route.len);
        free(route.p); if (bad) return -1;
    }
    LmbSha sha; uint8_t digest[32]; lmb_sha_init(&sha); lmb_sha_update(&sha,b->p,b->len); lmb_sha_final(&sha,digest);
    return lmb_buf_bytes(b,digest,32);
}
static inline int lmb_route_registry_decode(const void *bytes,size_t n,LmbRouteRegistry *s) {
    memset(s,0,sizeof *s);
    if (n<44 || n>LMB_ROUTE_REGISTRY_BYTES || memcmp(bytes,"LMBREG01",8)) return -1;
    LmbSha sha; uint8_t digest[32]; lmb_sha_init(&sha); lmb_sha_update(&sha,bytes,n-32); lmb_sha_final(&sha,digest);
    if (memcmp(digest,(const uint8_t *)bytes+n-32,32)) return -1;
    LmbCur c={bytes,n-32,8};
    if (lmb_cur_u32(&c,&s->count) || s->count>LMB_ROUTE_MAX) return -1;
    for (uint32_t i=0;i<s->count;i++) {
        uint32_t size;
        if (lmb_cur_u32(&c,&size) || size>c.len-c.off ||
            lmb_route_decode((const uint8_t *)bytes+c.off,size,&s->routes[i])) return -1;
        c.off+=size;
    }
    return c.off!=c.len || !lmb_route_registry_valid(s) ? -1 : 0;
}
/* A reader can open the old inode immediately before atomic publication
 * unlinks its directory entry. Zero links is then a valid immutable snapshot,
 * not an unsafe alias. Multiple hard links remain forbidden. The descriptor
 * was obtained with O_NOFOLLOW from the private registry directory. */
static inline int lmb_route_registry_read_fd(int fd,LmbRouteRegistry *s) {
    struct stat st;
    if (fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&077) || !S_ISREG(st.st_mode) ||
        st.st_nlink>1 || st.st_size<44 || st.st_size>LMB_ROUTE_REGISTRY_BYTES) return LMB_ROUTE_UNSAFE;
    size_t n=(size_t)st.st_size; uint8_t *bytes=malloc(n);
    int bad=!bytes || lmb_read_full(fd,bytes,n) || lmb_route_registry_decode(bytes,n,s);
    free(bytes); return bad ? LMB_ROUTE_UNSAFE : 0;
}
static inline int lmb_route_registry_read(int dir,LmbRouteRegistry *s) {
    memset(s,0,sizeof *s);
    int fd=openat(dir,".registry",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) {
        if (errno!=ENOENT) return LMB_ROUTE_UNSAFE;
        uint8_t ids[LMB_ROUTE_MAX][32]; size_t count=0;
        int rc=lmb_route_legacy_list(dir,ids,&count); if (rc) return rc;
        for (size_t i=0;i<count;i++) {
            rc=lmb_route_legacy_load(dir,ids[i],&s->routes[i]); if (rc) return LMB_ROUTE_UNSAFE;
        }
        s->count=(uint32_t)count; return lmb_route_registry_valid(s) ? 0 : LMB_ROUTE_UNSAFE;
    }
    int rc=lmb_route_registry_read_fd(fd,s); close(fd); return rc;
}
/* Caller holds the registry writer lock. An incomplete .transaction can be
 * discarded, but an unsafe entry is never followed or overwritten. */
static inline int lmb_route_registry_publish(int dir,const LmbRouteRegistry *s) {
    struct stat st;
    if (!fstatat(dir,".transaction",&st,AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 ||
            unlinkat(dir,".transaction",0)) return LMB_ROUTE_UNSAFE;
    } else if (errno!=ENOENT) return LMB_ROUTE_UNSAFE;
    LmbBuf b={0}; int bad=lmb_route_registry_encode(s,&b),fd=-1;
    if (!bad) { fd=openat(dir,".transaction",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); bad=fd<0; }
    for (size_t at=0;!bad && at<b.len;) {
        ssize_t n=write(fd,b.p+at,b.len-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    free(b.p);
    if (fd>=0) { if (!bad && fsync(fd)) bad=1; if (close(fd)) bad=1; }
    if (!bad && renameat(dir,".transaction",dir,".registry")) bad=1;
    if (bad) (void)unlinkat(dir,".transaction",0); else if (fsync(dir)) bad=1;
    return bad ? LMB_ROUTE_UNSAFE : 0;
}
static inline int lmb_route_load(int dir,const uint8_t id[32],LmbModelRoute *route) {
    LmbRouteRegistry *s=calloc(1,sizeof *s); if (!s) return LMB_ROUTE_UNSAFE;
    int rc=lmb_route_registry_read(dir,s);
    if (!rc) {
        rc=LMB_ROUTE_MISSING;
        for (uint32_t i=0;i<s->count;i++) if (!memcmp(s->routes[i].id,id,32)) { *route=s->routes[i]; rc=0; break; }
    }
    free(s); return rc;
}
static inline int lmb_route_list(int dir,uint8_t ids[LMB_ROUTE_MAX][32],size_t *count) {
    *count=0; LmbRouteRegistry *s=calloc(1,sizeof *s); if (!s) return LMB_ROUTE_UNSAFE;
    int rc=lmb_route_registry_read(dir,s);
    if (!rc) { *count=s->count; for (uint32_t i=0;i<s->count;i++) memcpy(ids[i],s->routes[i].id,32); }
    free(s); return rc;
}
/* Revision zero creates. Every expected revision and immutable model
 * contract must match, or none of the proposed routes becomes visible. */
static inline int lmb_route_save_many(int dir,const LmbModelRoute *routes,const uint32_t *expected,uint32_t count) {
    if (!routes || !expected || !count || count>LMB_ROUTE_MAX) return LMB_ROUTE_UNSAFE;
    for (uint32_t i=0;i<count;i++) {
        if (!lmb_route_valid(&routes[i]) || expected[i]==UINT32_MAX || routes[i].revision!=expected[i]+1) return LMB_ROUTE_UNSAFE;
        for (uint32_t j=0;j<i;j++) if (!memcmp(routes[i].id,routes[j].id,32)) return LMB_ROUTE_CONFLICT;
    }
    int lock=lmb_api_access_lock(dir); if (lock<0) return LMB_ROUTE_BUSY;
    LmbRouteRegistry *s=calloc(1,sizeof *s); int rc=s ? lmb_route_registry_read(dir,s) : LMB_ROUTE_UNSAFE;
    for (uint32_t i=0;!rc && i<count;i++) {
        uint32_t at=0; const LmbModelRoute *r=&routes[i];
        while (at<s->count && memcmp(s->routes[at].id,r->id,32)) at++;
        if (at==s->count) {
            if (expected[i]) { rc=LMB_ROUTE_MISSING; break; }
            if (s->count==LMB_ROUTE_MAX) { rc=LMB_ROUTE_QUOTA; break; }
            s->count++;
        } else {
            const LmbModelRoute *old=&s->routes[at];
            if (old->revision!=expected[i] || memcmp(old->content,r->content,32) || strcmp(old->tracker,r->tracker) ||
                strcmp(old->adapter,r->adapter) || strcmp(old->numeric_class,r->numeric_class) ||
                old->numeric_abi!=r->numeric_abi || old->greedy_only!=r->greedy_only) { rc=LMB_ROUTE_CONFLICT; break; }
        }
        s->routes[at]=*r;
    }
    if (!rc && !lmb_route_registry_valid(s)) rc=LMB_ROUTE_CONFLICT;
    if (!rc) rc=lmb_route_registry_publish(dir,s);
    free(s); close(lock); return rc;
}
static inline int lmb_route_save(int dir,const LmbModelRoute *r,uint32_t expected) {
    return lmb_route_save_many(dir,r,&expected,1);
}
static inline int lmb_route_remove(int dir,const uint8_t id[32],uint32_t revision) {
    int lock=lmb_api_access_lock(dir); if (lock<0) return LMB_ROUTE_BUSY;
    LmbRouteRegistry *s=calloc(1,sizeof *s); int rc=s ? lmb_route_registry_read(dir,s) : LMB_ROUTE_UNSAFE;
    if (!rc) {
        uint32_t at=0; while (at<s->count && memcmp(s->routes[at].id,id,32)) at++;
        if (at==s->count) rc=LMB_ROUTE_MISSING;
        else if (s->routes[at].revision!=revision) rc=LMB_ROUTE_CONFLICT;
        else { s->routes[at]=s->routes[--s->count]; rc=lmb_route_registry_publish(dir,s); }
    }
    free(s); close(lock); return rc;
}
#endif
