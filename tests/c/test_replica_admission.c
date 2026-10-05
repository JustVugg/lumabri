#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/runtime/lumabri_replica_admission.h"
#include <assert.h>
#include <sys/wait.h>
#include <signal.h>

int main(void) {
    (void)lmb_sign;
    char temporary[]="/tmp/lmb-replica-admission-XXXXXX"; assert(mkdtemp(temporary));
    int access=lmb_api_access_dir(temporary,0); assert(access>=0);
    uint8_t first[32]={1}, second[32]={2}; int held=-1, another=-1;
    assert(!lmb_replica_admit(access,first,&held) && held>=0);
    assert(lmb_replica_admit(access,first,&another)==LMB_REPLICA_BUSY && another==-1);
    assert(!lmb_replica_admit(access,second,&another)); close(another);
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) {
        close(held);
        assert(lmb_replica_admit(access,first,&another)==LMB_REPLICA_BUSY);
        assert(!lmb_replica_admit(access,second,&another));
        _exit(0); /* process death, without application cleanup */
    }
    int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!lmb_replica_admit(access,second,&another)); close(another); close(held);
    int cells[LMB_REPLICA_CELLS];
    for (unsigned i=0;i<LMB_REPLICA_CELLS;i++) {
        uint8_t id[32]={(uint8_t)(i+1)}; assert(!lmb_replica_admit(access,id,&cells[i]));
    }
    uint8_t extra[32]={99};
    assert(lmb_replica_admit(access,extra,&another)==LMB_REPLICA_BUSY);
    close(cells[3]); assert(!lmb_replica_admit(access,extra,&cells[3]));
    for (unsigned i=0;i<LMB_REPLICA_CELLS;i++) close(cells[i]);
    int dir=openat(access,"dispatch",O_RDONLY|O_DIRECTORY|O_CLOEXEC); assert(dir>=0);
    int partial=openat(dir,"0.lease",O_WRONLY|O_TRUNC|O_CLOEXEC); assert(partial>=0);
    assert(write(partial,"partial",7)==7); close(partial);
    assert(!lmb_replica_admit(access,first,&held)); close(held);
    assert(!fchmodat(dir,"0.lease",0644,0));
    assert(lmb_replica_admit(access,first,&held)==LMB_REPLICA_UNSAFE);
    assert(!fchmodat(dir,"0.lease",0600,0));
    assert(!unlinkat(dir,"0.lease",0)); assert(!symlinkat("1.lease",dir,"0.lease"));
    assert(lmb_replica_admit(access,first,&held)==LMB_REPLICA_UNSAFE);
    for (unsigned i=0;i<LMB_REPLICA_CELLS;i++) {
        char name[16]; snprintf(name,sizeof name,"%u.lease",i); assert(!unlinkat(dir,name,0));
    }
    assert(!unlinkat(dir,"access.lock",0)); close(dir);
    assert(!unlinkat(access,"dispatch",AT_REMOVEDIR)); close(access); assert(!rmdir(temporary));
    puts("REPLICA ADMISSION: PASS (exact allocation, other alias busy, process death release, bounded cells, torn hint recovery, unsafe files)");
    return 0;
}
