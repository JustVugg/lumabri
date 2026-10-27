/* Immutable birth record plus a kernel-held lifetime fence. Only the creator
 * may acquire the shared description; it is inherited by every owned engine
 * and descendant. No code unlocks it: the last close proves the entire owned
 * process tree is gone. A fresh exclusive description can then attest exit,
 * including after a keeper crash. Missing/corrupt records prove nothing.
 * Files are never replaced, removed or reopened for another lifetime. */
#ifndef LMB_ALLOCATION_LIFETIME_H
#define LMB_ALLOCATION_LIFETIME_H
#include "lumabri_retired_receipts.h"
#define LMB_LIFETIME_BYTES 188u
#define LMB_LIFETIME_MAX 4096u
typedef struct {
    uint8_t allocation[32], root[32], requester[32], offer_hash[64];
    uint64_t reserved_bytes;
    uint32_t edge, begin, end;
} LmbAllocationLifetime;

static int lmb_lifetime_valid(const LmbAllocationLifetime *r) {
    uint8_t id=0,root=0,owner=0,hash=0;
    for (unsigned i=0;i<32;i++) { id|=r->allocation[i]; root|=r->root[i]; owner|=r->requester[i]; }
    for (unsigned i=0;i<64;i++) hash|=r->offer_hash[i];
    return id && root && owner && hash && r->reserved_bytes && r->reserved_bytes<=(UINT64_C(1)<<50) &&
        r->edge<=1 && r->begin<r->end;
}
static int lmb_lifetime_encode(const LmbAllocationLifetime *r,uint8_t b[LMB_LIFETIME_BYTES]) {
    if (!lmb_lifetime_valid(r)) return -1;
    memcpy(b,"LMBLIF1\0",8); memcpy(b+8,r->allocation,32); memcpy(b+40,r->root,32);
    memcpy(b+72,r->requester,32); memcpy(b+104,r->offer_hash,64);
    lmb_put32(b+168,(uint32_t)(r->reserved_bytes>>32)); lmb_put32(b+172,(uint32_t)r->reserved_bytes);
    lmb_put32(b+176,r->edge); lmb_put32(b+180,r->begin); lmb_put32(b+184,r->end); return 0;
}
static int lmb_lifetime_decode(const uint8_t *b,size_t size,LmbAllocationLifetime *r) {
    memset(r,0,sizeof *r);
    if (size!=LMB_LIFETIME_BYTES || memcmp(b,"LMBLIF1\0",8)) return -1;
    memcpy(r->allocation,b+8,32); memcpy(r->root,b+40,32); memcpy(r->requester,b+72,32);
    memcpy(r->offer_hash,b+104,64); r->reserved_bytes=((uint64_t)lmb_get32(b+168)<<32)|lmb_get32(b+172);
    r->edge=lmb_get32(b+176); r->begin=lmb_get32(b+180); r->end=lmb_get32(b+184);
    return lmb_lifetime_valid(r) ? 0 : -1;
}
static void lmb_lifetime_name(const uint8_t id[32],char name[80]) {
    char hex[65]; lmb_hex(hex,id,32); snprintf(name,80,"%s.lifetime",hex);
}
static int lmb_lifetime_dir(const char *service,int create) {
    int base=open(service,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (base<0) return -1;
    if (lmb_retired_private(base,1)) { close(base); errno=EACCES; return -1; }
    int dir=-1;
    if (!create || !mkdirat(base,"allocation-lifetimes",0700) || errno==EEXIST)
        dir=openat(base,"allocation-lifetimes",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    int saved=errno; close(base); errno=saved;
    if (dir>=0 && lmb_retired_private(dir,1)) { close(dir); errno=EACCES; return -1; }
    return dir;
}
/* Also rejects an interrupted birth write: that identity may never reload. */
static int lmb_lifetime_unused(int dir,const uint8_t id[32]) {
    char name[80]; lmb_lifetime_name(id,name);
    int at=lmb_journal_location(dir,name);
    if (at>=0) { close(at); return 0; }
    return errno==ENOENT ? 1 : -1;
}
/* Returns an owned CLOEXEC descriptor >=3. Hold it before any allocation
 * fork. Only the chosen child's copy has CLOEXEC cleared; unrelated models
 * must not keep each other's lifetimes alive. Caller closes, NEVER LOCK_UN. */
static int lmb_lifetime_create(int dir,const LmbAllocationLifetime *r) {
    uint8_t bytes[LMB_LIFETIME_BYTES]; char name[80];
    if (lmb_lifetime_encode(r,bytes)) return -1;
    int lock=lmb_retired_lock(dir),fd=-1,bad=1,target=-1;
    if (lock<0 || lmb_lifetime_unused(dir,r->allocation)!=1) goto done;
    lmb_lifetime_name(r->allocation,name);
    target=lmb_journal_select(dir,name,".lifetime",LMB_LIFETIME_MAX);
    if (target<0) goto done;
    fd=openat(target,name,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if (fd<0) goto done;
    if (fd<3) { int copy=fcntl(fd,F_DUPFD_CLOEXEC,3); close(fd); fd=copy; }
    if (fd<0 || lmb_retired_private(fd,0) || flock(fd,LOCK_SH|LOCK_NB)) goto done;
    size_t at=0;
    while (at<sizeof bytes) {
        ssize_t n=write(fd,bytes+at,sizeof bytes-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) goto done;
        at+=(size_t)n;
    }
    if (fsync(fd) || fsync(target)) goto done;
    bad=0;
done:
    if (target>=0) close(target);
    if (lock>=0) close(lock);
    if (bad && fd>=0) { close(fd); fd=-1; }
    return fd;
}
/* 0 = all inherited lifetime holders exited; 1 = still held; -1 = unknown.
 * The exclusive proof uses a NEW description, never an upgrade of the
 * keeper's shared one (an upgrade could unlock its descendants too). */
static int lmb_lifetime_prove(int dir,const uint8_t id[32],LmbAllocationLifetime *r) {
    char name[80]; lmb_lifetime_name(id,name); memset(r,0,sizeof *r);
    int target=lmb_journal_location(dir,name);
    if (target<0) return -1;
    int fd=openat(target,name,O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) { close(target); return -1; }
    int rc=-1; struct stat st; uint8_t b[LMB_LIFETIME_BYTES];
    if (lmb_retired_private(fd,0)) goto done;
    if (flock(fd,LOCK_EX|LOCK_NB)) {
        if (errno==EWOULDBLOCK || errno==EAGAIN) rc=1;
        goto done;
    }
    if (fstat(fd,&st) || st.st_size!=(off_t)sizeof b || lmb_read_full(fd,b,sizeof b) ||
        lmb_lifetime_decode(b,sizeof b,r) || memcmp(id,r->allocation,32) || fsync(fd) || fsync(target)) goto done;
    rc=0;
done:
    close(fd); close(target); return rc;
}
#endif
