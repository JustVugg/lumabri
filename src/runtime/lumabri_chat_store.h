/* Bounded, private chat documents. No inference authority lives here.
 * One credential digest owns one namespace, so reusing a revoked username
 * cannot expose its former conversations. The OS account remains trusted. */
#ifndef LMB_CHAT_STORE_H
#define LMB_CHAT_STORE_H
#include "lumabri_api_access.h"
#include <dirent.h>

#define LMB_CHAT_MAX 32u
#define LMB_CHAT_BYTES (256u * 1024u)
enum { LMB_CHAT_OK=0, LMB_CHAT_MISSING=1, LMB_CHAT_CONFLICT=2,
       LMB_CHAT_QUOTA=3, LMB_CHAT_UNSAFE=4, LMB_CHAT_BUSY=5 };
typedef struct { char id[33]; uint32_t revision; uint64_t updated; char *data; size_t size; } LmbChat;

static inline int lmb_chat_id(const char *id) {
    if (strlen(id)!=32) return 0;
    for (unsigned i=0; i<32; i++) if (!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f'))) return 0;
    return 1;
}
static inline int lmb_chat_subdir(int parent, const char *name) {
    if (mkdirat(parent,name,0700) && errno!=EEXIST) return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static inline int lmb_chat_dir(int access, const LmbApiUser *user) {
    int root=lmb_chat_subdir(access,"history"); if (root<0) return -1;
    char namespace[65]; lmb_hex(namespace,user->digest,32);
    int fd=lmb_chat_subdir(root,namespace); close(root); return fd;
}
static inline void lmb_chat_free(LmbChat *chat) {
    if (chat->data) { lmb_api_wipe(chat->data,chat->size); free(chat->data); }
    memset(chat,0,sizeof *chat);
}
/* Atomic replacement allows lock-free readers to see an old OR new complete
 * document. Updates/deletes/counting must hold the directory access lock. */
static inline int lmb_chat_load(int dir, const char *id, LmbChat *chat) {
    memset(chat,0,sizeof *chat);
    if (!lmb_chat_id(id)) return LMB_CHAT_UNSAFE;
    int fd=openat(dir,id,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? LMB_CHAT_MISSING : LMB_CHAT_UNSAFE;
    struct stat st; unsigned char header[16];
    if (lmb_api_private_fd(fd,0) || fstat(fd,&st) || st.st_size<16 ||
        st.st_size>16+LMB_CHAT_BYTES || read(fd,header,16)!=16 || memcmp(header,"LMBCHAT1",8)) {
        close(fd); return LMB_CHAT_UNSAFE;
    }
    uint32_t revision=lmb_get32(header+8), size=lmb_get32(header+12);
    if (!revision || size!=(uint64_t)st.st_size-16) { close(fd); return LMB_CHAT_UNSAFE; }
    char *data=malloc((size_t)size+1);
    if (!data) { close(fd); return LMB_CHAT_UNSAFE; }
    size_t at=0;
    while (at<size) {
        ssize_t got=read(fd,data+at,size-at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) { close(fd); lmb_api_wipe(data,at); free(data); return LMB_CHAT_UNSAFE; }
        at+=(size_t)got;
    }
    close(fd); data[size]=0;
    memcpy(chat->id,id,33); chat->revision=revision;
    chat->updated=st.st_mtime>0 ? (uint64_t)st.st_mtime : 0;
    chat->data=data; chat->size=size; return LMB_CHAT_OK;
}
/* The list is independently opened: dup(dir) would share the directory
 * cursor between forked API children. Never silently omit unsafe entries. */
static inline int lmb_chat_list(int dir, char ids[LMB_CHAT_MAX][33], size_t *count) {
    *count=0; int fd=openat(dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return LMB_CHAT_UNSAFE;
    DIR *stream=fdopendir(fd); if (!stream) { close(fd); return LMB_CHAT_UNSAFE; }
    struct dirent *entry; int rc=LMB_CHAT_OK;
    errno=0;
    while ((entry=readdir(stream))) {
        if (!lmb_chat_id(entry->d_name)) continue;
        struct stat st;
        if (*count==LMB_CHAT_MAX || fstatat(dir,entry->d_name,&st,AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) ||
            st.st_nlink!=1 || st.st_size<16 || st.st_size>16+LMB_CHAT_BYTES) { rc=LMB_CHAT_UNSAFE; break; }
        memcpy(ids[(*count)++],entry->d_name,33);
        errno=0;
    }
    if (errno && !entry) rc=LMB_CHAT_UNSAFE;
    closedir(stream); return rc;
}
/* Exact fixed transaction name under the per-user lock: crashes leave at
 * most one bounded temporary file, recovered on the next write. */
static inline int lmb_chat_temporary_clear(int dir) {
    struct stat st;
    if (fstatat(dir,".transaction",&st,AT_SYMLINK_NOFOLLOW)) return errno==ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1) return -1;
    return unlinkat(dir,".transaction",0);
}
static inline int lmb_chat_commit(int dir, const char *id, uint32_t expected,
                                 const void *data, size_t size, int remove) {
    if (!lmb_chat_id(id) || (!remove && (!data || !size || size>LMB_CHAT_BYTES))) return LMB_CHAT_UNSAFE;
    int lock=lmb_api_access_lock(dir); if (lock<0) return LMB_CHAT_BUSY;
    LmbChat previous; int rc=lmb_chat_load(dir,id,&previous);
    if (rc==LMB_CHAT_OK) {
        uint32_t observed=previous.revision; lmb_chat_free(&previous);
        rc=expected==observed && expected<UINT32_MAX ? LMB_CHAT_OK : LMB_CHAT_CONFLICT;
    } else if (rc==LMB_CHAT_MISSING) rc=!expected && !remove ? LMB_CHAT_OK : LMB_CHAT_MISSING;
    if (rc) goto done;
    if (remove) {
        rc=unlinkat(dir,id,0) || fsync(dir) ? LMB_CHAT_UNSAFE : LMB_CHAT_OK;
        goto done;
    }
    char ids[LMB_CHAT_MAX][33]; size_t count;
    rc=lmb_chat_list(dir,ids,&count); if (rc) goto done;
    if (!expected && count>=LMB_CHAT_MAX) { rc=LMB_CHAT_QUOTA; goto done; }
    if (lmb_chat_temporary_clear(dir)) { rc=LMB_CHAT_UNSAFE; goto done; }
    int fd=openat(dir,".transaction",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) { rc=LMB_CHAT_UNSAFE; goto done; }
    unsigned char header[16]; memcpy(header,"LMBCHAT1",8);
    lmb_put32(header+8,expected+1); lmb_put32(header+12,(uint32_t)size);
    const void *parts[]={header,data}; size_t sizes[]={16,size}; int bad=0;
    for (unsigned i=0; i<2 && !bad; i++) for (size_t at=0; at<sizes[i];) {
        ssize_t n=write(fd,(const char *)parts[i]+at,sizes[i]-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { bad=1; break; }
        at+=(size_t)n;
    }
    if (!bad && fsync(fd)) bad=1;
    if (close(fd)) bad=1;
    if (!bad && renameat(dir,".transaction",dir,id)) bad=1;
    if (bad) (void)unlinkat(dir,".transaction",0);
    else if (fsync(dir)) bad=1;
    rc=bad ? LMB_CHAT_UNSAFE : LMB_CHAT_OK;
done:
    close(lock); return rc;
}
#endif
