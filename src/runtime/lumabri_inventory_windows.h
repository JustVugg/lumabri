/* The donor's signed inventory runs independently of its approval loop.
 * Only copied observations cross the thread boundary; reservations stay in
 * the keeper. A failed heartbeat expires at the tracker, never reserves RAM. */
#ifndef LMB_INVENTORY_WINDOWS_H
#define LMB_INVENTORY_WINDOWS_H
#include "lumabri_inventory.h"
#include "lumabri_secure.h"
typedef struct {
    CRITICAL_SECTION lock;
    HANDLE stop,ready,thread;
    LmbMachineReport report;
    char tracker[256];
    uint8_t secret[64];
} LmbWindowsInventory;
static DWORD WINAPI lmb_inventory_thread_windows(void *arg) {
    LmbWindowsInventory *p=arg;
    while (WaitForSingleObject(p->stop,0)==WAIT_TIMEOUT) {
        LmbSocket fd=lmb_connect_ms_io(p->tracker,1500,1000);LmbMsg reply={0};uint8_t nonce[32];
        int ready=fd!=LMB_INVALID_SOCKET && !lmb_auth(fd) &&
            !lmb_send(fd,LMB_CHALLENGE,NULL,0,NULL,0) && !lmb_recv_bounded(fd,&reply,64,1000) &&
            reply.op==LMB_CHALLENGE_R && reply.body_len==32 && !reply.pay_len;
        if (ready) memcpy(nonce,reply.body,32);
        lmb_msg_free(&reply);
        while (ready && WaitForSingleObject(p->stop,0)==WAIT_TIMEOUT) {
            EnterCriticalSection(&p->lock);LmbMachineReport report=p->report;LeaveCriticalSection(&p->lock);
            LmbBuf body={0},signed_data={0};uint8_t sig[64];
            int bad=lmb_inventory_pack(&body,&report) || lmb_buf_str(&signed_data,"lumabri.machine.v1") ||
                lmb_buf_bytes(&signed_data,nonce,32) || lmb_buf_bytes(&signed_data,body.p,body.len);
            if (!bad) {
                lmb_sign(sig,signed_data.p,signed_data.len,p->secret);
                bad=lmb_buf_bytes(&body,sig,64) || lmb_send(fd,LMB_MACHINE_REPORT,body.p,(uint32_t)body.len,NULL,0) ||
                    lmb_recv_bounded(fd,&reply,64,1000) || reply.op!=LMB_OK || reply.body_len || reply.pay_len;
            }
            free(body.p);free(signed_data.p);lmb_msg_free(&reply);
            if (bad) break;
            SetEvent(p->ready);
            if (WaitForSingleObject(p->stop,LMB_INVENTORY_HEARTBEAT_MS)!=WAIT_TIMEOUT) break;
        }
        if (fd!=LMB_INVALID_SOCKET) lmb_close(fd);
        if (WaitForSingleObject(p->stop,1000)!=WAIT_TIMEOUT) break;
    }
    return 0;
}
static void lmb_inventory_stop_windows(LmbWindowsInventory *p) {
    if (p->thread) {
        SetEvent(p->stop);WaitForSingleObject(p->thread,INFINITE);CloseHandle(p->thread);
        p->thread=NULL;
    }
    if (p->stop) CloseHandle(p->stop);
    if (p->ready) CloseHandle(p->ready);
    p->stop=p->ready=NULL;SecureZeroMemory(p->secret,sizeof p->secret);
}
static int lmb_inventory_start_windows(LmbWindowsInventory *p,const char *tracker,LmbMachineReport *report) {
    memset(p,0,sizeof *p);InitializeCriticalSection(&p->lock);
    char key[1200];
    if (strlen(tracker)>=sizeof p->tracker ||
        lmb_peer_identity(lmb_peer_key_path(key,sizeof key),p->secret,report->identity)) goto bad;
    p->report=*report;strcpy(p->tracker,tracker);
    p->stop=CreateEventW(NULL,TRUE,FALSE,NULL);p->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!p->stop || !p->ready || !(p->thread=CreateThread(NULL,0,lmb_inventory_thread_windows,p,0,NULL)) ||
        WaitForSingleObject(p->ready,8000)!=WAIT_OBJECT_0) goto bad;
    return 0;
 bad:
    lmb_inventory_stop_windows(p);DeleteCriticalSection(&p->lock);return -1;
}
#endif
