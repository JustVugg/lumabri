/* Exact, durable tracker metadata behind bounded in-memory caches. These
 * records are identity/integrity evidence, never weights or KV state. */
#ifndef LMB_TRACKER_HISTORY_H
#define LMB_TRACKER_HISTORY_H
#include "lumabri_journal_index.h"
#include "lumabri_sha.h"

typedef struct { int dir, lock; } TrackerHistory;

static void tracker_history_end(TrackerHistory *h) {
    if (h->lock>=0) close(h->lock);
    if (h->dir>=0) close(h->dir);
    h->dir=h->lock=-1;
}
static int tracker_history_begin(const char *bindings,TrackerHistory *h) {
    *h=(TrackerHistory){-1,-1};
    char path[1024];
    if (!bindings || !*bindings || snprintf(path,sizeof path,"%s.history",bindings)>=(int)sizeof path)
        return -1;
    if (mkdir(path,0700) && errno!=EEXIST) return -1;
    h->dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (h->dir<0 || lmb_journal_private(h->dir,1)) goto fail;
    /* Publication of the history directory must also survive a crash. */
    char parent[1024]; snprintf(parent,sizeof parent,"%s",path);
    char *slash=strrchr(parent,'/');
    if (slash) { if (slash==parent) slash[1]=0; else *slash=0; }
    else snprintf(parent,sizeof parent,".");
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) goto fail;
    int bad=fsync(fd); close(fd); if (bad) goto fail;
    h->lock=openat(h->dir,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if (h->lock<0 || lmb_journal_private(h->lock,0) || flock(h->lock,LOCK_EX)) goto fail;
    return 0;
fail:
    tracker_history_end(h); return -1;
}
static void tracker_history_key(const char *kind,const char *model,const char *path,char name[80]) {
    LmbSha sha; uint8_t digest[32]; lmb_sha_init(&sha);
    const char *parts[]={kind,model,path};
    for (unsigned i=0;i<3;i++) lmb_sha_update(&sha,parts[i],strlen(parts[i])+1);
    lmb_sha_final(&sha,digest); lmb_hex(name,digest,32); strcat(name,".entry");
}
static void tracker_history_digest(const char *name,const void *bytes,size_t size,uint8_t out[32]) {
    LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,name,strlen(name)+1);
    lmb_sha_update(&sha,bytes,size); lmb_sha_final(&sha,out);
}
/* 1 means absent. Malformed/unsafe evidence is never treated as a new key. */
static int tracker_history_read(TrackerHistory *h,const char *name,LmbBuf *out) {
    memset(out,0,sizeof *out);
    int fd=lmb_journal_open(h->dir,name,O_RDONLY);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat s; int bad=fstat(fd,&s) || s.st_size<40 || (uint64_t)s.st_size>LMB_MAX_BODY+2048u;
    uint8_t *bytes=bad ? NULL : malloc((size_t)s.st_size);
    if (!bytes) bad=1;
    if (!bad) bad=lmb_read_full(fd,bytes,(size_t)s.st_size);
    close(fd);
    if (!bad) {
        uint8_t expected[32]; tracker_history_digest(name,bytes,(size_t)s.st_size-32,expected);
        bad=memcmp(bytes,"LMB-TRH1",8) || memcmp(expected,bytes+s.st_size-32,32);
        if (!bad) bad=lmb_buf_bytes(out,bytes+8,(size_t)s.st_size-40);
    }
    free(bytes); if (bad) { free(out->p); memset(out,0,sizeof *out); return -1; }
    return 0;
}
/* Caller holds the store lock and has checked the previous exact record.
 * Atomic replacement only upgrades evidence (e.g. a newer signing key). */
static int tracker_history_write(TrackerHistory *h,const char *name,const LmbBuf *payload) {
    if (payload->len>LMB_MAX_BODY+2008u) return -1;
    int at=lmb_journal_select(h->dir,name,".entry",256);
    if (at<0) return -1;
    char pending[80]; snprintf(pending,sizeof pending,"%.64s.pending",name);
    struct stat s;
    int exists=!fstatat(at,pending,&s,AT_SYMLINK_NOFOLLOW);
    if ((exists && (!S_ISREG(s.st_mode) || s.st_uid!=geteuid() || (s.st_mode&077) || s.st_nlink!=1)) ||
        (!exists && errno!=ENOENT) || (exists && unlinkat(at,pending,0))) { close(at); return -1; }
    LmbBuf b={0}; uint8_t digest[32];
    int bad=lmb_buf_bytes(&b,"LMB-TRH1",8) || lmb_buf_bytes(&b,payload->p,payload->len);
    if (!bad) { tracker_history_digest(name,b.p,b.len,digest); bad=lmb_buf_bytes(&b,digest,32); }
    int fd=bad ? -1 : openat(at,pending,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    else {
        size_t offset=0;
        while (!bad && offset<b.len) {
            ssize_t n=write(fd,b.p+offset,b.len-offset);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) bad=1; else offset+=(size_t)n;
        }
        if (!bad) bad=fsync(fd);
        if (close(fd)) bad=1;
        if (!bad) bad=renameat(at,pending,at,name) || fsync(at);
    }
    free(b.p); close(at); return bad ? -1 : 0;
}
#endif
