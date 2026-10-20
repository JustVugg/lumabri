/* Owner-private, checksum protected lifecycle intent. Callers hold the
 * per-model lock through reconciliation. A lost fsync acknowledgement is
 * uncertainty, not permission to allocate again. */
#ifndef LMB_ELASTIC_STORE_H
#define LMB_ELASTIC_STORE_H
#include "lumabri_elastic_policy.h"
#define LMB_ELASTIC_BYTES 16384u
static inline void lmb_elastic_digest(const void *data,size_t n,uint8_t out[32]) {
    LmbSha s; lmb_sha_init(&s); lmb_sha_update(&s,data,n); lmb_sha_final(&s,out);
}
static inline int lmb_elastic_encode(const LmbElasticPolicy *p,LmbBuf *b) {
    if (b->len || !lmb_elastic_valid(p)) return -1;
#define EP(call) do { if (call) return -1; } while (0)
    EP(lmb_buf_bytes(b,"LMBELA02",8));
    const uint32_t values[]={p->revision,p->enabled,p->faulted,p->min_replicas,p->max_replicas,p->cooldown,
        p->idle_seconds,p->up_rejections,p->ttft_ms,p->phase,p->request.context,p->request.sessions,
        p->request.max_new,p->request.node_count};
    for (unsigned i=0;i<sizeof values/sizeof *values;i++) EP(lmb_buf_u32(b,values[i]));
    EP(lmb_buf_u64(b,p->max_reserved_bytes)); EP(lmb_buf_u64(b,p->last_change)); EP(lmb_buf_u64(b,p->last_check));
    EP(lmb_buf_u64(b,p->pending_bytes)); EP(lmb_buf_u64(b,p->idle_since_ms));
    for (unsigned i=0;i<LMB_ROUTE_REPLICAS;i++) EP(lmb_buf_u64(b,p->resident_bytes[i]));
    EP(lmb_buf_bytes(b,p->pending_allocation,32)); EP(lmb_buf_bytes(b,p->observer,32));
    EP(lmb_buf_str(b,p->source_stamp)); EP(lmb_buf_str(b,p->reason));
    EP(lmb_buf_str(b,p->request.models[0])); EP(lmb_buf_str(b,p->request.operation)); EP(lmb_buf_str(b,p->request.review));
    for (uint32_t i=0;i<p->request.node_count;i++) EP(lmb_buf_str(b,p->request.nodes[i]));
    for (unsigned i=0;i<2;i++) {
        LmbBuf r={0};
        int used=!i || p->phase==LMB_ELASTIC_PUBLISH_GROW || p->phase==LMB_ELASTIC_PUBLISH_SHRINK;
        int bad=used && lmb_route_encode(i ? &p->target : &p->route,&r);
        if (!bad) bad=lmb_buf_u32(b,(uint32_t)r.len) || (r.len && lmb_buf_bytes(b,r.p,r.len));
        free(r.p); if (bad) return -1;
    }
    EP(lmb_buf_bytes(b,p->health_observer,32));
    EP(lmb_buf_u32(b,p->health_revision)); EP(lmb_buf_u32(b,p->health_cursor));
    for (unsigned i=0;i<LMB_ROUTE_REPLICAS;i++) {
        EP(lmb_buf_u64(b,p->health[i].at_ms)); EP(lmb_buf_u64(b,p->health[i].at_wall));
        EP(lmb_buf_u32(b,p->health[i].state)); EP(lmb_buf_u32(b,p->health[i].failures));
    }
    uint8_t hash[32]; lmb_elastic_digest(b->p,b->len,hash); EP(lmb_buf_bytes(b,hash,32));
    return b->len>LMB_ELASTIC_BYTES ? -1 : 0;
#undef EP
}
static inline int lmb_elastic_decode(const void *bytes,size_t n,LmbElasticPolicy *out) {
    memset(out,0,sizeof *out);
    if (!bytes || n<320 || n>LMB_ELASTIC_BYTES ||
        (memcmp(bytes,"LMBELA01",8) && memcmp(bytes,"LMBELA02",8))) return -1;
    int v2=!memcmp(bytes,"LMBELA02",8);
    uint8_t hash[32]; lmb_elastic_digest(bytes,n-32,hash);
    if (memcmp(hash,(const uint8_t *)bytes+n-32,32)) return -1;
    LmbElasticPolicy p={0}; LmbCur c={bytes,n-32,8};
#define EG(call) do { if (call) return -1; } while (0)
    uint32_t *values[]={&p.revision,&p.enabled,&p.faulted,&p.min_replicas,&p.max_replicas,&p.cooldown,
        &p.idle_seconds,&p.up_rejections,&p.ttft_ms,&p.phase,&p.request.context,&p.request.sessions,
        &p.request.max_new,&p.request.node_count};
    for (unsigned i=0;i<sizeof values/sizeof *values;i++) EG(lmb_cur_u32(&c,values[i]));
    if (p.request.node_count>LMB_PREPARATION_NODES) return -1;
    p.request.model_count=1; strcpy(p.request.action,"start");
    EG(lmb_cur_u64(&c,&p.max_reserved_bytes)); EG(lmb_cur_u64(&c,&p.last_change)); EG(lmb_cur_u64(&c,&p.last_check));
    EG(lmb_cur_u64(&c,&p.pending_bytes)); EG(lmb_cur_u64(&c,&p.idle_since_ms));
    for (unsigned i=0;i<LMB_ROUTE_REPLICAS;i++) EG(lmb_cur_u64(&c,&p.resident_bytes[i]));
    EG(lmb_cur_bytes(&c,p.pending_allocation,32)); EG(lmb_cur_bytes(&c,p.observer,32));
    EG(lmb_route_string(&c,p.source_stamp,sizeof p.source_stamp)); EG(lmb_route_string(&c,p.reason,sizeof p.reason));
    EG(lmb_route_string(&c,p.request.models[0],64)); EG(lmb_route_string(&c,p.request.operation,65));
    EG(lmb_route_string(&c,p.request.review,65));
    for (uint32_t i=0;i<p.request.node_count;i++) EG(lmb_route_string(&c,p.request.nodes[i],65));
    for (unsigned i=0;i<2;i++) {
        uint32_t size; EG(lmb_cur_u32(&c,&size));
        int used=!i || p.phase==LMB_ELASTIC_PUBLISH_GROW || p.phase==LMB_ELASTIC_PUBLISH_SHRINK;
        if ((!!size!=!!used) || size>c.len-c.off ||
            (used && lmb_route_decode((const uint8_t *)bytes+c.off,size,i ? &p.target : &p.route))) return -1;
        c.off+=size;
    }
    if (v2) {
        EG(lmb_cur_bytes(&c,p.health_observer,32));
        EG(lmb_cur_u32(&c,&p.health_revision)); EG(lmb_cur_u32(&c,&p.health_cursor));
        for (unsigned i=0;i<LMB_ROUTE_REPLICAS;i++) {
            EG(lmb_cur_u64(&c,&p.health[i].at_ms)); EG(lmb_cur_u64(&c,&p.health[i].at_wall));
            EG(lmb_cur_u32(&c,&p.health[i].state)); EG(lmb_cur_u32(&c,&p.health[i].failures));
        }
    }
    if (c.off!=c.len || !lmb_elastic_valid(&p)) return -1;
    *out=p; return 0;
#undef EG
}
static inline int lmb_elastic_root(int access,int create) {
    if (create && mkdirat(access,"elastic",0700) && errno!=EEXIST) return -1;
    int fd=openat(access,"elastic",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_elastic_dir(int root,const uint8_t id[32],int create) {
    if (!lmb_route_nonzero(id)) return -1;
    char name[65]; lmb_hex(name,id,32);
    if (create && mkdirat(root,name,0700) && errno!=EEXIST) return -1;
    int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_elastic_load(int dir,LmbElasticPolicy *p) {
    memset(p,0,sizeof *p);
    int fd=openat(dir,"policy",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat st; uint8_t bytes[LMB_ELASTIC_BYTES];
    int bad=fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) ||
        st.st_nlink>1 || st.st_size<320 || st.st_size>LMB_ELASTIC_BYTES;
    if (!bad) bad=lmb_read_full(fd,bytes,(size_t)st.st_size) || lmb_elastic_decode(bytes,(size_t)st.st_size,p);
    close(fd); return bad ? -1 : 0;
}
static inline int lmb_elastic_save(int dir,const LmbElasticPolicy *p) {
    LmbElasticPolicy prior; if (lmb_elastic_load(dir,&prior)<0) return -1;
    LmbBuf b={0}; if (lmb_elastic_encode(p,&b)) { free(b.p); return -1; }
    struct stat st; int bad=0;
    if (!fstatat(dir,".pending",&st,AT_SYMLINK_NOFOLLOW)) {
        bad=!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 || unlinkat(dir,".pending",0);
    } else if (errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,".pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    for (size_t at=0;!bad && at<b.len;) {
        ssize_t n=write(fd,b.p+at,b.len-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    free(b.p);
    if (fd>=0) { if (!bad && fsync(fd)) bad=1; if (close(fd)) bad=1; }
    if (!bad) bad=renameat(dir,".pending",dir,"policy") || fsync(dir);
    return bad ? -1 : 0;
}
#endif
