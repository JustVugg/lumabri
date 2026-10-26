/* A sealed allocation's terminal acknowledgement outlives its keeper slot.
 * PREPARED is only an intent: it never proves release. DONE is written after
 * the owning keeper has stopped its children and returned its reservation.
 * Neither a missing receipt nor an unreachable process is evidence of exit.
 * Callers hold the directory lock for every store. Records are never evicted
 * or reused. History is indexed after the legacy flat namespace fills. */
#ifndef LMB_RETIRED_RECEIPTS_H
#define LMB_RETIRED_RECEIPTS_H
#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_node_control.h"
#include "lumabri_journal_index.h"
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#define LMB_RETIRED_RECEIPTS_MAX 256u
#define LMB_RETIRED_RECEIPT_BYTES (108u+LMB_NODE_CONTROL_REPLY_BYTES)
enum { LMB_RECEIPT_PREPARED=1, LMB_RECEIPT_DONE=2 };
typedef struct {
    uint8_t allocation[32], root[32], requester[32];
    uint32_t phase;
    LmbNodeControl seal;
} LmbRetiredReceipt;

static int lmb_retired_valid(const LmbRetiredReceipt *r) {
    uint8_t id=0,root=0,owner=0;
    for (unsigned i=0;i<32;i++) { id|=r->allocation[i]; root|=r->root[i]; owner|=r->requester[i]; }
    uint8_t bytes[LMB_NODE_CONTROL_REPLY_BYTES]; LmbNodeControl decoded;
    lmb_node_control_reply(bytes,0,&r->seal);
    return id && root && owner && (r->phase==LMB_RECEIPT_PREPARED || r->phase==LMB_RECEIPT_DONE) &&
        !lmb_node_control_decode(bytes,sizeof bytes,&decoded) &&
        r->seal.draining==LMB_NODE_CONTROL_RETIRED && !r->seal.sessions && !r->seal.experts;
}
static int lmb_retired_encode(const LmbRetiredReceipt *r, uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES]) {
    if (!lmb_retired_valid(r)) return -1;
    memcpy(bytes,"LMBREL1\0",8); memcpy(bytes+8,r->allocation,32);
    memcpy(bytes+40,r->root,32); memcpy(bytes+72,r->requester,32);
    lmb_put32(bytes+104,r->phase); lmb_node_control_reply(bytes+108,0,&r->seal); return 0;
}
static int lmb_retired_decode(const uint8_t *bytes, size_t size, LmbRetiredReceipt *r) {
    memset(r,0,sizeof *r);
    if (size!=LMB_RETIRED_RECEIPT_BYTES || memcmp(bytes,"LMBREL1\0",8)) return -1;
    memcpy(r->allocation,bytes+8,32); memcpy(r->root,bytes+40,32); memcpy(r->requester,bytes+72,32);
    r->phase=lmb_get32(bytes+104);
    return lmb_node_control_decode(bytes+108,LMB_NODE_CONTROL_REPLY_BYTES,&r->seal) ||
        !lmb_retired_valid(r) ? -1 : 0;
}
static int lmb_retired_private(int fd, int directory) {
    struct stat s;
    return fstat(fd,&s) || s.st_uid!=geteuid() || (s.st_mode&077) ||
        (directory ? !S_ISDIR(s.st_mode) : !S_ISREG(s.st_mode) || s.st_nlink!=1) ? -1 : 0;
}
/* A read-only lookup never creates a receipt directory. ENOENT means no
 * evidence, not a released allocation. The service parent must be private. */
static int lmb_retired_dir(const char *service_directory, int create) {
    int base=open(service_directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (base<0) return -1;
    if (lmb_retired_private(base,1)) { close(base); errno=EACCES; return -1; }
    int fd=-1;
    if (!create || !mkdirat(base,"retired-allocations",0700) || errno==EEXIST)
        fd=openat(base,"retired-allocations",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int saved=errno; close(base); errno=saved;
    if (fd>=0 && lmb_retired_private(fd,1)) { close(fd); errno=EACCES; return -1; }
    return fd;
}
static int lmb_retired_lock(int dir) {
    int fd=-1;
    for (unsigned attempt=0;attempt<4;attempt++) {
        fd=openat(dir,"lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if (fd>=0 || errno!=ENOENT) break;
        fd=openat(dir,"lock",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd>=0 && (lmb_retired_private(fd,0) || flock(fd,LOCK_EX|LOCK_NB))) { close(fd); return -1; }
    return fd;
}
static void lmb_retired_name(const uint8_t id[32], char name[80]) {
    char hex[65]; lmb_hex(hex,id,32); snprintf(name,80,"%s.release",hex);
}
/* 1 = no record, 0 = decoded record (possibly only PREPARED), -1 = unsafe. */
static int lmb_retired_load(int dir, const uint8_t id[32], LmbRetiredReceipt *out) {
    char name[80]; lmb_retired_name(id,name);
    int fd=lmb_journal_open(dir,name,O_RDONLY);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat s; uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES];
    int bad=lmb_retired_private(fd,0) || fstat(fd,&s) || s.st_size!=(off_t)sizeof bytes ||
        lmb_read_full(fd,bytes,sizeof bytes) || lmb_retired_decode(bytes,sizeof bytes,out) ||
        memcmp(out->allocation,id,32);
    close(fd); return bad ? -1 : 0;
}
/* A previous writer may have completed rename but lost the directory fsync.
 * Re-establish persistence before replaying an acknowledgement. */
static int lmb_retired_sync(int dir, const uint8_t id[32]) {
    char name[80]; lmb_retired_name(id,name);
    int at=lmb_journal_location(dir,name);
    if (at<0) return -1;
    int fd=openat(at,name,O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    int bad=fd<0 || lmb_retired_private(fd,0) || fsync(fd) || fsync(at);
    if (fd>=0) close(fd);
    close(at); return bad ? -1 : 0;
}
static int lmb_retired_store(int dir, const LmbRetiredReceipt *r) {
    uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES],prior[LMB_RETIRED_RECEIPT_BYTES]; LmbRetiredReceipt old;
    if (lmb_retired_encode(r,bytes)) return -1;
    int found=lmb_retired_load(dir,r->allocation,&old);
    if (found<0) return -1;
    if (!found) {
        if (old.phase>r->phase) return -1; /* DONE cannot become an intent. */
        old.phase=r->phase;
        if (lmb_retired_encode(&old,prior) || memcmp(bytes,prior,sizeof bytes)) return -1;
    } else if (r->phase!=LMB_RECEIPT_PREPARED) return -1;
    char name[80]; lmb_retired_name(r->allocation,name);
    int target=lmb_journal_select(dir,name,".release",LMB_RETIRED_RECEIPTS_MAX);
    if (target<0) return -1;
    dir=target;
    struct stat st; int bad=0;
    if (!fstatat(dir,"pending",&st,AT_SYMLINK_NOFOLLOW)) {
        bad=!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 || unlinkat(dir,"pending",0);
    } else if (errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,"pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    size_t at=0;
    while (!bad && at<sizeof bytes) {
        ssize_t n=write(fd,bytes+at,sizeof bytes-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    if (!bad && fsync(fd)) bad=1;
    if (fd>=0 && close(fd)) bad=1;
    if (!bad && renameat(dir,"pending",dir,name)) bad=1;
    if (!bad && fsync(dir)) bad=1;
    close(target);
    return bad ? -1 : 0;
}
#endif
