/* Native durable allocation evidence. Births are immutable and never reused.
 * A deny-write/delete HANDLE is inherited by the owned worker: an exclusive
 * new open proves the last holder exited, including after a keeper crash.
 * LockFileEx is deliberately not used as an inherited lifetime fence.
 * Receipt intent/completion are separate create-once, flushed records. */
#ifndef LMB_ALLOCATION_WINDOWS_H
#define LMB_ALLOCATION_WINDOWS_H
#include "src/runtime/lumabri_private_file_windows.h"
#include "src/protocol/lumabri_allocation_lifetime_wire.h"
#include "src/protocol/lumabri_retired_receipt_wire.h"

static int lmb_allocation_path_windows(const char *base,const uint8_t id[32],
        const char *suffix,int create,char path[1600]) {
    char root[1400],shard[1440],hex[65]; lmb_hex(hex,id,32);
    if (snprintf(root,sizeof root,"%s/allocations",base)>=(int)sizeof root ||
        snprintf(shard,sizeof shard,"%s/%.2s",root,hex)>=(int)sizeof shard ||
        snprintf(path,1600,"%s/%s.%s",shard,hex,suffix)>=1600) return -1;
    if (create && (lmb_private_mkdir(root) || lmb_private_mkdir(shard))) return -1;
    return 0;
}
static HANDLE lmb_allocation_open_windows(const char *path,int create,int prove) {
    LmbPrivatePath p; LmbPrivateSecurity s;
    if (lmb_private_path(path,&p)) return INVALID_HANDLE_VALUE;
    if (lmb_private_security(&s)) { lmb_private_path_close(&p); return INVALID_HANDLE_VALUE; }
    HANDLE h=CreateFileW(p.path,READ_CONTROL|GENERIC_READ|((create || prove) ? GENERIC_WRITE : 0),
        prove ? 0 : FILE_SHARE_READ,&s.attributes,create ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (h==INVALID_HANDLE_VALUE) lmb_private_error(GetLastError());
    else if (lmb_private_validate(h,s.user->User.Sid,0)) { CloseHandle(h); h=INVALID_HANDLE_VALUE; }
    lmb_private_security_free(&s); lmb_private_path_close(&p); return h;
}
static int lmb_allocation_read_windows(HANDLE h,void *bytes,DWORD size) {
    LARGE_INTEGER length,zero={0}; DWORD got=0;
    return !GetFileSizeEx(h,&length) || length.QuadPart!=size ||
        !SetFilePointerEx(h,zero,NULL,FILE_BEGIN) || !ReadFile(h,bytes,size,&got,NULL) || got!=size ? -1 : 0;
}
static int lmb_allocation_unused_windows(const char *base,const uint8_t id[32]) {
    char path[1600];
    if (lmb_allocation_path_windows(base,id,"birth",0,path)) return -1;
    HANDLE h=lmb_allocation_open_windows(path,0,0);
    if (h==INVALID_HANDLE_VALUE) return errno==ENOENT ? 1 : errno==EBUSY ? 0 : -1;
    CloseHandle(h); return 0; /* an interrupted/invalid birth also consumes its ID */
}
static HANDLE lmb_allocation_create_windows(const char *base,const LmbAllocationLifetime *record) {
    uint8_t bytes[LMB_LIFETIME_BYTES]; char path[1600]; DWORD wrote=0;
    if (lmb_lifetime_encode(record,bytes) ||
        lmb_allocation_path_windows(base,record->allocation,"birth",1,path)) return INVALID_HANDLE_VALUE;
    HANDLE h=lmb_allocation_open_windows(path,1,0);
    if (h==INVALID_HANDLE_VALUE) return h;
    if (!WriteFile(h,bytes,sizeof bytes,&wrote,NULL) || wrote!=sizeof bytes || !FlushFileBuffers(h)) {
        CloseHandle(h); return INVALID_HANDLE_VALUE; /* never remove a partial birth */
    }
    return h;
}
/* 0 exited, 1 live holder, -1 unknown. Never equate missing evidence with exit. */
static int lmb_allocation_prove_windows(const char *base,const uint8_t id[32],LmbAllocationLifetime *r) {
    char path[1600]; uint8_t bytes[LMB_LIFETIME_BYTES]; memset(r,0,sizeof *r);
    if (lmb_allocation_path_windows(base,id,"birth",0,path)) return -1;
    HANDLE h=lmb_allocation_open_windows(path,0,1);
    if (h==INVALID_HANDLE_VALUE) return errno==EBUSY ? 1 : -1;
    int rc=lmb_allocation_read_windows(h,bytes,sizeof bytes) || lmb_lifetime_decode(bytes,sizeof bytes,r) ||
        memcmp(id,r->allocation,32) || !FlushFileBuffers(h) ? -1 : 0;
    CloseHandle(h); return rc;
}
static int lmb_receipt_read_windows(const char *base,const uint8_t id[32],int done,LmbRetiredReceipt *r) {
    char path[1600]; uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES];
    if (lmb_allocation_path_windows(base,id,done ? "done" : "intent",0,path)) return -1;
    HANDLE h=lmb_allocation_open_windows(path,0,0);
    if (h==INVALID_HANDLE_VALUE) return errno==ENOENT ? 1 : -1;
    int rc=lmb_allocation_read_windows(h,bytes,sizeof bytes) || lmb_retired_decode(bytes,sizeof bytes,r) ||
        memcmp(id,r->allocation,32) || r->phase!=(done ? LMB_RECEIPT_DONE : LMB_RECEIPT_PREPARED) ? -1 : 0;
    CloseHandle(h); return rc;
}
static int lmb_receipt_store_windows(const char *base,const LmbRetiredReceipt *r) {
    uint8_t bytes[LMB_RETIRED_RECEIPT_BYTES],prior[sizeof bytes]; char path[1600]; DWORD wrote=0;
    if (lmb_retired_encode(r,bytes)) return -1;
    if (r->phase==LMB_RECEIPT_DONE) {
        LmbRetiredReceipt intent; LmbAllocationLifetime exit;
        if (lmb_receipt_read_windows(base,r->allocation,0,&intent) ||
            lmb_allocation_prove_windows(base,r->allocation,&exit) ||
            memcmp(exit.root,r->root,32) || memcmp(exit.requester,r->requester,32)) return -1;
        intent.phase=LMB_RECEIPT_DONE;
        if (lmb_retired_encode(&intent,prior) || memcmp(prior,bytes,sizeof bytes)) return -1;
    }
    if (lmb_allocation_path_windows(base,r->allocation,r->phase==LMB_RECEIPT_DONE ? "done" : "intent",1,path)) return -1;
    HANDLE h=lmb_allocation_open_windows(path,1,0);
    if (h==INVALID_HANDLE_VALUE) {
        if (errno!=EEXIST) return -1;
        h=lmb_allocation_open_windows(path,0,1);
        if (h==INVALID_HANDLE_VALUE) return -1;
        int bad=lmb_allocation_read_windows(h,prior,sizeof prior) || memcmp(bytes,prior,sizeof prior) || !FlushFileBuffers(h);
        CloseHandle(h); return bad ? -1 : 0;
    }
    int bad=!WriteFile(h,bytes,sizeof bytes,&wrote,NULL) || wrote!=sizeof bytes || !FlushFileBuffers(h);
    CloseHandle(h); return bad ? -1 : 0;
}
#endif
