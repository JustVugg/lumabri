#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/runtime/lumabri_api_access.h"
#include <assert.h>

int main(void) {
    (void)lmb_sign;
    char path[]="/tmp/lumabri-api-access-XXXXXX"; assert(mkdtemp(path));
    int dir=lmb_api_access_dir(path,0); assert(dir>=0);
    int lock=lmb_api_access_lock(dir); assert(lock>=0 && lmb_api_access_lock(dir)<0);
    char token[98], auth[128];
    assert(lmb_api_user_create(dir,"../unsafe",token));
    assert(!lmb_api_user_create(dir,"alice",token));
    snprintf(auth,sizeof auth,"Bearer %s",token);
    LmbApiUser user; assert(!lmb_api_authorize(dir,auth,&user) && !user.count && !strcmp(user.name,"alice"));
    assert(lmb_api_user_create(dir,"alice",token));
    uint8_t allocation[32]={1}; assert(!lmb_api_user_allows(&user,allocation));
    user.count=1; assert(lmb_api_user_save(dir,&user));
    memcpy(user.allocations[0],allocation,32); user.count=1;
    assert(!lmb_api_user_save(dir,&user));
    memcpy(user.allocations[1],allocation,32); user.count=2; assert(lmb_api_user_save(dir,&user)); user.count=1;
    assert(!lmb_api_authorize(dir,auth,&user) && lmb_api_user_allows(&user,allocation));
    char wrong[128]; strcpy(wrong,auth); wrong[strlen(wrong)-1]^=1;
    assert(lmb_api_authorize(dir,wrong,&user));
    assert(lmb_api_authorize(dir,"Bearer alice.short",&user));
    assert(lmb_api_authorize(dir,"alice.secret",&user));
    int first=lmb_api_user_permit(dir,"alice",2), second=lmb_api_user_permit(dir,"alice",2);
    assert(first>=0 && second>=0 && lmb_api_user_permit(dir,"alice",2)<0);
    close(first); first=lmb_api_user_permit(dir,"alice",2); assert(first>=0);
    close(first); close(second);
    assert(!fchmodat(dir,"alice.user",0644,0)); assert(lmb_api_authorize(dir,auth,&user));
    assert(!fchmodat(dir,"alice.user",0600,0)); assert(!lmb_api_authorize(dir,auth,&user));
    assert(!renameat(dir,"alice.user",dir,"saved.user"));
    assert(!symlinkat("saved.user",dir,"alice.user")); assert(lmb_api_authorize(dir,auth,&user));
    assert(!unlinkat(dir,"alice.user",0)); assert(!linkat(dir,"saved.user",dir,"alice.user",0));
    assert(lmb_api_authorize(dir,auth,&user));
    assert(!unlinkat(dir,"saved.user",0)); assert(!lmb_api_authorize(dir,auth,&user));
    assert(!unlinkat(dir,"alice.user",0)); assert(lmb_api_authorize(dir,auth,&user));
    close(lock);
    assert(!unlinkat(dir,"access.lock",0)); assert(!unlinkat(dir,"alice.slot-0",0)); assert(!unlinkat(dir,"alice.slot-1",0));
    close(dir); assert(!rmdir(path));
    puts("API ACCESS: PASS (hashed bearer credentials, exact model grants, private/atomic files, quota, revoke and path rejection)");
    return 0;
}
