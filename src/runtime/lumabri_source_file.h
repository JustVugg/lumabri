/* Checkpoint publisher filesystem boundary. Network sockets never use these
 * descriptors. Windows paths are UTF-8 and files use 64-bit positioned I/O. */
#ifndef LUMABRI_SOURCE_FILE_H
#define LUMABRI_SOURCE_FILE_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    uint64_t size, mtime_ns, ctime_ns;
    int regular, directory;
} LmbSourceStat;
typedef int (*LmbSourceVisit)(const char *, void *);

#ifdef _WIN32
#include "lumabri_private_file_windows.h"

static int lmb_source_info(HANDLE h,LmbSourceStat *s) {
    FILE_BASIC_INFO basic;FILE_STANDARD_INFO standard;
    if (GetFileType(h)!=FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(h,FileBasicInfo,&basic,sizeof basic) ||
        !GetFileInformationByHandleEx(h,FileStandardInfo,&standard,sizeof standard) ||
        (basic.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {errno=EACCES;return -1;}
    memset(s,0,sizeof *s);s->size=(uint64_t)standard.EndOfFile.QuadPart;
    /* Epoch is deliberately unchanged: sidecars are local caches, not wire data. */
    s->mtime_ns=(uint64_t)basic.LastWriteTime.QuadPart*100;
    s->ctime_ns=(uint64_t)basic.ChangeTime.QuadPart*100;
    s->directory=standard.Directory!=0;s->regular=!s->directory;
    return 0;
}

static int lmb_source_open(const char *path,int flags) {
    LmbPrivatePath p;if (lmb_private_path(path,&p)) return -1;
    int write=(flags&O_WRONLY)||(flags&O_RDWR);
    HANDLE h=CreateFileW(p.path,GENERIC_READ|(write?GENERIC_WRITE:0),FILE_SHARE_READ,
        NULL,flags&O_CREAT ? (flags&O_EXCL?CREATE_NEW:OPEN_ALWAYS) : OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OVERLAPPED,NULL);
    DWORD error=GetLastError();
    lmb_private_path_close(&p);
    if (h==INVALID_HANDLE_VALUE) return lmb_private_error(error);
    LmbSourceStat st;
    if (lmb_source_info(h,&st) || !st.regular) {CloseHandle(h);errno=EACCES;return -1;}
    if (flags&O_TRUNC) {
        FILE_END_OF_FILE_INFO end={0};
        if (!SetFileInformationByHandle(h,FileEndOfFileInfo,&end,sizeof end)) {
            DWORD error=GetLastError();CloseHandle(h);return lmb_private_error(error);
        }
    }
    int fd=_open_osfhandle((intptr_t)h,_O_BINARY|_O_NOINHERIT|(write?_O_RDWR:_O_RDONLY));
    if (fd<0) CloseHandle(h);
    return fd;
}
static int lmb_source_fstat(int fd,LmbSourceStat *s) {
    return lmb_source_info((HANDLE)_get_osfhandle(fd),s);
}
static int lmb_source_stat(const char *path,LmbSourceStat *s) {
    LmbPrivatePath p;if (lmb_private_path(path,&p)) return -1;
    HANDLE h=CreateFileW(p.path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    DWORD error=GetLastError();
    lmb_private_path_close(&p);
    if (h==INVALID_HANDLE_VALUE) return lmb_private_error(error);
    int rc=lmb_source_info(h,s);CloseHandle(h);return rc;
}
static ptrdiff_t lmb_source_io(int fd,void *data,size_t size,uint64_t at,int write) {
    if (size>MAXDWORD || at>INT64_MAX || size>(uint64_t)INT64_MAX-at) {errno=EINVAL;return -1;}
    HANDLE h=(HANDLE)_get_osfhandle(fd);OVERLAPPED io={0};DWORD done=0;
    io.Offset=(DWORD)at;io.OffsetHigh=(DWORD)(at>>32);
    io.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!io.hEvent) return lmb_private_error(GetLastError());
    BOOL ok=write?WriteFile(h,data,(DWORD)size,&done,&io):ReadFile(h,data,(DWORD)size,&done,&io);
    DWORD error=ok?ERROR_SUCCESS:GetLastError();
    if (!ok && error==ERROR_IO_PENDING) {
        ok=GetOverlappedResult(h,&io,&done,TRUE);error=ok?ERROR_SUCCESS:GetLastError();
    }
    CloseHandle(io.hEvent);
    if (!write && error==ERROR_HANDLE_EOF) return 0;
    return ok?(ptrdiff_t)done:lmb_private_error(error);
}
static ptrdiff_t lmb_source_pread(int fd,void *data,size_t size,uint64_t at) {
    return lmb_source_io(fd,data,size,at,0);
}
static ptrdiff_t lmb_source_pwrite(int fd,const void *data,size_t size,uint64_t at) {
    return lmb_source_io(fd,(void *)data,size,at,1);
}
static int lmb_source_close(int fd) {return _close(fd);}
static int lmb_source_sync(int fd) {
    return FlushFileBuffers((HANDLE)_get_osfhandle(fd))?0:lmb_private_error(GetLastError());
}
static int lmb_source_unlink(const char *path) {
    LmbPrivatePath p;if (lmb_private_path(path,&p)) return -1;
    int rc=DeleteFileW(p.path)?0:lmb_private_error(GetLastError());
    lmb_private_path_close(&p);return rc;
}
static int lmb_source_rename(const char *from,const char *to) {
    LmbPrivatePath a,b;if (lmb_private_path(from,&a)) return -1;
    if (lmb_private_path(to,&b)) {lmb_private_path_close(&a);return -1;}
    int rc=MoveFileExW(a.path,b.path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?0:
        lmb_private_error(GetLastError());
    lmb_private_path_close(&b);lmb_private_path_close(&a);return rc;
}
static int lmb_source_mkdir(const char *path) {
    LmbPrivatePath p;if (lmb_private_path(path,&p)) return -1;
    int rc=CreateDirectoryW(p.path,NULL)?0:lmb_private_error(GetLastError());
    lmb_private_path_close(&p);return rc;
}
/* Metadata streams are synchronous; positioned checkpoint handles above are
 * overlapped, so concurrent READ requests never share a seek cursor. */
static FILE *lmb_source_fopen(const char *path,const char *mode) {
    if (strcmp(mode,"rb") && strcmp(mode,"r")) {errno=EINVAL;return NULL;}
    LmbPrivatePath p;if (lmb_private_path(path,&p)) return NULL;
    HANDLE h=CreateFileW(p.path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    DWORD error=GetLastError();
    lmb_private_path_close(&p);
    if (h==INVALID_HANDLE_VALUE) {lmb_private_error(error);return NULL;}
    LmbSourceStat st;
    if (lmb_source_info(h,&st) || !st.regular) {CloseHandle(h);return NULL;}
    int fd=_open_osfhandle((intptr_t)h,_O_RDONLY|_O_BINARY|_O_NOINHERIT);
    if (fd<0) {CloseHandle(h);return NULL;}
    FILE *fp=_fdopen(fd,"rb");if (!fp) _close(fd);return fp;
}
static int lmb_source_list(const char *path,LmbSourceVisit visit,void *context) {
    LmbPrivatePath p;if (lmb_private_path(path,&p)) return -1;
    HANDLE directory=CreateFileW(p.path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    LmbSourceStat st;int rc=-1;wchar_t pattern[1030];
    if (directory==INVALID_HANDLE_VALUE || lmb_source_info(directory,&st) || !st.directory) goto done;
    if (swprintf(pattern,sizeof pattern/sizeof *pattern,L"%ls\\*",p.path)<0) goto done;
    WIN32_FIND_DATAW entry;HANDLE search=FindFirstFileW(pattern,&entry);
    if (search==INVALID_HANDLE_VALUE) {rc=GetLastError()==ERROR_FILE_NOT_FOUND?0:-1;goto done;}
    rc=0;
    do {
        char name[3072];
        if (entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) {rc=-1;errno=EACCES;break;}
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,entry.cFileName,-1,
                                 name,sizeof name,NULL,NULL)) {rc=-1;break;}
        if ((rc=visit(name,context))) break;
    } while (FindNextFileW(search,&entry));
    if (!rc && GetLastError()!=ERROR_NO_MORE_FILES) rc=-1;
    FindClose(search);
done:
    if (directory!=INVALID_HANDLE_VALUE) CloseHandle(directory);
    lmb_private_path_close(&p);return rc;
}
#else
#include <dirent.h>
#include <unistd.h>
static void lmb_source_stat_copy(LmbSourceStat *out,const struct stat *st) {
    memset(out,0,sizeof *out);out->size=(uint64_t)st->st_size;
#ifdef __APPLE__
    out->mtime_ns=(uint64_t)st->st_mtimespec.tv_sec*1000000000ull+st->st_mtimespec.tv_nsec;
    out->ctime_ns=(uint64_t)st->st_ctimespec.tv_sec*1000000000ull+st->st_ctimespec.tv_nsec;
#else
    out->mtime_ns=(uint64_t)st->st_mtim.tv_sec*1000000000ull+st->st_mtim.tv_nsec;
    out->ctime_ns=(uint64_t)st->st_ctim.tv_sec*1000000000ull+st->st_ctim.tv_nsec;
#endif
    out->regular=S_ISREG(st->st_mode);out->directory=S_ISDIR(st->st_mode);
}
static int lmb_source_stat(const char *path,LmbSourceStat *s) {
    struct stat st;if (stat(path,&st)) return -1;lmb_source_stat_copy(s,&st);return 0;
}
static int lmb_source_fstat(int fd,LmbSourceStat *s) {
    struct stat st;if (fstat(fd,&st)) return -1;lmb_source_stat_copy(s,&st);return 0;
}
static int lmb_source_open(const char *path,int flags) {return open(path,flags|O_CLOEXEC,0644);}
static ptrdiff_t lmb_source_pread(int fd,void *data,size_t size,uint64_t at) {return pread(fd,data,size,(off_t)at);}
static ptrdiff_t lmb_source_pwrite(int fd,const void *data,size_t size,uint64_t at) {return pwrite(fd,data,size,(off_t)at);}
static int lmb_source_close(int fd) {return close(fd);}
static int lmb_source_sync(int fd) {return fsync(fd);}
static int lmb_source_unlink(const char *path) {return unlink(path);}
static int lmb_source_rename(const char *a,const char *b) {return rename(a,b);}
static int lmb_source_mkdir(const char *path) {return mkdir(path,0755);}
static FILE *lmb_source_fopen(const char *path,const char *mode) {return fopen(path,mode);}
static int lmb_source_list(const char *path,LmbSourceVisit visit,void *context) {
    DIR *d=opendir(path);if (!d) return -1;struct dirent *entry;int rc=0;
    for (;;) {
        errno=0;entry=readdir(d);
        if (!entry) {if (errno) rc=-1;break;}
        if ((rc=visit(entry->d_name,context))) break;
    }
    closedir(d);return rc;
}
#endif
#endif
