/* Owner-private, exact-key lifecycle history. Keep legacy records in place;
 * once the flat namespace is full, new keys go into a two-level hash-prefix
 * namespace. No receipt is deleted, summarized probabilistically or reused.
 * Readers do exact lookups; writers hold their store's existing global lock.
 * This removes a fixed lifetime-count ceiling, not the need for disk space.
 */
#ifndef LMB_JOURNAL_INDEX_H
#define LMB_JOURNAL_INDEX_H
#include "lumabri_proto.h"
#include <dirent.h>
#include <sys/stat.h>

#define LMB_JOURNAL_HISTORY "history-v1"

static LMB_MAYBE_UNUSED int lmb_journal_private(int fd, int directory) {
    struct stat s;
    if (fstat(fd,&s)) return -1;
    if (s.st_uid!=geteuid() || (s.st_mode&077) ||
        (directory ? !S_ISDIR(s.st_mode) : !S_ISREG(s.st_mode) || s.st_nlink!=1)) {
        errno=EACCES; return -1;
    }
    return 0;
}
static LMB_MAYBE_UNUSED int lmb_journal_name_valid(const char *name) {
    size_t n=name ? strlen(name) : 0;
    if (n<66 || n>79 || name[64]!='.') return 0;
    for (size_t i=0;i<64;i++)
        if (!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='f'))) return 0;
    for (size_t i=65;i<n;i++) if (!(name[i]>='a' && name[i]<='z')) return 0;
    return 1;
}
static LMB_MAYBE_UNUSED int lmb_journal_child(int parent,const char *name,int create) {
    if (lmb_journal_private(parent,1)) { errno=EACCES; return -1; }
    if (create && mkdirat(parent,name,0700) && errno!=EEXIST) return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    /* Sync existing entries too: a prior creator may have lost the parent
     * fsync. Never return writable history with an uncommitted path. */
    if (lmb_journal_private(fd,1) || (create && fsync(parent))) {
        int saved=errno; close(fd); errno=saved ? saved : EACCES; return -1;
    }
    return fd;
}
static LMB_MAYBE_UNUSED int lmb_journal_history(int dir,const char *name,int create) {
    if (!lmb_journal_name_valid(name)) { errno=EINVAL; return -1; }
    char first[3]={name[0],name[1],0},second[3]={name[2],name[3],0};
    const char *parts[]={LMB_JOURNAL_HISTORY,first,second};
    int at=fcntl(dir,F_DUPFD_CLOEXEC,3);
    if (at<0) return -1;
    for (unsigned i=0;i<3;i++) {
        int next=lmb_journal_child(at,parts[i],create),saved=errno;
        close(at); at=next; errno=saved;
        if (at<0) return -1;
    }
    return at;
}
/* Return an owned parent descriptor for an EXISTING key. Any directory entry
 * occupies the identity, including a partial write or unsafe link. The actual
 * read must validate the file; unsafe evidence never becomes a missing key. */
static LMB_MAYBE_UNUSED int lmb_journal_location(int dir,const char *name) {
    if (!lmb_journal_name_valid(name)) { errno=EINVAL; return -1; }
    if (lmb_journal_private(dir,1)) { errno=EACCES; return -1; }
    struct stat s;
    if (!fstatat(dir,name,&s,AT_SYMLINK_NOFOLLOW)) return fcntl(dir,F_DUPFD_CLOEXEC,3);
    if (errno!=ENOENT) return -1;
    int at=lmb_journal_history(dir,name,0);
    if (at<0) return -1;
    if (!fstatat(at,name,&s,AT_SYMLINK_NOFOLLOW)) return at;
    int saved=errno; close(at); errno=saved; return -1;
}
static LMB_MAYBE_UNUSED int lmb_journal_open(int dir,const char *name,int flags) {
    if (flags&(O_CREAT|O_TRUNC)) { errno=EINVAL; return -1; }
    int at=lmb_journal_location(dir,name);
    if (at<0) return -1;
    int fd=openat(at,name,flags|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK),saved=errno;
    close(at); errno=saved;
    if (fd>=0 && lmb_journal_private(fd,0)) { close(fd); errno=EACCES; return -1; }
    return fd;
}
/* Select a parent for a new key without relocating any earlier one. The
 * caller serializes this with creation and uses O_EXCL or its transactional
 * store. Flat-record count is an indexing threshold, not evidence eviction. */
