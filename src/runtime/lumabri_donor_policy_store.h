/* Private donor-owner policy store. Reads never mutate owner authority;
 * grant/revoke use revision fences and one local kernel lock. */
#ifndef LMB_DONOR_POLICY_STORE_H
#define LMB_DONOR_POLICY_STORE_H
#include "lumabri_donor_policy.h"
#include <sys/file.h>
#include <dirent.h>
static inline int lmb_donor_policy_private(int fd,int directory) {
    struct stat st;
    return fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&077) ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode) || st.st_nlink>1) ? -1 : 0;
}
static inline int lmb_donor_policy_dir(const char *service_directory,int create) {
    int base=open(service_directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (base<0) return -1;
    int fd=-1;
    if (!lmb_donor_policy_private(base,1) &&
        (!create || !mkdirat(base,"donor-policies",0700) || errno==EEXIST))
        fd=openat(base,"donor-policies",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    close(base);
    if (fd>=0 && lmb_donor_policy_private(fd,1)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_donor_policy_lock(int dir) {
    int fd=openat(dir,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    struct stat st;
    if (lmb_donor_policy_private(fd,0) || fstat(fd,&st) || st.st_nlink!=1 || flock(fd,LOCK_EX|LOCK_NB)) {
        close(fd); return -1;
    }
    return fd;
}
static inline int lmb_donor_policy_load(int dir,const uint8_t id[32],LmbDonorPolicy *p) {
    char name[80],hex[65]; lmb_hex(hex,id,32); snprintf(name,sizeof name,"%s.policy",hex);
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat st; uint8_t bytes[LMB_DONOR_POLICY_BYTES];
    int bad=lmb_donor_policy_private(fd,0) || fstat(fd,&st) || st.st_size<100 || st.st_size>(off_t)sizeof bytes;
    if (!bad) bad=lmb_read_full(fd,bytes,(size_t)st.st_size) || lmb_donor_policy_decode(bytes,(size_t)st.st_size,p) || memcmp(id,p->id,32);
    close(fd); return bad ? -1 : 0;
}
static inline int lmb_donor_policy_store(int dir,const LmbDonorPolicy *p,uint32_t expected) {
    LmbDonorPolicy old; int found=lmb_donor_policy_load(dir,p->id,&old);
    if (found<0 || (found ? expected!=0 : old.revision!=expected) || expected==UINT32_MAX || p->revision!=expected+1) return -1;
    if (found) {
        int fd=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC); DIR *scan=fd<0 ? NULL : fdopendir(fd);
        if (!scan) { if (fd>=0) close(fd); return -1; }
        unsigned count=0; struct dirent *e; int bad=0;
        for (;;) {
            errno=0; e=readdir(scan); if (!e) { bad=errno!=0; break; }
            if (strlen(e->d_name)==71 && !strcmp(e->d_name+64,".policy")) count++;
        }
        closedir(scan); if (bad || count>=LMB_DONOR_POLICY_MAX) return -1;
    }
    LmbBuf b={0}; if (lmb_donor_policy_encode(p,&b)) { free(b.p); return -1; }
    struct stat st; int bad=0;
    if (!fstatat(dir,"pending",&st,AT_SYMLINK_NOFOLLOW)) {
        bad=!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 || unlinkat(dir,"pending",0);
    } else if (errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,"pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    size_t at=0;
    while (!bad && at<b.len) {
        ssize_t n=write(fd,b.p+at,b.len-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    if (!bad) bad=fsync(fd);
    if (fd>=0 && close(fd)) bad=1;
    char name[80],hex[65]; lmb_hex(hex,p->id,32); snprintf(name,sizeof name,"%s.policy",hex);
    if (!bad) bad=renameat(dir,"pending",dir,name) || fsync(dir);
    free(b.p); return bad ? -1 : 0;
}
#endif
