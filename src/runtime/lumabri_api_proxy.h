/* Private HTTP upstream for an explicitly configured local TLS terminator.
 * No network listener, forwarded-header trust, CORS relaxation or TLS code.
 * The proxy must run as the same OS user and still supply each API credential.
 * Includes follow lumabri_api_access.h and the normal POSIX controller headers. */
#ifndef LMB_API_PROXY_H
#define LMB_API_PROXY_H
#include <sys/un.h>

typedef struct {
    int fd, lock;
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    dev_t device;
    ino_t inode;
} LmbApiProxy;

static void lmb_api_proxy_close(LmbApiProxy *proxy) {
    if (proxy->fd>=0) close(proxy->fd);
    struct stat observed;
    if (proxy->inode && !lstat(proxy->path,&observed) && S_ISSOCK(observed.st_mode) &&
        observed.st_dev==proxy->device && observed.st_ino==proxy->inode && observed.st_uid==geteuid())
        (void)unlink(proxy->path);
    if (proxy->lock>=0) close(proxy->lock);
    proxy->fd=proxy->lock=-1; proxy->inode=0;
}
static int lmb_api_proxy_listen(LmbApiProxy *proxy, int access, const char *directory) {
    memset(proxy,0,sizeof *proxy); proxy->fd=proxy->lock=-1;
    struct stat held, named, prior;
    char canonical[PATH_MAX];
    if (lmb_api_private_fd(access,1) || fstat(access,&held) || !realpath(directory,canonical) ||
        lstat(canonical,&named) || held.st_dev!=named.st_dev || held.st_ino!=named.st_ino) return -1;
    int n=snprintf(proxy->path,sizeof proxy->path,"%s/proxy.sock",canonical);
    if (n<0 || (size_t)n>=sizeof proxy->path) { errno=ENAMETOOLONG; return -1; }
    proxy->lock=lmb_api_open_lockfile(access,"proxy.lock");
    if (proxy->lock<0 || lmb_api_private_fd(proxy->lock,0) || flock(proxy->lock,LOCK_EX|LOCK_NB)) goto fail;
    if (!fstatat(access,"proxy.sock",&prior,AT_SYMLINK_NOFOLLOW)) {
        /* Only a stale socket under this exact private directory. Never
         * replace a regular file, symlink, foreign owner or active listener. */
        if (!S_ISSOCK(prior.st_mode) || prior.st_uid!=geteuid() || (prior.st_mode&077)) { errno=EACCES; goto fail; }
        int probe=socket(AF_UNIX,SOCK_STREAM,0); if (probe<0) goto fail;
        if (fcntl(probe,F_SETFL,O_NONBLOCK)) { int saved=errno; close(probe); errno=saved; goto fail; }
        struct sockaddr_un address={0}; address.sun_family=AF_UNIX;
        memcpy(address.sun_path,proxy->path,strlen(proxy->path)+1);
        int connected=connect(probe,(struct sockaddr *)&address,sizeof address), saved=errno;
        close(probe);
        if (!connected || (saved!=ECONNREFUSED && saved!=ENOENT)) { errno=EADDRINUSE; goto fail; }
        if (unlinkat(access,"proxy.sock",0)) goto fail;
    } else if (errno!=ENOENT) goto fail;
    proxy->fd=socket(AF_UNIX,SOCK_STREAM,0); if (proxy->fd<0) goto fail;
    struct sockaddr_un address={0}; address.sun_family=AF_UNIX;
    memcpy(address.sun_path,proxy->path,strlen(proxy->path)+1);
    if (fcntl(proxy->fd,F_SETFD,FD_CLOEXEC) || bind(proxy->fd,(struct sockaddr *)&address,sizeof address)) goto fail;
    if (lstat(proxy->path,&prior) || !S_ISSOCK(prior.st_mode) || prior.st_uid!=geteuid()) goto fail;
    proxy->device=prior.st_dev; proxy->inode=prior.st_ino;
    if (chmod(proxy->path,0600) || listen(proxy->fd,16)) goto fail;
    return 0;
fail: {
    int saved=errno; lmb_api_proxy_close(proxy); errno=saved; return -1;
}
}
#endif
