/* Owner-private operation receipts. An immutable intent is written before
 * starting a keeper. Interrupted intents are NEVER automatically replayed. */
#ifndef LMB_PREPARATION_STORE_H
#define LMB_PREPARATION_STORE_H
#include "lumabri_preparation_request.h"
static int preparation_dir(int access) {
    if (mkdirat(access,"preparations",0700) && errno!=EEXIST) return -1;
    int fd=openat(access,"preparations",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static void preparation_digest(const void *p,size_t n,uint8_t out[32]) {
    LmbSha sha; lmb_sha_init(&sha); lmb_sha_update(&sha,p,n); lmb_sha_final(&sha,out);
}
static int preparation_write(int fd,const void *data,size_t size) {
    const uint8_t *p=data;
    while (size) {
        ssize_t n=write(fd,p,size);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        p+=(size_t)n; size-=(size_t)n;
    }
    return 0;
}
static int preparation_read(int dir,const char *name,void *bytes,size_t cap,size_t *size) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat st;
    /* A concurrently replaced, already opened snapshot may have nlink=0. */
    int bad=fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) ||
        st.st_nlink>1 || st.st_size<=0 || (uint64_t)st.st_size>cap;
    if (!bad) { *size=(size_t)st.st_size; bad=lmb_read_full(fd,bytes,*size); }
    close(fd); return bad ? -1 : 0;
}
static int preparation_intent(int dir,const char *id,const uint8_t digest[32],int create) {
    char name[80]; snprintf(name,sizeof name,"%s.intent",id);
    uint8_t old[32]; size_t n=0;
    int rc=preparation_read(dir,name,old,sizeof old,&n);
    if (!rc) return n==32 && !memcmp(old,digest,32) ? 1 : -1;
    if (rc<0 || !create) return -1;
    /* Bound permanent receipts without forgetting an idempotency key. */
    int scan=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC); DIR *d=scan<0 ? NULL : fdopendir(scan);
    if (!d) { if (scan>=0) close(scan); return -1; }
    unsigned count=0; struct dirent *e;
    errno=0;
    while ((e=readdir(d))) { if (strstr(e->d_name,".intent")) count++; }
    int bad=errno!=0 || count>=256; closedir(d);
    if (bad) return -1;
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    bad=preparation_write(fd,digest,32) || fsync(fd);
    if (close(fd)) bad=1;
    if (!bad) bad=fsync(dir);
    /* Preserve even a damaged intent: uncertainty is not permission to retry. */
    return bad ? -1 : 0;
}
static int preparation_save(int dir,const HomeServiceSnapshot *s) {
    LmbBuf b={0}; if (home_service_pack(&b,s)) { free(b.p); return -1; }
    uint8_t digest[32]; preparation_digest(b.p,b.len,digest);
    if (lmb_buf_bytes(&b,digest,32)) { free(b.p); return -1; }
    char id[65],name[80],tmp[80]; lmb_hex(id,s->instance,32);
    snprintf(name,sizeof name,"%s.state",id); snprintf(tmp,sizeof tmp,"%s.pending",id);
    /* Exactly one keeper writes an operation. Leftover pending is bounded
     * per ID; it can never be followed through a symlink. */
    struct stat st; int bad=0;
    if (!fstatat(dir,tmp,&st,AT_SYMLINK_NOFOLLOW)) {
        bad=!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 || unlinkat(dir,tmp,0);
    } else if (errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    else {
        bad=preparation_write(fd,b.p,b.len) || fsync(fd);
        if (close(fd)) bad=1;
        if (!bad) bad=renameat(dir,tmp,dir,name) || fsync(dir);
    }
    free(b.p); return bad ? -1 : 0;
}
static int preparation_load(int dir,const char *id,HomeServiceSnapshot *s) {
    char name[80]; snprintf(name,sizeof name,"%s.state",id);
    uint8_t bytes[HOME_SVC_MAX+32],hash[32],expected[32]; size_t size=0;
    if (preparation_read(dir,name,bytes,sizeof bytes,&size) || size<33 || lmb_unhex(expected,id,32)) return -1;
    preparation_digest(bytes,size-32,hash);
    return memcmp(hash,bytes+size-32,32) || home_service_unpack(bytes,size-32,s) ||
        memcmp(s->instance,expected,32) || strcmp(s->role,"prepare") ? -1 : 0;
}
#endif
