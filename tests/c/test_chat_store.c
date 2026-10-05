#define _GNU_SOURCE
#include "src/runtime/lumabri_chat_store.h"
#include <assert.h>

int main(void) {
    (void)lmb_sign;
    char root[]="/tmp/lmb-chat-store-XXXXXX"; assert(mkdtemp(root));
    int access=lmb_api_access_dir(root,0); assert(access>=0);
    LmbApiUser alice={.name="alice",.digest={1}}, bob={.name="bob",.digest={2}};
    int a=lmb_chat_dir(access,&alice), b=lmb_chat_dir(access,&bob); assert(a>=0 && b>=0);
    const char *id="0123456789abcdef0123456789abcdef", *data="{\"private\":\"café\"}";
    assert(!lmb_chat_id("../0123456789abcdef0123456789abcd"));
    LmbChat chat;
    assert(lmb_chat_load(a,id,&chat)==LMB_CHAT_MISSING);
    assert(!lmb_chat_commit(a,id,0,data,strlen(data),0));
    assert(!lmb_chat_load(a,id,&chat)); assert(chat.revision==1 && !strcmp(chat.data,data)); lmb_chat_free(&chat);
    assert(lmb_chat_load(b,id,&chat)==LMB_CHAT_MISSING);
    assert(lmb_chat_commit(a,id,0,data,strlen(data),0)==LMB_CHAT_CONFLICT);
    assert(lmb_chat_commit(a,id,3,NULL,0,1)==LMB_CHAT_CONFLICT);
    assert(!lmb_chat_commit(a,id,1,"{}",2,0));
    assert(!lmb_chat_load(a,id,&chat)); assert(chat.revision==2 && !strcmp(chat.data,"{}")); lmb_chat_free(&chat);
    int lock=lmb_api_access_lock(a); assert(lock>=0);
    assert(lmb_chat_commit(a,id,2,"{}",2,0)==LMB_CHAT_BUSY); close(lock);
    /* Bound storage, allow updates at quota, and independently rewind lists. */
    for (unsigned i=0; i<LMB_CHAT_MAX-1; i++) {
        char other[33]; snprintf(other,sizeof other,"%032x",i);
        assert(!lmb_chat_commit(a,other,0,"{}",2,0));
    }
    assert(lmb_chat_commit(a,"ffffffffffffffffffffffffffffffff",0,"{}",2,0)==LMB_CHAT_QUOTA);
    assert(!lmb_chat_commit(a,id,2,"{}",2,0));
    char ids[LMB_CHAT_MAX][33]; size_t count=0;
    for (unsigned i=0; i<3; i++) { assert(!lmb_chat_list(a,ids,&count)); assert(count==LMB_CHAT_MAX); }
    assert(lmb_chat_commit(a,id,3,"x",LMB_CHAT_BYTES+1,0)==LMB_CHAT_UNSAFE);
    int temp=openat(a,".transaction",O_WRONLY|O_CREAT|O_EXCL,0600); assert(temp>=0); close(temp);
    assert(!lmb_chat_commit(a,id,3,"{}",2,0)); /* stale transaction recovered */
    assert(!faccessat(a,id,F_OK,0));
    assert(symlinkat(id,a,".transaction")==0);
    assert(lmb_chat_commit(a,id,4,"{}",2,0)==LMB_CHAT_UNSAFE);
    assert(!unlinkat(a,".transaction",0));
    assert(!lmb_chat_commit(a,id,4,NULL,0,1));
    assert(lmb_chat_load(a,id,&chat)==LMB_CHAT_MISSING);
    assert(symlinkat("access.lock",a,id)==0);
    assert(lmb_chat_load(a,id,&chat)==LMB_CHAT_UNSAFE);
    assert(lmb_chat_commit(a,id,0,"{}",2,0)==LMB_CHAT_UNSAFE);
    assert(!unlinkat(a,id,0));
    for (unsigned i=0; i<LMB_CHAT_MAX-1; i++) {
        char other[33]; snprintf(other,sizeof other,"%032x",i); assert(!unlinkat(a,other,0));
    }
    assert(!unlinkat(a,"access.lock",0)); close(a); close(b);
    int histories=openat(access,"history",O_RDONLY|O_DIRECTORY); assert(histories>=0);
    char ns[65]; lmb_hex(ns,alice.digest,32); assert(!unlinkat(histories,ns,AT_REMOVEDIR));
    lmb_hex(ns,bob.digest,32); assert(!unlinkat(histories,ns,AT_REMOVEDIR)); close(histories);
    assert(!unlinkat(access,"history",AT_REMOVEDIR)); close(access); assert(!rmdir(root));
    puts("CHAT STORE: PASS (private namespaces, revisions, bounds, atomic updates, locks, unsafe files)");
    return 0;
}
