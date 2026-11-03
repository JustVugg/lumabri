/* Bounded private records, with atomic replacement and no reparse traversal. */
#ifndef LMB_PRIVATE_BLOB_WINDOWS_H
#define LMB_PRIVATE_BLOB_WINDOWS_H
#include "lumabri_private_file_windows.h"
#include "src/crypto/lumabri_random.h"
static int lmb_private_blob_read(const char *path,void *bytes,size_t cap,size_t *length) {
    *length=0;
    HANDLE h=lmb_private_open_sharing(path,0,0,NULL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
    if (h==INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size;DWORD got=0;
    int bad=!GetFileSizeEx(h,&size) || size.QuadPart<=0 || (uint64_t)size.QuadPart>cap || cap>MAXDWORD;
    if (!bad) bad=!ReadFile(h,bytes,(DWORD)size.QuadPart,&got,NULL) || got!=size.QuadPart;
    if (!CloseHandle(h)) bad=1;
    if (!bad) *length=got;
    return bad ? -1 : 0;
}
static int lmb_private_blob_write(const char *path,const void *bytes,size_t length) {
    if (!bytes || !length || length>MAXDWORD) return -1;
    LmbPrivatePath target;LmbPrivateSecurity security;
    if (lmb_private_path(path,&target)) return -1;
    if (lmb_private_security(&security)) {lmb_private_path_close(&target);return -1;}
    uint8_t random[16];char hex[33];wchar_t temp[1100];
    lmb_random(random,sizeof random);lmb_hex(hex,random,sizeof random);
    int n=swprintf(temp,1100,L"%ls.%hs.tmp",target.path,hex),rc=-1;
    HANDLE h=n<0 ? INVALID_HANDLE_VALUE : CreateFileW(temp,GENERIC_WRITE,0,&security.attributes,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (h!=INVALID_HANDLE_VALUE) {
        DWORD written;
        rc=!WriteFile(h,bytes,(DWORD)length,&written,NULL) || written!=length || !FlushFileBuffers(h);
        if (!CloseHandle(h)) rc=-1;
        if (!rc) {
            HANDLE old=lmb_private_open(path,0,0,NULL);
            if (old!=INVALID_HANDLE_VALUE) {
                CloseHandle(old);if (!ReplaceFileW(target.path,temp,NULL,0,NULL,NULL)) rc=-1;
            } else if (errno!=ENOENT || !MoveFileExW(temp,target.path,MOVEFILE_WRITE_THROUGH)) rc=-1;
        }
        if (rc) DeleteFileW(temp);
    }
    lmb_private_security_free(&security);lmb_private_path_close(&target);return rc ? -1 : 0;
}
#endif
