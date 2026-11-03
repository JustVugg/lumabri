/* Locked private tracker state. The binding parser and generation arithmetic
 * are shared; this boundary owns OS file handles and durable flushes. */
#ifndef LMB_TRACKER_STATE_H
#define LMB_TRACKER_STATE_H
#ifdef _WIN32
#include "lumabri_private_file_windows.h"
static FILE *tracker_state_open(const char *path,int write,int create) {
    return lmb_private_stream(path,write,create);
}
static int tracker_state_flush(FILE *stream) {return lmb_private_stream_flush(stream);}
static void tracker_state_close(FILE *stream) {lmb_private_stream_close(stream);}
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static FILE *tracker_state_open(const char *path,int write,int create) {
    int flags=(write ? O_RDWR : O_RDONLY)|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK;
    if (create) flags|=O_CREAT;
    int fd=open(path,flags,0600);if (fd<0) return NULL;
    struct stat s;
    if (fstat(fd,&s) || !S_ISREG(s.st_mode) || s.st_uid!=geteuid() || (s.st_mode&022) ||
        flock(fd,write ? LOCK_EX : LOCK_SH)) {close(fd);errno=EACCES;return NULL;}
    FILE *stream=fdopen(fd,write ? "r+b" : "rb");
    if (!stream) close(fd);
    return stream;
}
static int tracker_state_flush(FILE *stream) {return fflush(stream) || fsync(fileno(stream)) ? -1 : 0;}
static void tracker_state_close(FILE *stream) {fclose(stream);}
#endif
#endif
