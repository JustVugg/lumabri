#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "lumabri_proto.h"
#include <assert.h>
#include <sys/wait.h>
static int sync_fail;
static int receipt_fsync(int fd) {
    if (sync_fail) { sync_fail=0; errno=EIO; return -1; }
    return fsync(fd);
}
#define fsync receipt_fsync
#include "src/runtime/lumabri_retired_receipts.h"

int main(void) {
    (void)lmb_sign;
    char path[]="/tmp/lmb-release-receipts-XXXXXX"; assert(mkdtemp(path));
    assert(lmb_retired_dir(path,0)<0 && errno==ENOENT);
    int dir=lmb_retired_dir(path,1); assert(dir>=0);
    int lock=lmb_retired_lock(dir); assert(lock>=0 && lmb_retired_lock(dir)<0);
    LmbRetiredReceipt r={.allocation={1},.root={2},.requester={3},.phase=LMB_RECEIPT_PREPARED,
        .seal={.instance={4},.revision=3,.draining=LMB_NODE_CONTROL_RETIRED}},readback;
    uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES];
    assert(!lmb_retired_encode(&r,bytes));
    for (size_t n=0;n<sizeof bytes;n++) assert(lmb_retired_decode(bytes,n,&readback));
    assert(!lmb_retired_decode(bytes,sizeof bytes,&readback));
    r.seal.sessions=1; assert(lmb_retired_encode(&r,bytes)); r.seal.sessions=0;
    r.seal.draining=1; assert(lmb_retired_encode(&r,bytes)); r.seal.draining=LMB_NODE_CONTROL_RETIRED;
    assert(lmb_retired_load(dir,r.allocation,&readback)==1);
    r.phase=LMB_RECEIPT_DONE; assert(lmb_retired_store(dir,&r)); /* no unreserved proof */
    r.phase=LMB_RECEIPT_PREPARED;
    assert(!lmb_retired_store(dir,&r));
    assert(!lmb_retired_load(dir,r.allocation,&readback) && readback.phase==LMB_RECEIPT_PREPARED);
    r.phase=LMB_RECEIPT_DONE; sync_fail=1;
    assert(lmb_retired_store(dir,&r));
    assert(!lmb_retired_load(dir,r.allocation,&readback) && readback.phase==LMB_RECEIPT_PREPARED);
    assert(!lmb_retired_store(dir,&r));
    assert(!lmb_retired_load(dir,r.allocation,&readback) && readback.phase==LMB_RECEIPT_DONE);
    assert(!lmb_retired_store(dir,&r));
    assert(!lmb_retired_sync(dir,r.allocation));
    sync_fail=1; assert(lmb_retired_sync(dir,r.allocation));
    r.phase=LMB_RECEIPT_PREPARED; assert(lmb_retired_store(dir,&r)); r.phase=LMB_RECEIPT_DONE;
    r.requester[0]++; assert(lmb_retired_store(dir,&r)); r.requester[0]--;
    r.root[0]++; assert(lmb_retired_store(dir,&r)); r.root[0]--;
    r.seal.revision++; assert(lmb_retired_store(dir,&r)); r.seal.revision--;
    r.seal.instance[0]++; assert(lmb_retired_store(dir,&r)); r.seal.instance[0]--;
    char name[80]; lmb_retired_name(r.allocation,name);
    assert(!fchmodat(dir,name,0644,0)); assert(lmb_retired_load(dir,r.allocation,&readback)<0);
    assert(!fchmodat(dir,name,0600,0));
    assert(!linkat(dir,name,dir,"extra",0)); assert(lmb_retired_load(dir,r.allocation,&readback)<0);
    assert(!unlinkat(dir,"extra",0));
    assert(!symlinkat(name,dir,"pending")); assert(lmb_retired_store(dir,&r));
    assert(!unlinkat(dir,"pending",0));
    uint8_t other[32]={9}; char other_name[80]; lmb_retired_name(other,other_name);
    assert(!symlinkat(name,dir,other_name)); assert(lmb_retired_load(dir,other,&readback)<0);
    assert(!unlinkat(dir,other_name,0));
    int malformed=openat(dir,other_name,O_WRONLY|O_CREAT|O_EXCL,0600); assert(malformed>=0);
    assert(!lmb_retired_encode(&r,bytes));
    assert(write(malformed,bytes,sizeof bytes)==sizeof bytes); close(malformed);
    assert(lmb_retired_load(dir,other,&readback)<0); /* valid record under another identity */
    assert(!unlinkat(dir,other_name,0));
    close(lock); close(dir);
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        int reopened=lmb_retired_dir(path,0); assert(reopened>=0);
        assert(!lmb_retired_load(reopened,r.allocation,&readback) && readback.phase==LMB_RECEIPT_DONE);
        assert(!memcmp(readback.requester,r.requester,32) && readback.seal.revision==3);
        close(reopened); _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    dir=lmb_retired_dir(path,0); assert(dir>=0); lock=lmb_retired_lock(dir); assert(lock>=0);
    for (unsigned i=1;i<LMB_RETIRED_RECEIPTS_MAX;i++) {
        r.allocation[1]=(uint8_t)i; r.phase=LMB_RECEIPT_PREPARED;
        assert(!lmb_retired_store(dir,&r));
    }
    r.allocation[2]=1; assert(!lmb_retired_store(dir,&r)); /* indexed, no evidence evicted */
    assert(!lmb_retired_load(dir,r.allocation,&readback) && readback.phase==LMB_RECEIPT_PREPARED);
    r.phase=LMB_RECEIPT_DONE; assert(!lmb_retired_store(dir,&r));
    assert(!lmb_retired_load(dir,r.allocation,&readback) && readback.phase==LMB_RECEIPT_DONE);
    assert(!lmb_retired_sync(dir,r.allocation));
    lmb_retired_name(r.allocation,name);
    int history=lmb_journal_location(dir,name); assert(history>=0);
    assert(!unlinkat(history,name,0)); close(history);
    r.allocation[2]=0; r.phase=LMB_RECEIPT_DONE; assert(!lmb_retired_store(dir,&r));
    for (unsigned i=0;i<LMB_RETIRED_RECEIPTS_MAX;i++) {
        r.allocation[1]=(uint8_t)i; lmb_retired_name(r.allocation,name); assert(!unlinkat(dir,name,0));
    }
    history=lmb_journal_child(dir,LMB_JOURNAL_HISTORY,0); assert(history>=0);
    int first=lmb_journal_child(history,"01",0); assert(first>=0);
    assert(!unlinkat(first,"ff",AT_REMOVEDIR)); close(first);
    assert(!unlinkat(history,"01",AT_REMOVEDIR)); close(history);
    assert(!unlinkat(dir,LMB_JOURNAL_HISTORY,AT_REMOVEDIR));
    assert(!unlinkat(dir,"lock",0)); close(lock); close(dir);
    int base=open(path,O_RDONLY|O_DIRECTORY); assert(base>=0);
    assert(!unlinkat(base,"retired-allocations",AT_REMOVEDIR)); close(base); assert(!rmdir(path));
    puts("RETIRED RECEIPTS: PASS (intent is not release, immutable identity, sync failure, restart, indexed history, unsafe entries rejected)");
    return 0;
}