static LMB_MAYBE_UNUSED int lmb_journal_select(int dir,const char *name,const char *suffix,unsigned flat_limit) {
    if (!lmb_journal_name_valid(name) || !suffix || strcmp(name+64,suffix) || !flat_limit) {
        errno=EINVAL; return -1;
    }
    int at=lmb_journal_location(dir,name);
    if (at>=0 || errno!=ENOENT) return at;
    int copy=openat(dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (copy<0) return -1;
    DIR *scan=fdopendir(copy);
    if (!scan) { int saved=errno; close(copy); errno=saved; return -1; }
    unsigned count=0; int bad=0; struct dirent *entry;
    while (count<flat_limit) {
        errno=0; entry=readdir(scan);
        if (!entry) { bad=errno; break; }
        if (lmb_journal_name_valid(entry->d_name) && !strcmp(entry->d_name+64,suffix)) count++;
    }
    closedir(scan);
    if (bad) { errno=bad; return -1; }
    return count<flat_limit ? fcntl(dir,F_DUPFD_CLOEXEC,3) : lmb_journal_history(dir,name,1);
}
/* Bounded keyset page, ordered by immutable ID (not by modification time).
 * One extra key proves that another page exists. The cursor is exclusive;
 * concurrent new keys before it appear on refresh, never as a false snapshot.
 * Only listing uses directory scans; all lifecycle mutations use exact keys. */
#define LMB_JOURNAL_PAGE 256u
typedef struct {
    char names[LMB_JOURNAL_PAGE+1][80];
    unsigned count;
} LmbJournalPage;
static LMB_MAYBE_UNUSED int lmb_journal_page_scan(int dir,const char *suffix,const char *after,
                                                const char *prefix,LmbJournalPage *page) {
    int copy=openat(dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (copy<0) return -1;
    DIR *scan=fdopendir(copy);
    if (!scan) { int saved=errno; close(copy); errno=saved; return -1; }
    int bad=0; struct dirent *entry;
    for (;;) {
        errno=0; entry=readdir(scan);
        if (!entry) { bad=errno; break; }
        const char *name=entry->d_name;
        if (!lmb_journal_name_valid(name) || strcmp(name+64,suffix)) continue;
        if (prefix && strncmp(prefix,name,4)) { bad=EINVAL; break; }
        if (after[0] && strncmp(name,after,64)<=0) continue;
        unsigned at=0;
        while (at<page->count && strcmp(page->names[at],name)<0) at++;
        if (at<page->count && !strcmp(page->names[at],name)) { bad=EEXIST; break; }
        if (at>LMB_JOURNAL_PAGE) continue;
        if (page->count<=LMB_JOURNAL_PAGE) page->count++;
        memmove(page->names[at+1],page->names[at],(page->count-at-1)*sizeof page->names[0]);
        strcpy(page->names[at],name);
    }
    closedir(scan);
    if (bad) { errno=bad; return -1; }
    return 0;
}
static LMB_MAYBE_UNUSED int lmb_journal_page(int dir,const char *suffix,const char *after,LmbJournalPage *page) {
    char check[80];
    if (!suffix || !after || strlen(suffix)>15 || (after[0] && strlen(after)!=64)) { errno=EINVAL; return -1; }
    snprintf(check,sizeof check,"%064u%s",0u,suffix);
    if (!lmb_journal_name_valid(check)) { errno=EINVAL; return -1; }
    if (after[0]) {
        snprintf(check,sizeof check,"%s%s",after,suffix);
        if (!lmb_journal_name_valid(check)) { errno=EINVAL; return -1; }
    }
    memset(page,0,sizeof *page);
    if (lmb_journal_private(dir,1) || lmb_journal_page_scan(dir,suffix,after,NULL,page)) return -1;
    int history=lmb_journal_child(dir,LMB_JOURNAL_HISTORY,0);
    if (history<0) return errno==ENOENT ? 0 : -1;
    int bad=0;
    for (unsigned i=0;i<256 && !bad;i++) {
        char first[3]; snprintf(first,sizeof first,"%02x",i);
        if (after[0] && strncmp(first,after,2)<0) continue;
        if (page->count>LMB_JOURNAL_PAGE && strncmp(first,page->names[LMB_JOURNAL_PAGE],2)>0) break;
        int parent=lmb_journal_child(history,first,0);
        if (parent<0) { if (errno!=ENOENT) bad=errno; continue; }
        for (unsigned j=0;j<256 && !bad;j++) {
            char second[3],prefix[5]; snprintf(second,sizeof second,"%02x",j);
            snprintf(prefix,sizeof prefix,"%02x%02x",i,j);
            if (after[0] && strncmp(prefix,after,4)<0) continue;
            if (page->count>LMB_JOURNAL_PAGE && strncmp(prefix,page->names[LMB_JOURNAL_PAGE],4)>0) break;
            int bucket=lmb_journal_child(parent,second,0);
            if (bucket<0) { if (errno!=ENOENT) bad=errno; continue; }
            if (lmb_journal_page_scan(bucket,suffix,after,prefix,page)) bad=errno;
            close(bucket);
        }
        close(parent);
    }
    close(history);
    if (bad) { errno=bad; return -1; }
    return 0;
}
#endif
