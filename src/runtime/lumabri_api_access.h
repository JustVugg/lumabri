/* Local operator-owned API identities. High-entropy bearer tokens are shown
 * once; only SHA-512 digests and exact approved allocation IDs are persisted.
 * This grants inference, never donor approval, loading, eviction or admin. */
#ifndef LMB_API_ACCESS_H
#define LMB_API_ACCESS_H
#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include <sys/file.h>
#include <sys/stat.h>

#define LMB_API_MODELS_MAX 64u
typedef struct {
    char name[33];
    uint8_t digest[64], allocations[LMB_API_MODELS_MAX][32];
    uint32_t count;
} LmbApiUser;

static inline void lmb_api_wipe(void *data, size_t n) {
    volatile unsigned char *p=data; while (n--) *p++=0;
}
static inline int lmb_api_username(const char *name) {
    size_t n=strnlen(name,33); if (!n || n>32) return 0;
    for (size_t i=0; i<n; i++) {
        unsigned c=(unsigned char)name[i];
        if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-')) return 0;
    }
    return 1;
}
static inline int lmb_api_private_fd(int fd, int directory) {
    struct stat s;
    return fstat(fd,&s) || s.st_uid!=geteuid() || (s.st_mode&077) ||
        (directory ? !S_ISDIR(s.st_mode) : !S_ISREG(s.st_mode) || s.st_nlink!=1) ? -1 : 0;
}
static inline int lmb_api_access_dir(const char *path, int create) {
    if (create && mkdir(path,0700) && errno!=EEXIST) return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    if (lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
/* Keep this lock across each read/modify/atomic-save operation. */
static inline int lmb_api_access_lock(int dir) {
    int fd=openat(dir,"access.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    if (lmb_api_private_fd(fd,0) || flock(fd,LOCK_EX|LOCK_NB)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_api_user_load(int dir, const char *name, LmbApiUser *user) {
    if (!lmb_api_username(name)) return -1;
    char path[40]; snprintf(path,sizeof path,"%s.user",name);
    int fd=openat(dir,path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    struct stat s; unsigned char bytes[8+64+4+LMB_API_MODELS_MAX*32];
    if (lmb_api_private_fd(fd,0) || fstat(fd,&s) || s.st_size<76 ||
        (uint64_t)s.st_size>sizeof bytes) { close(fd); return -1; }
    int bad=0;
    size_t n=(size_t)s.st_size, at=0;
    while (!bad && at<n) {
        ssize_t got=read(fd,bytes+at,n-at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0 || (size_t)got>n-at) bad=1; else at+=(size_t)got;
    }
    close(fd);
    if (bad || memcmp(bytes,"LMBUSER1",8)) return -1;
    uint32_t count=lmb_get32(bytes+72);
    if (count>LMB_API_MODELS_MAX || n!=76+(size_t)count*32) return -1;
    memset(user,0,sizeof *user); memcpy(user->name,name,strlen(name)+1);
    memcpy(user->digest,bytes+8,64); user->count=count;
    memcpy(user->allocations,bytes+76,(size_t)count*32);
    for (uint32_t i=0; i<count; i++) {
        unsigned nonzero=0;
        for (unsigned k=0; k<32; k++) nonzero|=user->allocations[i][k];
        if (!nonzero) return -1;
        for (uint32_t k=0; k<i; k++) if (!memcmp(user->allocations[i],user->allocations[k],32)) return -1;
    }
    return 0;
}
static inline int lmb_api_user_save(int dir, const LmbApiUser *user) {
    if (!lmb_api_username(user->name) || user->count>LMB_API_MODELS_MAX) return -1;
    for (uint32_t i=0; i<user->count; i++) {
        unsigned nonzero=0;
        for (unsigned k=0; k<32; k++) nonzero|=user->allocations[i][k];
        if (!nonzero) return -1;
        for (uint32_t k=0; k<i; k++) if (!memcmp(user->allocations[i],user->allocations[k],32)) return -1;
    }
    unsigned char bytes[8+64+4+LMB_API_MODELS_MAX*32]; size_t n=76+(size_t)user->count*32;
    memcpy(bytes,"LMBUSER1",8); memcpy(bytes+8,user->digest,64); lmb_put32(bytes+72,user->count);
    memcpy(bytes+76,user->allocations,(size_t)user->count*32);
    uint8_t random[16]; char hex[33], temporary[48], path[40];
    lmb_random(random,sizeof random); lmb_hex(hex,random,sizeof random);
    snprintf(temporary,sizeof temporary,".access-%s",hex);
    snprintf(path,sizeof path,"%s.user",user->name);
    int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    int bad=0; size_t at=0;
    while (!bad && at<n) {
        ssize_t wrote=write(fd,bytes+at,n-at);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) bad=1; else at+=(size_t)wrote;
    }
    if (!bad && fsync(fd)) bad=1;
    if (close(fd)) bad=1;
    if (!bad && renameat(dir,temporary,dir,path)) bad=1;
    if (bad) (void)unlinkat(dir,temporary,0);
    else if (fsync(dir)) bad=1;
    lmb_api_wipe(bytes,sizeof bytes);
    return bad ? -1 : 0;
}
static inline int lmb_api_user_create(int dir, const char *name, char token[98]) {
    if (!lmb_api_username(name)) return -1;
    char path[40]; snprintf(path,sizeof path,"%s.user",name);
    struct stat s;
    if (!fstatat(dir,path,&s,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) return -1;
    LmbApiUser user={0}; memcpy(user.name,name,strlen(name)+1);
    uint8_t random[32]; char hex[65]; lmb_random(random,sizeof random); lmb_hex(hex,random,sizeof random);
    snprintf(token,98,"%s.%s",name,hex); lmb_sha512(token,strlen(token),user.digest);
    lmb_api_wipe(random,sizeof random); lmb_api_wipe(hex,sizeof hex);
    int rc=lmb_api_user_save(dir,&user);
    if (rc) lmb_api_wipe(token,98);
    return rc;
}
static inline int lmb_api_authorize(int dir, const char *authorization, LmbApiUser *user) {
    if (strncmp(authorization,"Bearer ",7)) return -1;
    const char *token=authorization+7, *dot=strchr(token,'.');
    if (!dot || dot==token || dot-token>32 || strlen(dot+1)!=64) return -1;
    char name[33]; memcpy(name,token,(size_t)(dot-token)); name[dot-token]=0;
    uint8_t bytes[32], digest[64];
    if (!lmb_api_username(name) || lmb_unhex(bytes,dot+1,32)) return -1;
    lmb_sha512(token,strlen(token),digest);
    int bad=lmb_api_user_load(dir,name,user);
    unsigned different=0;
    if (!bad) for (unsigned i=0; i<64; i++) different|=digest[i]^user->digest[i];
    lmb_api_wipe(bytes,sizeof bytes); lmb_api_wipe(digest,sizeof digest);
    return bad || different ? -1 : 0;
}
static inline int lmb_api_user_allows(const LmbApiUser *user, const uint8_t allocation[32]) {
    for (uint32_t i=0; i<user->count; i++) if (!memcmp(user->allocations[i],allocation,32)) return 1;
    return 0;
}
/* A held descriptor is one in-flight request, released automatically at
 * process exit. Shared across API processes using the same access directory. */
static inline int lmb_api_user_permit(int dir, const char *name, unsigned limit) {
    if (!lmb_api_username(name) || !limit || limit>8) return -1;
    for (unsigned i=0; i<limit; i++) {
        char path[48]; snprintf(path,sizeof path,"%s.slot-%u",name,i);
        int fd=openat(dir,path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) return -1;
        if (lmb_api_private_fd(fd,0)) { close(fd); return -1; }
        if (!flock(fd,LOCK_EX|LOCK_NB)) return fd;
        int occupied=errno==EWOULDBLOCK || errno==EAGAIN; close(fd);
        if (!occupied) return -1;
    }
    return -1;
}
#endif
