#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "lumabri_proto.h"
#include <stdarg.h>
#include <assert.h>
/* Exercise a cold-open race: the competing creator
 * wins before our exclusive create. The next lookup must reuse its inode.
 * Nonexclusive O_CREAT deliberately cannot hide that race in this test. */
static int inject_create_race;
static unsigned combined_lock_opens;
static int lock_openat(int dir, const char *path, int flags, ...) {
    mode_t mode=0;
    if (flags&O_CREAT) { va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap); }
    if (!strcmp(path,"access.lock") && (flags&O_CREAT)) {
        if (!(flags&O_EXCL)) {
            combined_lock_opens++;
            if (inject_create_race) { errno=ENOENT; return -1; }
        } else if (inject_create_race) {
            int fd=openat(dir,path,flags,mode); assert(fd>=0); close(fd);
            inject_create_race=0; errno=EEXIST; return -1;
        }
    }
    return openat(dir,path,flags,mode);
}
/* Successful cleanup may change errno (including through the gateway's
 * transport close hook). Make that deterministic on every CI platform. */
static int cleanup_close(int fd) {
    int rc=close(fd);
    if (!rc) errno=EIO;
    return rc;
}
#define close(fd) cleanup_close(fd)
#define openat lock_openat
#include "src/runtime/lumabri_replica_admission.h"
#include <sys/wait.h>
#include <signal.h>

static void concurrent_admission(int access) {
    int start[2], result[2], finish[2];
    assert(!pipe(start) && !pipe(result) && !pipe(finish));
    pid_t children[6];
    for (unsigned i=0;i<6;i++) {
        children[i]=fork(); assert(children[i]>=0);
        if (!children[i]) {
            close(start[1]); close(result[0]); close(finish[1]);
            char byte; assert(read(start[0],&byte,1)==1);
            uint8_t allocation[32]={77}; int permit=-1;
            int rc=lmb_replica_admit(access,allocation,&permit);
            assert(write(result[1],&rc,sizeof rc)==sizeof rc);
            assert(read(finish[0],&byte,1)==1);
            if (permit>=0) close(permit);
            _exit(0);
        }
    }
    close(start[0]); close(result[1]); close(finish[0]);
    assert(write(start[1],"123456",6)==6); close(start[1]);
    unsigned admitted=0,busy=0;
    for (unsigned i=0;i<6;i++) {
        int rc; assert(!lmb_read_full(result[0],&rc,sizeof rc));
        assert(rc==LMB_REPLICA_OK || rc==LMB_REPLICA_BUSY);
        admitted+=rc==LMB_REPLICA_OK; busy+=rc==LMB_REPLICA_BUSY;
    }
    assert(admitted==1 && busy==5); close(result[0]);
    assert(write(finish[1],"123456",6)==6); close(finish[1]);
    for (unsigned i=0;i<6;i++) {
        int status; assert(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && !WEXITSTATUS(status));
    }
}

