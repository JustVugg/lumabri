/* Read-only cluster visibility is an explicit capability, separate from
 * inference grants and private conversation ownership. A recreated username
 * never inherits its predecessor's operator permission. */
#ifndef LMB_OPERATOR_ACCESS_H
#define LMB_OPERATOR_ACCESS_H
#include "lumabri_api_access.h"

static inline int lmb_operator_allows(int dir, const LmbApiUser *user) {
    if (!user || !lmb_api_username(user->name)) return 0;
    char file[48]; snprintf(file,sizeof file,"%s.operator",user->name);
    int fd=openat(dir,file,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return 0;
    unsigned char record[72]; struct stat st;
    int bad=lmb_api_private_fd(fd,0) || fstat(fd,&st) || st.st_size!=sizeof record ||
        lmb_read_full(fd,record,sizeof record);
    close(fd);
    unsigned mismatch=bad ? 1 : memcmp(record,"LMBOPER1",8)!=0;
    if (!bad) for (unsigned i=0;i<64;i++) mismatch|=record[8+i]^user->digest[i];
    lmb_api_wipe(record,sizeof record); return !mismatch;
}
/* Caller holds the access database lock. Fixed private transaction file is
 * replaceable after a crash, but never followed if unsafe. */
static inline int lmb_operator_set(int dir, const LmbApiUser *user, int enabled) {
    if (!user || !lmb_api_username(user->name)) return -1;
    char file[48]; snprintf(file,sizeof file,"%s.operator",user->name);
    if (!enabled) {
        if (unlinkat(dir,file,0) && errno!=ENOENT) return -1;
        return fsync(dir);
    }
    const char *temporary=".operator.pending";
    int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    unsigned char record[72]; memcpy(record,"LMBOPER1",8); memcpy(record+8,user->digest,64);
    int bad=lmb_api_private_fd(fd,0) || ftruncate(fd,0); size_t at=0;
    while (!bad && at<sizeof record) {
        ssize_t n=write(fd,record+at,sizeof record-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    if (!bad && fsync(fd)) bad=1;
    if (close(fd)) bad=1;
    lmb_api_wipe(record,sizeof record);
    if (!bad && renameat(dir,temporary,dir,file)) bad=1;
    if (!bad && fsync(dir)) bad=1;
    return bad ? -1 : 0;
}
#endif
