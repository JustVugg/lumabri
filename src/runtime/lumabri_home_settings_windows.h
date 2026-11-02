/* Native household configuration: same data/format as the POSIX launcher,
 * stored privately on this user's local drive and replaced atomically. */
#ifndef LMB_HOME_SETTINGS_WINDOWS_H
#define LMB_HOME_SETTINGS_WINDOWS_H
#include "src/ui/lumabri_home_settings.h"
#include "lumabri_private_file_windows.h"

static int home_settings_path_windows(char *out,size_t cap) {
    return lmb_private_default_path(out,cap,"home.conf");
}
static int home_settings_load_windows(HomeSettings *out) {
    char path[1200],models[512];
    if (lmb_private_default_path(models,sizeof models,"models") || home_settings_path_windows(path,sizeof path)) return -1;
    home_settings_defaults(out,models);
    HANDLE file=lmb_private_open_sharing(path,0,0,NULL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
    if (file==INVALID_HANDLE_VALUE) return errno==ENOENT ? 0 : -1;
    LARGE_INTEGER size;char text[4096];DWORD got=0;
    int bad=!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>=(LONGLONG)sizeof text;
    if (!bad) bad=!ReadFile(file,text,(DWORD)size.QuadPart,&got,NULL) || got!=size.QuadPart || memchr(text,0,got)!=NULL;
    CloseHandle(file);
    if (bad) {SecureZeroMemory(text,sizeof text);return -1;}
    text[got]=0;HomeSettings next=*out;
    char *line=text;
    while (*line) {
        char *end=strchr(line,'\n');if (end) *end=0;
        if (*line && home_settings_line(&next,line)<0) {bad=1;break;}
        if (!end) break;
        line=end+1;
    }
    if (!bad) *out=next;
    SecureZeroMemory(&next,sizeof next);SecureZeroMemory(text,sizeof text);
    return bad ? -1 : 0;
}

static int home_settings_save_windows(const HomeSettings *settings) {
    char path[1200],lock_path[1200],text[4096];
    int length=home_settings_format(settings,text,sizeof text);
    if (length<0 || home_settings_path_windows(path,sizeof path) ||
        lmb_private_default_path(lock_path,sizeof lock_path,"home-settings.lock")) return -1;
    HANDLE guard=lmb_private_open(lock_path,1,1,NULL);
    if (guard==INVALID_HANDLE_VALUE) {SecureZeroMemory(text,sizeof text);return -1;}
    int rc=-1;
    if (lmb_private_lock(guard,1)) goto done;
    LmbPrivatePath target;LmbPrivateSecurity security;
    if (lmb_private_path(path,&target)) goto unlocked;
    if (lmb_private_security(&security)) {lmb_private_path_close(&target);goto unlocked;}
    uint8_t random[16];char hex[33];wchar_t temporary[1100];
    lmb_random(random,sizeof random);lmb_hex(hex,random,sizeof random);
    int n=swprintf(temporary,1100,L"%ls.%hs.tmp",target.path,hex);
    HANDLE file=n<0 ? INVALID_HANDLE_VALUE : CreateFileW(temporary,GENERIC_WRITE,0,
        &security.attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file!=INVALID_HANDLE_VALUE) {
        DWORD written;
        rc=!WriteFile(file,text,(DWORD)length,&written,NULL) || written!=(DWORD)length || !FlushFileBuffers(file);
        if (!CloseHandle(file)) rc=-1;
        if (!rc) {
            HANDLE old=lmb_private_open(path,0,0,NULL);
            if (old!=INVALID_HANDLE_VALUE) {
                CloseHandle(old);
                if (!ReplaceFileW(target.path,temporary,NULL,0,NULL,NULL)) rc=-1;
            } else if (errno!=ENOENT || !MoveFileExW(temporary,target.path,MOVEFILE_WRITE_THROUGH)) rc=-1;
        }
        if (rc) DeleteFileW(temporary);
    }
    lmb_private_security_free(&security);lmb_private_path_close(&target);
unlocked:
    lmb_private_unlock(guard);
done:
    CloseHandle(guard);SecureZeroMemory(text,sizeof text);return rc ? -1 : 0;
}

static int home_settings_activate_windows(const HomeSettings *s) {
    /* _putenv_s updates both the CRT view used by authentication and the
     * process environment inherited by the native background keeper. */
    return _putenv_s("LUMABRI_TOKEN",s->token) ? -1 : 0;
}

static int home_connection_check_windows(const char *address) {
    LmbSocket fd=lmb_connect_ms_io(address,1500,1500);
    if (fd==LMB_INVALID_SOCKET) return -1;
    LmbMsg reply={0};
    int bad=lmb_auth(fd) || lmb_send(fd,LMB_PING,NULL,0,NULL,0) ||
        lmb_recv_bounded(fd,&reply,256,1500) || reply.op!=LMB_OK || reply.body_len || reply.pay_len;
    lmb_msg_free(&reply);lmb_close(fd);return bad ? -2 : 0;
}

static int home_settings_join_windows(HomeSettings *current,const HomeSettings *next) {
    HomeServiceSnapshot running;
    const char *roles[]={"donor","tracker","prepare"};
    for (unsigned i=0;i<3;i++) if (!home_service_query(roles[i],HOME_SVC_STATUS,NULL,&running)) return -4;
    if (!next->tracker[0] || !next->token[0] || next->owner || home_settings_activate_windows(next)) return -2;
    int rc=home_connection_check_windows(next->tracker);
    if (!rc && home_settings_save_windows(next)) rc=-3;
    if (rc) (void)home_settings_activate_windows(current);
    else *current=*next;
    return rc;
}
#endif
