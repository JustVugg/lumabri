#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <unistd.h>
#include <errno.h>
static int sync_fault;
static int journal_test_fsync(int fd) { if (sync_fault) { errno=EIO; return -1; } return fsync(fd); }
#define fsync journal_test_fsync
#include "src/runtime/lumabri_journal_index.h"
#undef fsync
#include <assert.h>

static void name_for(unsigned value,char name[80]) {
    snprintf(name,80,"%08x%056x.intent",value,0);
}
static void save(int dir,unsigned value) {
    char name[80]; name_for(value,name);
    int at=lmb_journal_select(dir,name,".intent",2); assert(at>=0);
    int fd=openat(at,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600); assert(fd>=0);
    assert(write(fd,&value,sizeof value)==sizeof value && !fsync(fd)); close(fd);
    assert(!fsync(at)); close(at);
}
int main(void) {
    char path[]="/tmp/lmb-journal-index-XXXXXX"; assert(mkdtemp(path));
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW); assert(dir>=0);
    char name[80]; name_for(0,name);
    assert(lmb_journal_open(dir,name,O_RDONLY)<0 && errno==ENOENT);
    struct stat s; assert(fstatat(dir,LMB_JOURNAL_HISTORY,&s,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT);
    for (unsigned i=0;i<270;i++) save(dir,i);
    LmbJournalPage page;
    assert(!lmb_journal_page(dir,".intent","",&page) && page.count==257);
    for (unsigned i=0;i<257;i++) { name_for(i,name); assert(!strcmp(page.names[i],name)); }
    char cursor[65]; memcpy(cursor,page.names[255],64); cursor[64]=0;
    assert(!lmb_journal_page(dir,".intent",cursor,&page) && page.count==14);
    for (unsigned i=0;i<14;i++) { name_for(i+256,name); assert(!strcmp(page.names[i],name)); }
    assert(!lmb_journal_page(dir,".state","",&page) && !page.count);
    assert(lmb_journal_page(dir,".intent","../escape",&page)<0);
    assert(lmb_journal_page(dir,"../escape","",&page)<0);
    /* Traversal crosses real prefix boundaries, independent of insertion
     * order and with a flat key lexically after indexed keys. */
    const unsigned spread[]={0xfffe0000,0x01020000,0x01010000,0x10000000,0xffff0000};
    name_for(spread[0],name); sync_fault=1;
    assert(lmb_journal_select(dir,name,".intent",2)<0 && errno==EIO);
    /* A prior failed parent fsync is retried even for an existing directory. */
    assert(lmb_journal_select(dir,name,".intent",2)<0 && errno==EIO);
    sync_fault=0;
    for (unsigned i=0;i<sizeof spread/sizeof *spread;i++) save(dir,spread[i]);
    name_for(269,name); memcpy(cursor,name,64);
    assert(!lmb_journal_page(dir,".intent",cursor,&page) && page.count==5);
    const unsigned sorted[]={0x01010000,0x01020000,0x10000000,0xfffe0000,0xffff0000};
    for (unsigned i=0;i<5;i++) { name_for(sorted[i],name); assert(!strcmp(page.names[i],name)); }
    for (unsigned i=0;i<5;i++) {
        name_for(spread[i],name); int target=lmb_journal_location(dir,name); assert(target>=0);
        assert(!unlinkat(target,name,0)); close(target);
        int h=lmb_journal_child(dir,LMB_JOURNAL_HISTORY,0); assert(h>=0);
        char first[3]={name[0],name[1],0},second[3]={name[2],name[3],0};
        int parent=lmb_journal_child(h,first,0); assert(parent>=0);
        assert(!unlinkat(parent,second,AT_REMOVEDIR)); close(parent);
        if (unlinkat(h,first,AT_REMOVEDIR)) assert(errno==ENOTEMPTY || errno==EEXIST);
        close(h);
    }
    for (unsigned i=0;i<270;i++) {
        name_for(i,name); int fd=lmb_journal_open(dir,name,O_RDONLY); assert(fd>=0);
        unsigned actual=999; assert(read(fd,&actual,sizeof actual)==sizeof actual && actual==i); close(fd);
        int at=lmb_journal_select(dir,name,".intent",2); assert(at>=0);
        assert(openat(at,name,O_WRONLY|O_CREAT|O_EXCL,0600)<0 && errno==EEXIST); close(at);
        assert(fstatat(dir,name,&s,AT_SYMLINK_NOFOLLOW)==(i<2 ? 0 : -1));
    }
    /* A partial write remains an occupied identity, never a fresh intent. */
    name_for(269,name); int fd=lmb_journal_open(dir,name,O_RDWR); assert(fd>=0);
    assert(!ftruncate(fd,0)); close(fd);
    int at=lmb_journal_location(dir,name); assert(at>=0);
    assert(openat(at,name,O_CREAT|O_EXCL|O_WRONLY,0600)<0 && errno==EEXIST);
    /* All ancestors and files remain private; no following links/FIFOs. */
    assert(!fchmodat(at,name,0644,0)); assert(lmb_journal_open(dir,name,O_RDONLY)<0);
    assert(!fchmodat(at,name,0600,0));
    assert(!linkat(at,name,at,"alias",0)); assert(lmb_journal_open(dir,name,O_RDONLY)<0);
    assert(!unlinkat(at,"alias",0));
    assert(!unlinkat(at,name,0) && !symlinkat("/dev/null",at,name));
    int occupied=lmb_journal_location(dir,name); assert(occupied>=0); close(occupied);
    assert(lmb_journal_open(dir,name,O_RDONLY)<0);
    assert(!unlinkat(at,name,0) && !mkfifoat(at,name,0600));
    assert(lmb_journal_open(dir,name,O_RDONLY)<0); close(at);
    assert(lmb_journal_select(dir,"../escape.intent",".intent",2)<0);
    assert(lmb_journal_select(dir,name,".release",2)<0);
    assert(lmb_journal_open(dir,name,O_RDONLY|O_TRUNC)<0);
    int history=lmb_journal_child(dir,LMB_JOURNAL_HISTORY,0); assert(history>=0);
    assert(!fchmod(history,0755)); assert(lmb_journal_location(dir,name)<0 && errno==EACCES);
    assert(lmb_journal_page(dir,".intent","",&page)<0 && errno==EACCES);
    assert(!fchmod(history,0700)); close(history);
    close(dir); dir=open(path,O_RDONLY|O_DIRECTORY); assert(dir>=0);
    /* History remains usable after reopen, with the original flat keys intact. */
    name_for(1,name); fd=lmb_journal_open(dir,name,O_RDONLY); assert(fd>=0); close(fd);
    for (unsigned i=0;i<270;i++) {
        name_for(i,name); at=lmb_journal_location(dir,name); assert(at>=0);
        assert(!unlinkat(at,name,0)); close(at);
    }
    history=lmb_journal_child(dir,LMB_JOURNAL_HISTORY,0); assert(history>=0);
    int first=lmb_journal_child(history,"00",0); assert(first>=0);
    assert(!unlinkat(first,"00",AT_REMOVEDIR)); close(first);
    assert(!unlinkat(history,"00",AT_REMOVEDIR)); close(history);
    assert(!unlinkat(dir,LMB_JOURNAL_HISTORY,AT_REMOVEDIR)); close(dir); assert(!rmdir(path));
    puts("JOURNAL INDEX: PASS (flat compatibility, exact keys, bounded ordered pages across prefixes, partial records, private paths, reopen)");
    return 0;
}
