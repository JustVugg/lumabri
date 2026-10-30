/* Native compute admission. The connected private pipe owns the permit;
 * a process exit releases it without trusting a PID or expiring live work. */
#ifndef LUMABRI_COMPUTE_WINDOWS_H
#define LUMABRI_COMPUTE_WINDOWS_H
#include "src/runtime/lumabri_pipe_windows.h"
#include <pthread.h>
#include <stdatomic.h>

typedef HANDLE LmbComputePermit;
#define LMB_COMPUTE_INVALID INVALID_HANDLE_VALUE
static inline uint64_t lmb_compute_now(void) { return GetTickCount64(); }
static inline uint32_t lmb_compute_remaining(uint64_t began,uint32_t budget) {
    uint64_t now=lmb_compute_now();
    return now<began || now-began>=budget ? 0 : budget-(uint32_t)(now-began);
}
static inline void lmb_compute_release(LmbComputePermit *permit) {
    if (*permit!=LMB_COMPUTE_INVALID) CloseHandle(*permit);
    *permit=LMB_COMPUTE_INVALID;
}
static inline int lmb_compute_acquire(LmbComputePermit *permit,uint32_t wait_ms,
                                      int (*cancel)(void *),void *opaque) {
    *permit=LMB_COMPUTE_INVALID;
    /* The inherited Windows environment is UTF-16. CRT getenv converts to
     * the system ANSI codepage, which corrupts non-ASCII profile paths. */
    wchar_t wide[2048]; char path[6144];
    DWORD length=GetEnvironmentVariableW(L"LUMABRI_COMPUTE_SOCKET",wide,2048);
    if (!length) {
        DWORD error=GetLastError();
        if (error==ERROR_ENVVAR_NOT_FOUND || error==ERROR_SUCCESS) return 0;
        return lmb_private_error(error);
    }
    if (length>=2048 || !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,
            path,sizeof path,NULL,NULL)) { errno=EINVAL; return -1; }
    char name[200];
    if (!wait_ms || wait_ms>300000 || lmb_pipe_name(path,"compute",name,sizeof name)) {
        errno=EINVAL; return -1;
    }
    uint64_t began=lmb_compute_now(),deadline=began+wait_ms;
    HANDLE pipe=INVALID_HANDLE_VALUE;
    while ((pipe=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,NULL))==INVALID_HANDLE_VALUE) {
        DWORD error=GetLastError();
        if (cancel && cancel(opaque)) { errno=ECANCELED; return -1; }
        if (error!=ERROR_PIPE_BUSY) return lmb_private_error(error);
        if (!lmb_compute_remaining(began,wait_ms)) { errno=ETIMEDOUT; return -1; }
        Sleep(5);
    }
    uint8_t request[8]={'L','C','P',1},grant=0;
    for (unsigned i=0;i<4;i++) request[4+i]=(uint8_t)(wait_ms>>(8*i));
    int rc=-1;
    if (cancel && cancel(opaque)) { errno=ECANCELED; goto done; }
    if (lmb_pipe_server(pipe) ||
        lmb_pipe_io(pipe,request,sizeof request,1,deadline)) goto done;
    OVERLAPPED read={0}; read.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!read.hEvent) goto done;
    DWORD bytes=0;
    BOOL ok=ReadFile(pipe,&grant,1,&bytes,&read);
    int pending=!ok && GetLastError()==ERROR_IO_PENDING;
    while (pending) {
        if (cancel && cancel(opaque)) { errno=ECANCELED; break; }
        uint32_t remaining=lmb_compute_remaining(began,wait_ms);
        if (!remaining) { errno=ETIMEDOUT; break; }
        DWORD ready=WaitForSingleObject(read.hEvent,remaining<20 ? remaining : 20);
        if (ready==WAIT_TIMEOUT) continue;
        if (ready!=WAIT_OBJECT_0) break;
        ok=GetOverlappedResult(pipe,&read,&bytes,FALSE); pending=0;
    }
    if (pending) {
        CancelIoEx(pipe,&read);
        (void)GetOverlappedResult(pipe,&read,&bytes,TRUE);
    } else if (ok && bytes==1 && grant==1 && lmb_compute_remaining(began,wait_ms) &&
               !(cancel && cancel(opaque))) rc=0;
    CloseHandle(read.hEvent);