int main(void) {
    (void)lmb_sign;
    char temporary[]="/tmp/lmb-replica-admission-XXXXXX"; assert(mkdtemp(temporary));
    int access=lmb_api_access_dir(temporary,0); assert(access>=0);
    LmbReplicaDispatch snapshot;
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && !snapshot.count);
    struct stat absent;assert(fstatat(access,"dispatch",&absent,AT_SYMLINK_NOFOLLOW) && errno==ENOENT);
    inject_create_race=1;
    int guard=lmb_api_access_lock(access); assert(guard>=0);
    assert(!inject_create_race && !combined_lock_opens);
    errno=0;
    assert(lmb_api_access_lock(access)<0 && (errno==EWOULDBLOCK || errno==EAGAIN));
    close(guard);
    assert(!fchmodat(access,"access.lock",0644,0));
    errno=EWOULDBLOCK;
    assert(lmb_api_access_lock(access)<0 && errno==EACCES);
    assert(!fchmodat(access,"access.lock",0600,0));
    assert(!mkdirat(access,"removed",0700));
    int removed=openat(access,"removed",O_RDONLY|O_DIRECTORY|O_CLOEXEC); assert(removed>=0);
    assert(!unlinkat(access,"removed",AT_REMOVEDIR));
    assert(lmb_api_access_lock(removed)<0 && errno==ENOENT); close(removed);
    lmb_read_deadline_ms=lmb_io_monotonic_ms()+30000;
    concurrent_admission(access); /* cold directory, then existing lease hints */
    concurrent_admission(access);
    uint8_t first[32]={1}, second[32]={2}; int held=-1, another=-1;
    assert(!lmb_replica_admit(access,first,&held) && held>=0);
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && snapshot.count==1 &&
        !memcmp(snapshot.allocation[0],first,32));
    assert(lmb_replica_admit(access,first,&another)==LMB_REPLICA_BUSY && another==-1);
    assert(!lmb_replica_admit(access,second,&another));
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && snapshot.count==2);
    close(another);
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) {
        close(held);
        assert(lmb_replica_admit(access,first,&another)==LMB_REPLICA_BUSY);
        assert(!lmb_replica_admit(access,second,&another));
        _exit(0); /* process death, without application cleanup */
    }
    int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && snapshot.count==1 &&
        !memcmp(snapshot.allocation[0],first,32)); /* dead child left no phantom request */
    assert(!lmb_replica_admit(access,second,&another)); close(another); close(held);
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && !snapshot.count);
    int cells[LMB_REPLICA_CELLS];
    for (unsigned i=0;i<LMB_REPLICA_CELLS;i++) {
        uint8_t id[32]={(uint8_t)(i+1)}; assert(!lmb_replica_admit(access,id,&cells[i]));
    }
    uint8_t extra[32]={99};
    assert(lmb_replica_admit(access,extra,&another)==LMB_REPLICA_BUSY);
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && snapshot.count==LMB_REPLICA_CELLS);
    close(cells[3]); assert(!lmb_replica_admit(access,extra,&cells[3]));
    for (unsigned i=0;i<LMB_REPLICA_CELLS;i++) close(cells[i]);
    int dir=openat(access,"dispatch",O_RDONLY|O_DIRECTORY|O_CLOEXEC); assert(dir>=0);
    int writer=lmb_api_access_lock(dir);assert(writer>=0);
    assert(lmb_replica_dispatch_snapshot(access,&snapshot)==LMB_REPLICA_BUSY && !snapshot.count);
    close(writer);
    int partial=openat(dir,"0.lease",O_WRONLY|O_TRUNC|O_CLOEXEC); assert(partial>=0);
    assert(write(partial,"partial",7)==7);
    assert(!flock(partial,LOCK_EX|LOCK_NB));
    assert(lmb_replica_dispatch_snapshot(access,&snapshot)==LMB_REPLICA_UNSAFE && !snapshot.count);
    close(partial);
    assert(!lmb_replica_dispatch_snapshot(access,&snapshot) && !snapshot.count);
    assert(!lmb_replica_admit(access,first,&held)); close(held);
    assert(!fchmodat(dir,"0.lease",0644,0));
    assert(lmb_replica_admit(access,first,&held)==LMB_REPLICA_UNSAFE);
    assert(lmb_replica_dispatch_snapshot(access,&snapshot)==LMB_REPLICA_UNSAFE && !snapshot.count);
    assert(!fchmodat(dir,"0.lease",0600,0));
    assert(!unlinkat(dir,"0.lease",0)); assert(!symlinkat("1.lease",dir,"0.lease"));
    assert(lmb_replica_admit(access,first,&held)==LMB_REPLICA_UNSAFE);
    assert(lmb_replica_dispatch_snapshot(access,&snapshot)==LMB_REPLICA_UNSAFE && !snapshot.count);
    for (unsigned i=0;i<LMB_REPLICA_CELLS;i++) {
        char name[16]; snprintf(name,sizeof name,"%u.lease",i); assert(!unlinkat(dir,name,0));
    }
    assert(!unlinkat(dir,"access.lock",0)); close(dir);
    assert(!unlinkat(access,"dispatch",AT_REMOVEDIR));
    assert(!unlinkat(access,"access.lock",0)); close(access); assert(!rmdir(temporary));
    assert(!combined_lock_opens);
    puts("REPLICA ADMISSION: PASS (exact allocation, exclusive-create race, concurrent cold/warm admission, cleanup errno, removed directory, unsafe permissions, process death release, bounded cells, torn hint recovery)");
    return 0;
}
