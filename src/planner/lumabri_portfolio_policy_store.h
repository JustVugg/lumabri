/* One owner-private policy, guarded across configuration, decision, intent
 * and atomic route publication. No pending decision is blindly replayed. */
#ifndef LMB_PORTFOLIO_POLICY_STORE_H
#define LMB_PORTFOLIO_POLICY_STORE_H
#include "lumabri_portfolio_policy.h"
#include "lumabri_capacity_store.h"
#include "src/runtime/lumabri_model_routes.h"
#define LMB_POLICY_BYTES 4096u

static inline int lmb_policy_digest(const void *bytes,size_t n,uint8_t out[32]) {
    LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,bytes,n); lmb_sha_final(&sha,out); return 0;
}
static inline int lmb_policy_record_hash(const LmbCapacity *r,uint8_t out[32]) {
    LmbBuf b={0}; int bad=lmb_capacity_encode(r,&b);
    if (!bad) lmb_policy_digest(b.p,b.len,out);
    free(b.p); return bad;
}
static inline int lmb_policy_route_hash(const LmbModelRoute *r,uint8_t out[32]) {
    LmbBuf b={0}; int bad=lmb_route_encode(r,&b);
    if (!bad) lmb_policy_digest(b.p,b.len,out);
    free(b.p); return bad;
}
static inline int lmb_policy_encode(const LmbPortfolioPolicy *p,LmbBuf *b) {
    if (!b || b->len || !lmb_portfolio_policy_valid(p)) return -1;
#define PUT(call) do { if (call) return -1; } while (0)
    PUT(lmb_buf_bytes(b,"LMBPOL01",8));
    const uint32_t values[]={p->revision,p->enabled,p->models,p->candidates,p->current,p->ttft_ms,p->gap_ms,
        p->max_age,p->cooldown,p->horizon,p->min_saving_bps,p->pending,p->faulted};
    for (unsigned i=0;i<sizeof values/sizeof *values;i++) PUT(lmb_buf_u32(b,values[i]));
    PUT(lmb_buf_u64(b,p->ceiling_micro_per_hour)); PUT(lmb_buf_u64(b,p->switch_cost_micro));
    PUT(lmb_buf_u64(b,p->last_change)); PUT(lmb_buf_u64(b,p->last_check));
    PUT(lmb_buf_str(b,p->currency)); PUT(lmb_buf_str(b,p->tracker)); PUT(lmb_buf_str(b,p->reason));
    for (uint32_t i=0;i<p->models;i++) {
        PUT(lmb_buf_bytes(b,p->model_ids[i],32)); PUT(lmb_buf_u32(b,p->route_revisions[i]));
        PUT(lmb_buf_bytes(b,p->routes[i],32)); PUT(lmb_buf_bytes(b,p->targets[i],32));
    }
    for (uint32_t i=0;i<p->candidates;i++) {
        PUT(lmb_buf_str(b,p->names[i])); PUT(lmb_buf_bytes(b,p->records[i],32));
    }
    uint8_t digest[32]; lmb_policy_digest(b->p,b->len,digest); PUT(lmb_buf_bytes(b,digest,32));
    return b->len>LMB_POLICY_BYTES ? -1 : 0;
#undef PUT
}
static inline int lmb_policy_decode(const void *bytes,size_t n,LmbPortfolioPolicy *out) {
    memset(out,0,sizeof *out);
    if (!bytes || n<124 || n>LMB_POLICY_BYTES || memcmp(bytes,"LMBPOL01",8)) return -1;
    uint8_t digest[32]; lmb_policy_digest(bytes,n-32,digest);
    if (memcmp(digest,(const uint8_t *)bytes+n-32,32)) return -1;
    LmbCur c={bytes,n-32,8}; LmbPortfolioPolicy p={0};
#define GET(call) do { if (call) return -1; } while (0)
    uint32_t *values[]={&p.revision,&p.enabled,&p.models,&p.candidates,&p.current,&p.ttft_ms,&p.gap_ms,
        &p.max_age,&p.cooldown,&p.horizon,&p.min_saving_bps,&p.pending,&p.faulted};
    for (unsigned i=0;i<sizeof values/sizeof *values;i++) GET(lmb_cur_u32(&c,values[i]));
    if (p.models>LMB_CAPACITY_MODELS || p.candidates>LMB_CAPACITY_MODELS) return -1;
    GET(lmb_cur_u64(&c,&p.ceiling_micro_per_hour)); GET(lmb_cur_u64(&c,&p.switch_cost_micro));
    GET(lmb_cur_u64(&c,&p.last_change)); GET(lmb_cur_u64(&c,&p.last_check));
    GET(lmb_cal_get_text(&c,p.currency,sizeof p.currency)); GET(lmb_cal_get_text(&c,p.tracker,sizeof p.tracker));
    GET(lmb_cal_get_text(&c,p.reason,sizeof p.reason));
    for (uint32_t i=0;i<p.models;i++) {
        GET(lmb_cur_bytes(&c,p.model_ids[i],32)); GET(lmb_cur_u32(&c,&p.route_revisions[i]));
        GET(lmb_cur_bytes(&c,p.routes[i],32)); GET(lmb_cur_bytes(&c,p.targets[i],32));
    }
    for (uint32_t i=0;i<p.candidates;i++) {
        GET(lmb_cal_get_text(&c,p.names[i],sizeof p.names[i])); GET(lmb_cur_bytes(&c,p.records[i],32));
    }
    if (c.off!=c.len || !lmb_portfolio_policy_valid(&p)) return -1;
    *out=p; return 0;
#undef GET
}
static inline int lmb_policy_dir(int access,int create) {
    if (create && mkdirat(access,"portfolio-policy",0700) && errno!=EEXIST) return -1;
    int fd=openat(access,"portfolio-policy",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_policy_lock(int dir) {
    int fd=openat(dir,"policy.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if (fd>=0 && (lmb_api_private_fd(fd,0) || flock(fd,LOCK_EX|LOCK_NB))) { close(fd); return -1; }
    return fd;
}
/* 1 means unconfigured; corruption is never treated as absence. Atomic
 * rename can unlink an already-open old snapshot, so nlink==0 is valid. */
static inline int lmb_policy_load(int dir,LmbPortfolioPolicy *out) {
    memset(out,0,sizeof *out);
    int fd=openat(dir,"policy",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat st; unsigned char bytes[LMB_POLICY_BYTES];
    int bad=fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) ||
        st.st_nlink>1 || st.st_size<124 || st.st_size>LMB_POLICY_BYTES;
    if (!bad) bad=lmb_read_full(fd,bytes,(size_t)st.st_size) || lmb_policy_decode(bytes,(size_t)st.st_size,out);
    close(fd); return bad ? -1 : 0;
}
/* Caller holds policy.lock. An unsuccessful fsync is not proof of rollback;
 * the next reader must inspect the durable record and exact route hashes. */
static inline int lmb_policy_save(int dir,const LmbPortfolioPolicy *p) {
    LmbPortfolioPolicy prior; if (lmb_policy_load(dir,&prior)<0) return -1;
    LmbBuf b={0}; if (lmb_policy_encode(p,&b)) { free(b.p); return -1; }
    struct stat st; int bad=0;
    if (!fstatat(dir,".transaction",&st,AT_SYMLINK_NOFOLLOW)) {
        bad=!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1;
        if (!bad) bad=unlinkat(dir,".transaction",0);
    } else if (errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,".transaction",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    if (!bad) bad=lmb_capacity_write(fd,b.p,b.len) || fsync(fd);
    if (fd>=0 && close(fd)) bad=1;
    free(b.p);
    if (!bad) bad=renameat(dir,".transaction",dir,"policy") || fsync(dir);
    return bad ? -1 : 0;
}
/* One registry snapshot; never combine independently loaded route revisions.
 * Recognize only the exact old or exact intended full records. */
static inline int lmb_policy_reconcile(LmbPortfolioPolicy *p,const LmbRouteRegistry *s,uint64_t now) {
    int old=1,target=p->pending!=0;
    for (uint32_t i=0;i<p->models;i++) {
        const LmbModelRoute *r=NULL;
        for (uint32_t j=0;j<s->count;j++) if (!memcmp(s->routes[j].id,p->model_ids[i],32)) r=&s->routes[j];
        uint8_t hash[32];
        if (!r || lmb_policy_route_hash(r,hash)) { old=target=0; break; }
        old=old && !memcmp(hash,p->routes[i],32);
        target=target && p->route_revisions[i]<UINT32_MAX &&
            r->revision==p->route_revisions[i]+1 && !memcmp(hash,p->targets[i],32);
    }
    if (target) {
        p->current=p->pending-1; p->last_change=now;
        for (uint32_t i=0;i<p->models;i++) { p->route_revisions[i]++; memcpy(p->routes[i],p->targets[i],32); }
        strcpy(p->reason,"change_reconciled");
    } else if (old) {
        if (p->pending) strcpy(p->reason,"unpublished_intent_cleared");
    } else {
        p->faulted=1; strcpy(p->reason,"routes_changed_reconfigure_required");
    }
    p->pending=0; memset(p->targets,0,sizeof p->targets);
    return !old && !target ? -1 : target ? 1 : 0;
}
#endif
