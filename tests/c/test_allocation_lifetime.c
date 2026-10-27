#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/runtime/lumabri_allocation_lifetime.h"
#include <assert.h>
#include <sys/wait.h>

static void wait_success(pid_t pid,const char *phase) {
    int status=0;
    while (waitpid(pid,&status,0)<0) assert(errno==EINTR);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        fprintf(stderr,"allocation lifetime child failed: phase=%s status=%d\n",phase,status);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void erase_record(int dir,const uint8_t id[32]) {
    char name[80]; lmb_lifetime_name(id,name);
    int at=lmb_journal_location(dir,name); assert(at>=0);
    assert(!unlinkat(at,name,0)); close(at);
}
int main(int argc,char **argv) {
    (void)lmb_sign; (void)lmb_retired_dir; (void)lmb_retired_store; (void)lmb_retired_sync;
    if (argc==3 && !strcmp(argv[1],"cloexec")) {
        errno=0;
        assert(fcntl(atoi(argv[2]),F_GETFD)<0 && errno==EBADF);
        _exit(0);
    }
    if (argc==5 && !strcmp(argv[1],"descendant")) {
        int lifetime=atoi(argv[2]),gate=atoi(argv[3]),ready=atoi(argv[4]);
        assert(fcntl(lifetime,F_GETFD)==0);
        assert(write(ready,"R",1)==1); close(ready);
        char byte; assert(read(gate,&byte,1)==0); close(gate);
        /* No explicit unlock: exit closes our final inherited description. */
        _exit(0);
    }
    assert(argc==1);
    char path[]="/tmp/lmb-lifetime-XXXXXX"; assert(mkdtemp(path));
    assert(lmb_lifetime_dir(path,0)<0 && errno==ENOENT);
    int dir=lmb_lifetime_dir(path,1); assert(dir>=0);
    LmbAllocationLifetime r={.allocation={1},.root={2},.requester={3},.offer_hash={4},
        .reserved_bytes=UINT64_C(1)<<33,.edge=1,.begin=0,.end=2},proof;
    uint8_t bytes[LMB_LIFETIME_BYTES];
    assert(!lmb_lifetime_encode(&r,bytes));
    for (size_t i=0;i<sizeof bytes;i++) assert(lmb_lifetime_decode(bytes,i,&proof));
    assert(!lmb_lifetime_decode(bytes,sizeof bytes,&proof) && proof.reserved_bytes==r.reserved_bytes);
    bytes[0]^=1; assert(lmb_lifetime_decode(bytes,sizeof bytes,&proof)); bytes[0]^=1;
    r.edge=2; assert(lmb_lifetime_encode(&r,bytes)); r.edge=1;
    r.end=0; assert(lmb_lifetime_encode(&r,bytes)); r.end=2;
    assert(lmb_lifetime_unused(dir,r.allocation)==1);
    assert(lmb_lifetime_prove(dir,r.allocation,&proof)<0); /* absence is not exit */
    int held=lmb_lifetime_create(dir,&r); assert(held>=3 && (fcntl(held,F_GETFD)&FD_CLOEXEC));
    assert(lmb_lifetime_unused(dir,r.allocation)==0);
    assert(lmb_lifetime_create(dir,&r)<0 && lmb_lifetime_prove(dir,r.allocation,&proof)==1);

    int gate[2],ready[2]; assert(!pipe(gate) && !pipe(ready));
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        close(gate[1]); close(ready[0]); close(dir);
        pid_t grandchild=fork(); assert(grandchild>=0);
        if (grandchild) _exit(0); /* reaping the immediate child is insufficient */
        assert(!fcntl(held,F_SETFD,0));
        char fd[24],input[24],output[24];
        snprintf(fd,sizeof fd,"%d",held); snprintf(input,sizeof input,"%d",gate[0]);
        snprintf(output,sizeof output,"%d",ready[1]);
        execl(argv[0],argv[0],"descendant",fd,input,output,(char *)NULL);
        perror("exec lifetime descendant"); _exit(127);
    }
    close(gate[0]); close(ready[1]); close(held); wait_success(child,"forked parent");
    struct pollfd check={ready[0],POLLIN,0}; assert(poll(&check,1,3000)>0);
    char byte=0; assert(read(ready[0],&byte,1)==1 && byte=='R'); close(ready[0]);
    assert(lmb_lifetime_prove(dir,r.allocation,&proof)==1); /* orphan survived exec */
    close(gate[1]);
    int exited=0;
    for (unsigned i=0;i<150;i++) {
        int rc=lmb_lifetime_prove(dir,r.allocation,&proof); assert(rc>=0);
        if (!rc) { exited=1; break; }
        (void)poll(NULL,0,20);
    }
    assert(exited && !memcmp(proof.requester,r.requester,32) && proof.edge==1);
    assert(lmb_lifetime_create(dir,&r)<0); /* a proved lifetime can never restart */
    close(dir);
    dir=lmb_lifetime_dir(path,0); assert(dir>=0 && !lmb_lifetime_prove(dir,r.allocation,&proof));

    char name[80]; lmb_lifetime_name(r.allocation,name);
    assert(!fchmodat(dir,name,0644,0)); assert(lmb_lifetime_prove(dir,r.allocation,&proof)<0);
    assert(!fchmodat(dir,name,0600,0));
    assert(!linkat(dir,name,dir,"alias",0)); assert(lmb_lifetime_prove(dir,r.allocation,&proof)<0);
    assert(!unlinkat(dir,"alias",0));
    uint8_t other[32]={9}; char another[80]; lmb_lifetime_name(other,another);
    assert(!symlinkat(name,dir,another)); assert(lmb_lifetime_prove(dir,other,&proof)<0);
    assert(lmb_lifetime_unused(dir,other)==0 && !unlinkat(dir,another,0));
    int partial=openat(dir,another,O_WRONLY|O_CREAT|O_EXCL,0600); assert(partial>=0);
    assert(write(partial,bytes,8)==8); close(partial);
    assert(lmb_lifetime_prove(dir,other,&proof)<0 && !lmb_lifetime_unused(dir,other));
    erase_record(dir,other);
    assert(!mkfifoat(dir,another,0600)); assert(lmb_lifetime_prove(dir,other,&proof)<0);
    erase_record(dir,other);
    /* An unrelated CLOEXEC descriptor cannot retain another model at exec. */
    r.allocation[0]=2; held=lmb_lifetime_create(dir,&r); assert(held>=3);
    child=fork(); assert(child>=0);
    if (!child) {
        /* macOS does not put true in /bin. Re-exec our fixture instead,
         * verifying closure directly rather than assuming a helper path. */
        char fd[24]; snprintf(fd,sizeof fd,"%d",held);
        execl(argv[0],argv[0],"cloexec",fd,(char *)NULL);
        perror("exec lifetime CLOEXEC check"); _exit(127);
    }
    close(held); wait_success(child,"CLOEXEC"); assert(!lmb_lifetime_prove(dir,r.allocation,&proof));
    erase_record(dir,r.allocation); r.allocation[0]=1; erase_record(dir,r.allocation);
    /* Cross the actual production flat threshold; older evidence and the
     * first indexed identity must still reject reuse and prove real exit. */
    memset(r.allocation,0,sizeof r.allocation); r.allocation[0]=7;
    for (unsigned i=0;i<=LMB_LIFETIME_MAX;i++) {
        lmb_put32(r.allocation+4,i+1);
        held=lmb_lifetime_create(dir,&r); assert(held>=3);
        assert(lmb_lifetime_prove(dir,r.allocation,&proof)==1);
        close(held);
        assert(!lmb_lifetime_unused(dir,r.allocation) && !lmb_lifetime_prove(dir,r.allocation,&proof));
        assert(lmb_lifetime_create(dir,&r)<0);
    }
    close(dir); dir=lmb_lifetime_dir(path,0); assert(dir>=0);
    for (unsigned i=0;i<=LMB_LIFETIME_MAX;i++) {
        lmb_put32(r.allocation+4,i+1);
        assert(!lmb_lifetime_prove(dir,r.allocation,&proof));
        erase_record(dir,r.allocation);
    }
    int history=lmb_journal_child(dir,LMB_JOURNAL_HISTORY,0); assert(history>=0);
    int first=lmb_journal_child(history,"07",0); assert(first>=0);
    assert(!unlinkat(first,"00",AT_REMOVEDIR)); close(first);
    assert(!unlinkat(history,"07",AT_REMOVEDIR)); close(history);
    assert(!unlinkat(dir,LMB_JOURNAL_HISTORY,AT_REMOVEDIR));
    assert(!unlinkat(dir,"lock",0)); close(dir);
    int base=open(path,O_RDONLY|O_DIRECTORY); assert(base>=0);
    assert(!unlinkat(base,"allocation-lifetimes",AT_REMOVEDIR)); close(base); assert(!rmdir(path));
    puts("ALLOCATION LIFETIME: PASS (fork+exec descendants, last close, 4097 immutable lifetimes, restart, unsafe/missing records unknown)");
    return 0;
}
