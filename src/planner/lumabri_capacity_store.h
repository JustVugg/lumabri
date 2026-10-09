/* Operator-owned bounded probe records. Not an import endpoint; only the
 * local real inference probe produces them. Checksums detect corruption,
 * not a malicious owner. No prompt, reply or credential is persisted. */
#ifndef LMB_CAPACITY_STORE_H
#define LMB_CAPACITY_STORE_H
#include "lumabri_capacity.h"
#include "src/runtime/lumabri_api_access.h"
#include <dirent.h>
#define LMB_CAPACITY_RECORDS 32u
static inline int lmb_capacity_write(int fd,const void *data,size_t n) {
    const unsigned char *p=data;
    while (n) {
        ssize_t wrote=write(fd,p,n);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) return -1;
        p+=wrote; n-=(size_t)wrote;
    }
    return 0;
}
static inline int lmb_capacity_encode(const LmbCapacity *r,LmbBuf *out) {
    if (!out || out->len || !lmb_capacity_valid(r)) return -1;
    LmbBuf b={0}; uint8_t digest[32]; LmbSha sha;
#define PUT(call) do { if (call) goto bad; } while (0)
    PUT(lmb_buf_bytes(&b,"LMBCAP01",8)); PUT(lmb_buf_bytes(&b,r->fingerprint,32));
    PUT(lmb_buf_u32(&b,r->models)); PUT(lmb_buf_u32(&b,r->clients)); PUT(lmb_buf_u32(&b,r->rounds));
    PUT(lmb_buf_u32(&b,r->max_new)); PUT(lmb_buf_u32(&b,r->count)); PUT(lmb_buf_u32(&b,r->stable));
    PUT(lmb_buf_u64(&b,r->measured_at));
    for (uint32_t i=0;i<r->models;i++) {
        PUT(lmb_buf_bytes(&b,r->allocations[i],32)); PUT(lmb_buf_bytes(&b,r->contents[i],32));
        PUT(lmb_buf_bytes(&b,r->contracts[i],32));
        PUT(lmb_buf_u32(&b,r->numeric_abi[i])); PUT(lmb_buf_str(&b,r->numeric_class[i]));
    }
    for (uint32_t i=0;i<r->count;i++) {
        const LmbCapacitySample *s=&r->samples[i];
        PUT(lmb_buf_u32(&b,s->model)); PUT(lmb_buf_u32(&b,s->round)); PUT(lmb_buf_u32(&b,s->status));
        PUT(lmb_buf_u32(&b,s->complete)); PUT(lmb_buf_u32(&b,s->timing_known));
        PUT(lmb_buf_u32(&b,s->prompt_tokens)); PUT(lmb_buf_u32(&b,s->generated_tokens));
        PUT(lmb_cal_put_double(&b,s->ttft)); PUT(lmb_cal_put_double(&b,s->gap_p95)); PUT(lmb_cal_put_double(&b,s->completion));
    }
    lmb_sha_init(&sha); lmb_sha_update(&sha,b.p,b.len); lmb_sha_final(&sha,digest);
    PUT(lmb_buf_bytes(&b,digest,32)); if (b.len>LMB_CAPACITY_BYTES) goto bad;
    free(out->p); *out=b; return 0;
bad:
    free(b.p); return -1;
#undef PUT
}
static inline int lmb_capacity_decode(const void *bytes,size_t n,LmbCapacity *out) {
    memset(out,0,sizeof *out);
    if (!bytes || n<104 || n>LMB_CAPACITY_BYTES || memcmp(bytes,"LMBCAP01",8)) return -1;
    uint8_t digest[32]; LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,bytes,n-32); lmb_sha_final(&sha,digest);
    if (memcmp(digest,(const uint8_t *)bytes+n-32,32)) return -1;
    LmbCur c={bytes,n-32,8}; LmbCapacity r={0};
