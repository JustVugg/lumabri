/* Native last-handle proof survives keeper/worker separation; an intent or
 * PID disappearance is never enough to acknowledge released RAM. */
#include "src/runtime/lumabri_allocation_windows.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_ready.h"
#include <assert.h>
int wmain(int argc,wchar_t **argv) {
    if (argc==2 && !wcscmp(argv[1],L"--holder")) {
        assert(getenv("LUMABRI_ALLOCATION_LIFETIME_HANDLE"));assert(!lmb_ready_notify());
        Sleep(INFINITE);return 1;
    }
    wchar_t temporary[1024],folder[1200],self[2048];char base[3600],binary[6144];
    assert(GetTempPathW(1024,temporary) && GetModuleFileNameW(NULL,self,2048));
    swprintf(folder,1200,L"%lslumabri-lifetime-\x00e8-%lu-%llu",temporary,GetCurrentProcessId(),GetTickCount64());
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,folder,-1,base,sizeof base,NULL,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,self,-1,binary,sizeof binary,NULL,NULL));
    assert(!lmb_private_mkdir(base));
    LmbAllocationLifetime r={.reserved_bytes=128u<<20,.begin=2,.end=4},observed;
    memset(r.allocation,1,32);memset(r.root,2,32);memset(r.requester,3,32);memset(r.offer_hash,4,64);
    assert(lmb_allocation_unused_windows(base,r.allocation)==1);
    assert(lmb_allocation_prove_windows(base,r.allocation,&observed)==-1);
    HANDLE held=lmb_allocation_create_windows(base,&r);assert(held!=INVALID_HANDLE_VALUE);
    assert(lmb_allocation_unused_windows(base,r.allocation)==0);
    assert(lmb_allocation_create_windows(base,&r)==INVALID_HANDLE_VALUE);
    assert(lmb_allocation_prove_windows(base,r.allocation,&observed)==1);
    HANDLE ready=CreateEventW(NULL,TRUE,FALSE,NULL);assert(ready);
    LmbChildHandle handles[]={{"LUMABRI_READY_HANDLE",ready},{"LUMABRI_ALLOCATION_LIFETIME_HANDLE",held}};
    const char *args[]={"--holder"};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=1,.handles=handles,.nhandles=2};
    LmbOwnedProcess child={0};assert(!lmb_owned_process_start(&child,&launch));
    assert(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0);CloseHandle(ready);
    CloseHandle(held); /* the keeper's handle is gone, but its worker still holds the fence */
    assert(lmb_allocation_prove_windows(base,r.allocation,&observed)==1);
    LmbRetiredReceipt receipt={.phase=LMB_RECEIPT_PREPARED,.seal={.revision=3,.draining=LMB_NODE_CONTROL_RETIRED}};
    memset(receipt.seal.instance,5,32);memcpy(receipt.allocation,r.allocation,32);
    memcpy(receipt.root,r.root,32);memcpy(receipt.requester,r.requester,32);
    assert(!lmb_receipt_store_windows(base,&receipt));receipt.phase=LMB_RECEIPT_DONE;
    assert(lmb_receipt_store_windows(base,&receipt)==-1);
    assert(!lmb_owned_process_stop(&child,0,5000) && !lmb_owned_process_close(&child));
    uint64_t until=GetTickCount64()+5000;
    while (lmb_allocation_prove_windows(base,r.allocation,&observed)==1) {assert(GetTickCount64()<until);Sleep(10);}
    assert(!lmb_allocation_prove_windows(base,r.allocation,&observed) && !memcmp(observed.offer_hash,r.offer_hash,64));
    assert(!lmb_receipt_store_windows(base,&receipt) && !lmb_receipt_store_windows(base,&receipt));
    receipt.root[0]^=1;assert(lmb_receipt_store_windows(base,&receipt)==-1);receipt.root[0]^=1;
    LmbRetiredReceipt read;assert(!lmb_receipt_read_windows(base,r.allocation,1,&read) && read.phase==LMB_RECEIPT_DONE);
    assert(lmb_allocation_create_windows(base,&r)==INVALID_HANDLE_VALUE); /* terminal IDs still cannot replay */
    r.allocation[0]=9;held=lmb_allocation_create_windows(base,&r);assert(held!=INVALID_HANDLE_VALUE);
    LARGE_INTEGER zero={0};assert(SetFilePointerEx(held,zero,NULL,FILE_BEGIN) && SetEndOfFile(held));CloseHandle(held);
    assert(lmb_allocation_prove_windows(base,r.allocation,&observed)==-1);
    assert(lmb_allocation_unused_windows(base,r.allocation)==0 && lmb_allocation_create_windows(base,&r)==INVALID_HANDLE_VALUE);
    puts("NATIVE ALLOCATION: PASS (inherited last holder, unknown/missing/corrupt evidence, immutable IDs, intent versus completion)");
    return 0;
}
