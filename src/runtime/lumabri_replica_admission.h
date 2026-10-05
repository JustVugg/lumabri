/* Bounded same-operator gateway turn admission, keyed by exact allocation.
 * Kernel locks, not saved PIDs/counters, own capacity and retire on process
 * death. Fixed cells avoid unbounded files as approved allocations change.
 * The engine and machine-wide compute broker remain the final arbiters. */
#ifndef LMB_REPLICA_ADMISSION_H
#define LMB_REPLICA_ADMISSION_H
#include "lumabri_api_access.h"

#define LMB_REPLICA_CELLS 32u
enum { LMB_REPLICA_OK=0, LMB_REPLICA_BUSY=1, LMB_REPLICA_UNSAFE=-1 };

static inline int lmb_replica_admit(int access, const uint8_t allocation[32], int *permit) {
    *permit=-1;
    if (mkdirat(access,"dispatch",0700) && errno!=EEXIST) return LMB_REPLICA_UNSAFE;
    int dir=openat(access,"dispatch",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir<0) return LMB_REPLICA_UNSAFE;
    if (lmb_api_private_fd(dir,1)) { close(dir); return LMB_REPLICA_UNSAFE; }
    int guard=lmb_api_access_lock(dir);
    for (unsigned attempt=0; guard<0 && (errno==EWOULDBLOCK || errno==EAGAIN) && attempt<20; attempt++) {
        (void)poll(NULL,0,5); guard=lmb_api_access_lock(dir);
    }
    if (guard<0) { int busy=errno==EWOULDBLOCK || errno==EAGAIN; close(dir); return busy ? LMB_REPLICA_BUSY : LMB_REPLICA_UNSAFE; }
    int candidate=-1, result=LMB_REPLICA_BUSY;
    for (unsigned i=0; i<LMB_REPLICA_CELLS; i++) {
        char name[16]; snprintf(name,sizeof name,"%u.lease",i);
        int fd=openat(dir,name,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) { result=LMB_REPLICA_UNSAFE; goto done; }
        struct stat st;
        if (lmb_api_private_fd(fd,0) || fstat(fd,&st)) { close(fd); result=LMB_REPLICA_UNSAFE; goto done; }
        uint8_t saved[32]; int same=0;
        if (st.st_size==32) {
            if (lmb_read_full(fd,saved,sizeof saved)) { close(fd); result=LMB_REPLICA_UNSAFE; goto done; }
            same=!memcmp(saved,allocation,32);
        }
        if (flock(fd,LOCK_EX|LOCK_NB)) {
            int busy=errno==EWOULDBLOCK || errno==EAGAIN; close(fd);
            if (!busy || st.st_size!=32) { result=LMB_REPLICA_UNSAFE; goto done; }
            if (same) goto done; /* same allocation through another alias */
            continue;
        }
        /* An unlocked partial record may be left by a killed writer. Its
         * bytes are only a hint: no capacity is restored from stale state. */
        if (same) { if (candidate>=0) close(candidate); candidate=fd; break; }
        if (candidate<0) candidate=fd; else close(fd);
    }
    if (candidate>=0) {
        size_t at=0;
        while (at<32) {
            ssize_t n=pwrite(candidate,allocation+at,32-at,(off_t)at);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { result=LMB_REPLICA_UNSAFE; goto done; }
            at+=(size_t)n;
        }
        if (ftruncate(candidate,32)) { result=LMB_REPLICA_UNSAFE; goto done; }
        *permit=candidate; candidate=-1; result=LMB_REPLICA_OK;
    }
done:
    if (candidate>=0) close(candidate);
    close(guard); close(dir); return result;
}
#endif
