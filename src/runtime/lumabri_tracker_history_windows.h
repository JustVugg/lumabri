/* Native storage for the shared exact tracker history format. Runtime
 * liveness may expire; identity and checkpoint evidence never do. */
#ifndef LMB_TRACKER_HISTORY_WINDOWS_H
#define LMB_TRACKER_HISTORY_WINDOWS_H
#include "lumabri_private_file_windows.h"
typedef struct {char path[1200];HANDLE directory,lock;LmbPrivatePath parents;} TrackerHistory;

static void tracker_history_end(TrackerHistory *h) {
    if (h->lock!=INVALID_HANDLE_VALUE) {lmb_private_unlock(h->lock);CloseHandle(h->lock);}
    if (h->directory!=INVALID_HANDLE_VALUE) CloseHandle(h->directory);
    lmb_private_path_close(&h->parents);
    h->directory=h->lock=INVALID_HANDLE_VALUE;
}
static int tracker_history_begin(const char *bindings,TrackerHistory *h) {
    memset(h,0,sizeof *h);h->directory=h->lock=INVALID_HANDLE_VALUE;
    if (!bindings || !*bindings || snprintf(h->path,sizeof h->path,"%s.history",bindings)>=(int)sizeof h->path ||
        lmb_private_mkdir(h->path) || lmb_private_path(h->path,&h->parents)) return -1;
    LmbPrivateSecurity security;
    if (lmb_private_security(&security)) goto fail;
    h->directory=CreateFileW(h->parents.path,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    int bad=h->directory==INVALID_HANDLE_VALUE || lmb_private_validate(h->directory,security.user->User.Sid,1);
    lmb_private_security_free(&security);if (bad) goto fail;
    char lock[1280];
    if (snprintf(lock,sizeof lock,"%s\\lock",h->path)>=(int)sizeof lock) goto fail;
    h->lock=lmb_private_open(lock,1,1,NULL);
    if (h->lock==INVALID_HANDLE_VALUE || lmb_private_lock(h->lock,1)) goto fail;
    return 0;
fail:
    tracker_history_end(h);return -1;
}
static int tracker_history_name_windows(const char *name) {
    if (!name || strlen(name)!=70 || strcmp(name+64,".entry")) return -1;
    for (unsigned i=0;i<64;i++) if (!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='f'))) return -1;
    return 0;
}
/* Read older flat records first. New keys use the same history-v1/xx/yy
 * namespace as the POSIX index, without an in-memory lifetime limit. */
static int tracker_history_path_windows(TrackerHistory *h,const char *name,int create,char out[1400]) {
    if (tracker_history_name_windows(name)) {errno=EINVAL;return -1;}
    if (snprintf(out,1400,"%s\\%s",h->path,name)>=1400) return -1;
    HANDLE flat=lmb_private_open(out,0,0,NULL);
    if (flat!=INVALID_HANDLE_VALUE) {CloseHandle(flat);return 0;}
    if (errno!=ENOENT) return -1;
    char path[1400];int n=snprintf(path,sizeof path,"%s\\history-v1",h->path);
    if (n<0 || (size_t)n>=sizeof path || (create && lmb_private_mkdir(path))) return -1;
    if ((size_t)n+7>=sizeof path) return -1;
    snprintf(path+n,sizeof path-(size_t)n,"\\%.2s",name);n+=3;
    if (create && lmb_private_mkdir(path)) return -1;
    snprintf(path+n,sizeof path-(size_t)n,"\\%.2s",name+2);n+=3;
    if (create && lmb_private_mkdir(path)) return -1;
    return snprintf(out,1400,"%s\\%s",path,name)>=1400 ? -1 : 0;
}
static int tracker_history_read(TrackerHistory *h,const char *name,LmbBuf *out) {
    memset(out,0,sizeof *out);char path[1400];
    if (tracker_history_path_windows(h,name,0,path)) return -1;
    HANDLE file=lmb_private_open_sharing(path,0,0,NULL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
    if (file==INVALID_HANDLE_VALUE) return errno==ENOENT ? 1 : -1;
    LARGE_INTEGER size;DWORD got=0;
    int bad=!GetFileSizeEx(file,&size) || size.QuadPart<40 || size.QuadPart>LMB_MAX_BODY+2048u;
    uint8_t *bytes=bad ? NULL : malloc((size_t)size.QuadPart);if (!bytes) bad=1;
    if (!bad) bad=!ReadFile(file,bytes,(DWORD)size.QuadPart,&got,NULL) || got!=size.QuadPart;
    CloseHandle(file);
    if (!bad) {
        uint8_t digest[32];tracker_history_digest(name,bytes,(size_t)size.QuadPart-32,digest);
        bad=memcmp(bytes,"LMB-TRH1",8) || memcmp(digest,bytes+size.QuadPart-32,32);
        if (!bad) bad=lmb_buf_bytes(out,bytes+8,(size_t)size.QuadPart-40);
    }
    free(bytes);
    if (bad) {free(out->p);memset(out,0,sizeof *out);return -1;}
    return 0;
}
static int tracker_history_write(TrackerHistory *h,const char *name,const LmbBuf *payload) {
    char path[1400];
    if (payload->len>LMB_MAX_BODY+2008u || tracker_history_path_windows(h,name,1,path)) return -1;
    LmbPrivatePath target;LmbPrivateSecurity security;
    if (lmb_private_path(path,&target)) return -1;
    if (lmb_private_security(&security)) {lmb_private_path_close(&target);return -1;}
    LmbBuf b={0};uint8_t digest[32];
    int bad=lmb_buf_bytes(&b,"LMB-TRH1",8) || lmb_buf_bytes(&b,payload->p,payload->len);
    if (!bad) {tracker_history_digest(name,b.p,b.len,digest);bad=lmb_buf_bytes(&b,digest,32);}
    uint8_t nonce[16];char hex[33];lmb_random(nonce,sizeof nonce);lmb_hex(hex,nonce,sizeof nonce);
    wchar_t pending[1100];int n=swprintf(pending,1100,L"%ls.%hs.pending",target.path,hex);
    HANDLE file=bad || n<0 ? INVALID_HANDLE_VALUE : CreateFileW(pending,GENERIC_WRITE,0,&security.attributes,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) bad=1;
    else {
        DWORD written;
        bad=!WriteFile(file,b.p,(DWORD)b.len,&written,NULL) || written!=b.len || !FlushFileBuffers(file);
        if (!CloseHandle(file)) bad=1;
        if (!bad) {
            HANDLE old=lmb_private_open(path,0,0,NULL);
            if (old!=INVALID_HANDLE_VALUE) CloseHandle(old);
            else if (errno!=ENOENT) bad=1;
            if (!bad && !MoveFileExW(pending,target.path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) bad=1;
        }
        if (bad) DeleteFileW(pending);
    }
    free(b.p);lmb_private_security_free(&security);lmb_private_path_close(&target);return bad ? -1 : 0;
}
#endif