done:
    if (rc) { CloseHandle(pipe); if (!errno) errno=ECONNRESET; }
    else *permit=pipe;
    return rc;
}

#ifdef LMB_COMPUTE_BROKER_SERVER
typedef struct {
    HANDLE pipe;
    OVERLAPPED read;
    int pending,state;
    uint8_t request[8],extra;
    size_t received;
    uint64_t ticket,deadline;
} LmbComputeClient;
typedef struct {
    HANDLE listener,wake;
    OVERLAPPED connect;
    int connecting,started;
    pthread_t thread;
    _Atomic int stop;
    _Atomic uint32_t queued,active;
    _Atomic uint64_t grants;
    char path[200];
} LmbComputeBroker;

static inline void lmb_compute_drop(LmbComputeClient *c) {
    if (c->pipe!=INVALID_HANDLE_VALUE) {
        if (c->pending) {
            DWORD ignored; CancelIoEx(c->pipe,&c->read);
            (void)GetOverlappedResult(c->pipe,&c->read,&ignored,TRUE);
        }
        DisconnectNamedPipe(c->pipe); CloseHandle(c->pipe);
    }
    if (c->read.hEvent) CloseHandle(c->read.hEvent);
    memset(c,0,sizeof *c); c->pipe=INVALID_HANDLE_VALUE;
}
static inline int lmb_compute_read_next(LmbComputeClient *c) {
    ResetEvent(c->read.hEvent);
    DWORD count=0;
    BOOL ok=ReadFile(c->pipe,c->state ? &c->extra : c->request+c->received,
        c->state ? 1 : (DWORD)(sizeof c->request-c->received),&count,&c->read);
    if (!ok && GetLastError()!=ERROR_IO_PENDING) return -1;
    c->pending=1;
    if (ok) SetEvent(c->read.hEvent);
    return 0;
}
static inline int lmb_compute_listener(LmbComputeBroker *b,int first) {
    LmbPrivateSecurity security;
    if (lmb_private_security(&security)) return -1;
    b->listener=CreateNamedPipeA(b->path,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|
        (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
        LMB_COMPUTE_PENDING_MAX+2,1024,1024,0,&security.attributes);
    DWORD error=GetLastError();
    lmb_private_security_free(&security);
    if (b->listener==INVALID_HANDLE_VALUE) return lmb_private_error(error);
    ResetEvent(b->connect.hEvent);
    BOOL ok=ConnectNamedPipe(b->listener,&b->connect);
    error=ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error!=ERROR_IO_PENDING && error!=ERROR_PIPE_CONNECTED) {
        CloseHandle(b->listener); b->listener=INVALID_HANDLE_VALUE; return lmb_private_error(error);
    }
    b->connecting=error==ERROR_IO_PENDING;
    if (!b->connecting) SetEvent(b->connect.hEvent);
    return 0;
}
static void *lmb_compute_serve(void *opaque) {
    LmbComputeBroker *b=opaque;
    LmbComputeClient clients[LMB_COMPUTE_PENDING_MAX]={0};
    for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX;i++) clients[i].pipe=INVALID_HANDLE_VALUE;
    uint64_t ticket=0;
    while (!atomic_load(&b->stop)) {
        HANDLE events[LMB_COMPUTE_PENDING_MAX+2]={b->wake,b->connect.hEvent}; DWORD count=2;
        for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX;i++)
            if (clients[i].pipe!=INVALID_HANDLE_VALUE) events[count++]=clients[i].read.hEvent;
        DWORD ready=WaitForMultipleObjects(count,events,FALSE,50);
        if (ready==WAIT_FAILED || ready==WAIT_OBJECT_0) break;
        uint64_t now=lmb_compute_now();
        if (WaitForSingleObject(b->connect.hEvent,0)==WAIT_OBJECT_0) {
            DWORD ignored=0;
            if (b->connecting && !GetOverlappedResult(b->listener,&b->connect,&ignored,FALSE)) break;
            b->connecting=0;
            HANDLE accepted=b->listener; b->listener=INVALID_HANDLE_VALUE;
            /* Keep at least one instance alive while replacing the listener.
             * A rejected first client must not open a singleton-name gap. */
            if (lmb_compute_listener(b,0)) {
                DisconnectNamedPipe(accepted); CloseHandle(accepted); break;
            }
            int slot=-1;
            for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX;i++)
                if (clients[i].pipe==INVALID_HANDLE_VALUE) { slot=(int)i; break; }
            if (slot>=0) {
                LmbComputeClient *c=&clients[slot]; c->pipe=accepted; c->deadline=now+2000;
                c->read.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
                if (!c->read.hEvent || lmb_compute_read_next(c)) lmb_compute_drop(c);
            } else { DisconnectNamedPipe(accepted); CloseHandle(accepted); }
        }
        int active=0,first=-1; uint32_t queued=0;
        for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX;i++) {
            LmbComputeClient *c=&clients[i];
            if (c->pipe==INVALID_HANDLE_VALUE) continue;
            if (c->state!=2 && now>=c->deadline) { lmb_compute_drop(c); continue; }
            if (WaitForSingleObject(c->read.hEvent,0)==WAIT_OBJECT_0) {
                DWORD received=0;
                BOOL ok=GetOverlappedResult(c->pipe,&c->read,&received,FALSE); c->pending=0;
                if (!ok || !received || c->state || received>sizeof c->request-c->received) {
                    lmb_compute_drop(c); continue;
                }
                c->received+=received;
                if (c->received==sizeof c->request) {
                    uint32_t wait=0;
                    for (unsigned j=0;j<4;j++) wait|=(uint32_t)c->request[4+j]<<(8*j);
                    if (memcmp(c->request,"LCP\1",4) || !wait || wait>300000 ||
                        ticket==UINT64_MAX || lmb_pipe_peer(c->pipe)) { lmb_compute_drop(c); continue; }
                    c->state=1; c->ticket=++ticket; c->deadline=now+wait;
                }
                if (lmb_compute_read_next(c)) { lmb_compute_drop(c); continue; }
            }
            if (c->state==2) active=1;
            if (c->state==1) {
                queued++;
                if (first<0 || c->ticket<clients[first].ticket) first=(int)i;
            }
        }
        if (!active && first>=0) {
            LmbComputeClient *c=&clients[first]; uint8_t grant=1; queued--;
            if (!lmb_pipe_io(c->pipe,&grant,1,1,lmb_compute_now()+200)) {
                c->state=2; active=1; atomic_fetch_add(&b->grants,1);
            } else lmb_compute_drop(c);
        }
        atomic_store(&b->queued,queued); atomic_store(&b->active,(uint32_t)active);
    }
    for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX;i++) lmb_compute_drop(&clients[i]);
    atomic_store(&b->queued,0); atomic_store(&b->active,0); atomic_store(&b->stop,1);
    return NULL;
}
static inline void lmb_compute_close_listener(LmbComputeBroker *b) {
    if (b->listener!=INVALID_HANDLE_VALUE) {
        if (b->connecting) {
            DWORD ignored; CancelIoEx(b->listener,&b->connect);
            (void)GetOverlappedResult(b->listener,&b->connect,&ignored,TRUE);
        }
        CloseHandle(b->listener); b->listener=INVALID_HANDLE_VALUE;
    }
    if (b->connect.hEvent) CloseHandle(b->connect.hEvent);
    if (b->wake) CloseHandle(b->wake);
    b->connect.hEvent=b->wake=NULL;
}
static inline int lmb_compute_broker_start(LmbComputeBroker *b,const char *path) {
    memset(b,0,sizeof *b); b->listener=INVALID_HANDLE_VALUE;
    if (lmb_pipe_name(path,"compute",b->path,sizeof b->path)) return -1;
    b->wake=CreateEventW(NULL,TRUE,FALSE,NULL); b->connect.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!b->wake || !b->connect.hEvent || lmb_compute_listener(b,1) ||
        pthread_create(&b->thread,NULL,lmb_compute_serve,b)) { lmb_compute_close_listener(b); return -1; }
    b->started=1; return 0;
}
static inline void lmb_compute_broker_stop(LmbComputeBroker *b) {
    if (!b->started) return;
    atomic_store(&b->stop,1); SetEvent(b->wake); pthread_join(b->thread,NULL);
    lmb_compute_close_listener(b); b->started=0;
}
#endif
#endif