#define GET(call) do { if (call) return -1; } while (0)
    GET(lmb_cur_bytes(&c,r.fingerprint,32)); GET(lmb_cur_u32(&c,&r.models)); GET(lmb_cur_u32(&c,&r.clients));
    GET(lmb_cur_u32(&c,&r.rounds)); GET(lmb_cur_u32(&c,&r.max_new)); GET(lmb_cur_u32(&c,&r.count));
    GET(lmb_cur_u32(&c,&r.stable)); GET(lmb_cur_u64(&c,&r.measured_at));
    if (r.models>LMB_CAPACITY_MODELS || r.count>LMB_CAPACITY_SAMPLES) return -1;
    for (uint32_t i=0;i<r.models;i++) {
        GET(lmb_cur_bytes(&c,r.allocations[i],32)); GET(lmb_cur_bytes(&c,r.contents[i],32)); GET(lmb_cur_bytes(&c,r.contracts[i],32));
        GET(lmb_cur_u32(&c,&r.numeric_abi[i])); GET(lmb_cal_get_text(&c,r.numeric_class[i],sizeof r.numeric_class[i]));
    }
    for (uint32_t i=0;i<r.count;i++) {
        LmbCapacitySample *s=&r.samples[i];
        GET(lmb_cur_u32(&c,&s->model)); GET(lmb_cur_u32(&c,&s->round)); GET(lmb_cur_u32(&c,&s->status));
        GET(lmb_cur_u32(&c,&s->complete)); GET(lmb_cur_u32(&c,&s->timing_known));
        GET(lmb_cur_u32(&c,&s->prompt_tokens)); GET(lmb_cur_u32(&c,&s->generated_tokens));
        GET(lmb_cal_get_double(&c,&s->ttft)); GET(lmb_cal_get_double(&c,&s->gap_p95)); GET(lmb_cal_get_double(&c,&s->completion));
    }
    if (c.off!=c.len || !lmb_capacity_valid(&r)) return -1;
    *out=r; return 0;
#undef GET
}
static inline int lmb_capacity_dir(int access,int create) {
    if (create && mkdirat(access,"capacity",0700) && errno!=EEXIST) return -1;
    int fd=openat(access,"capacity",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_capacity_load(int dir,const char *name,LmbCapacity *out) {
    memset(out,0,sizeof *out); if (!lmb_api_username(name)) return -1;
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); if (fd<0) return -1;
    struct stat st; int rc=-1;
    if (!lmb_api_private_fd(fd,0) && !fstat(fd,&st) && st.st_size>=104 && st.st_size<=LMB_CAPACITY_BYTES) {
        unsigned char bytes[LMB_CAPACITY_BYTES],extra;
        if (!lmb_read_full(fd,bytes,(size_t)st.st_size) && read(fd,&extra,1)==0)
            rc=lmb_capacity_decode(bytes,(size_t)st.st_size,out);
    }
    close(fd); return rc;
}
/* Hold a dedicated probe lock for the entire run. This prevents concurrent
 * calibrations from contaminating each other but never locks inference. */
static inline int lmb_capacity_lock(int dir) {
    int fd=openat(dir,"probe.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if (fd>=0 && (lmb_api_private_fd(fd,0) || flock(fd,LOCK_EX|LOCK_NB))) { close(fd); return -1; }
    return fd;
}
static inline int lmb_capacity_room(int dir,const char *name) {
    if (!lmb_api_username(name)) return -1;
    /* Names are immutable: a cancelled replacement must not leave yesterday's
     * successful profile looking like the result of today's attempt. */
    struct stat st;
    if (!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) return -1;
    int scan=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC); if (scan<0) return -1;
    DIR *entries=fdopendir(scan); if (!entries) { close(scan); return -1; }
    unsigned count=0; struct dirent *item;
    errno=0;
    while ((item=readdir(entries))) if (lmb_api_username(item->d_name)) count++;
    int failed=errno!=0; closedir(entries); if (failed || count>=LMB_CAPACITY_RECORDS) return -1;
    return 0;
}
static inline int lmb_capacity_save(int dir,const char *name,const LmbCapacity *r) {
    if (lmb_capacity_room(dir,name)) return -1;
    LmbBuf b={0}; if (lmb_capacity_encode(r,&b)) return -1;
    struct stat st;
    if (!fstatat(dir,".pending",&st,AT_SYMLINK_NOFOLLOW)) {
        /* A crash after linkat but before unlink leaves two links. Removing
         * only the private transaction link repairs publication without ever
         * truncating the immutable target (or following a symlink). */
        if (!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink<1 || st.st_nlink>2 ||
            unlinkat(dir,".pending",0)) { free(b.p); return -1; }
    } else if (errno!=ENOENT) { free(b.p); return -1; }
    int fd=openat(dir,".pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    int bad=fd<0;
    if (!bad) bad=lmb_capacity_write(fd,b.p,b.len) || fsync(fd);
    if (fd>=0 && close(fd)) bad=1;
    free(b.p);
    /* linkat publishes without replacing an existing immutable name. */
    if (!bad && linkat(dir,".pending",dir,name,0)) bad=1;
    if (unlinkat(dir,".pending",0)) bad=1;
    if (!bad && fsync(dir)) bad=1;
    return bad ? -1 : 0;
}
static inline int lmb_capacity_remove(int dir,const char *name) {
    LmbCapacity record;
    if (lmb_capacity_load(dir,name,&record)) return -1;
    return unlinkat(dir,name,0) || fsync(dir) ? -1 : 0;
}
#endif
