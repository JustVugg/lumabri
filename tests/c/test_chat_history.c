/* Exercise the real history handler and filesystem with a deterministic
 * interleaving at lock release. Only the store's close() calls are wrapped;
 * production code has no test hooks and this works on Linux and macOS. */
#define _GNU_SOURCE
#include "src/runtime/lumabri_api_access.h"
#include <assert.h>
static int history_close(int fd);
#define close history_close
#include "src/runtime/lumabri_chat_store.h"
#undef close
#define main lumabri_cli_main
#include "lumabri.c"
#undef main

static int armed, injected, deleting, history_dir;
static struct stat watched;
static char committed_id[33], other_document[256];
static uint32_t committed_revision;
static uint64_t committed_updated;

static int history_close(int fd) {
    struct stat st;
    int trigger=armed && !fstat(fd,&st) && st.st_dev==watched.st_dev && st.st_ino==watched.st_ino;
    int rc=close(fd);
    if (trigger) {
        armed=0; assert(!rc);
        /* Schedule B after A commits and unlocks, before A sends its reply.
         * B must acquire the ordinary store lock; holding it for the HTTP
         * response would fail here rather than conceal the race. */
        char ids[LMB_CHAT_MAX][33]; size_t count;
        assert(!lmb_chat_list(history_dir,ids,&count) && count==1);
        strcpy(committed_id,ids[0]);
        LmbChat committed;
        assert(!lmb_chat_load(history_dir,committed_id,&committed));
        assert(committed.revision==committed_revision && strstr(committed.data,"Writer A"));
        committed_updated=committed.updated;
        lmb_chat_free(&committed);
        assert(!lmb_chat_commit(history_dir,committed_id,committed_revision,
            deleting ? NULL : other_document,deleting ? 0 : strlen(other_document),deleting));
        injected++;
    }
    return rc;
}

static unsigned history_request(int access, const LmbApiUser *user, const char *method,
                                const char *id, const char *body, char reply[2048]) {
    LmbApiHttp request={0}; strcpy(request.method,method);
    snprintf(request.path,sizeof request.path,"/api/v1/conversations%s%s",id ? "/" : "",id ? id : "");
    int sockets[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    api_history(sockets[0],access,user,&request,body,body ? strlen(body) : 0);
    assert(!close(sockets[0]));
    size_t used=0;
    for (;;) {
        assert(used<2047);
        ssize_t n=read(sockets[1],reply+used,2047-used);
        if (n<0 && errno==EINTR) continue;
        assert(n>=0); if (!n) break;
        used+=(size_t)n;
    }
    reply[used]=0; close(sockets[1]);
    unsigned status=0; assert(sscanf(reply,"HTTP/1.1 %u",&status)==1);
    return status;
}

static void history_race(int creating, int remove_document) {
    char root[]="/tmp/lmb-history-response-XXXXXX"; assert(mkdtemp(root));
    int access=lmb_api_access_dir(root,0); assert(access>=0);
    LmbApiUser user={.name="alice",.digest={1},.count=1};
    memset(user.allocations[0],1,32);
    char model[65]; lmb_hex(model,user.allocations[0],32);
    history_dir=lmb_chat_dir(access,&user); assert(history_dir>=0);
    char document[256], body[320], reply[2048];
    snprintf(document,sizeof document,"{\"model\":\"%s\",\"title\":\"Writer A\",\"state\":\"idle\",\"messages\":[]}",model);
    snprintf(other_document,sizeof other_document,"{\"model\":\"%s\",\"title\":\"Writer B\",\"state\":\"idle\",\"messages\":[]}",model);
    const char *id="0123456789abcdef0123456789abcdef";
    if (!creating) assert(!lmb_chat_commit(history_dir,id,0,document,strlen(document),0));
    int lock=lmb_api_access_lock(history_dir); assert(lock>=0);
    assert(!fstat(lock,&watched)); close(lock);
    snprintf(body,sizeof body,"{\"revision\":%u,\"conversation\":%s}",creating ? 0 : 1,document);
    committed_revision=creating ? 1 : 2;
    deleting=remove_document; injected=0; armed=1;
    unsigned status=history_request(access,&user,"POST",creating ? NULL : id,body,reply);
    assert(!armed && injected==1);
    assert(status==200 && "transaction snapshot response must remain successful after deletion");
    char expected[512];
    snprintf(expected,sizeof expected,"{\"id\":\"%s\",\"revision\":%u,\"updated\":%llu,\"conversation\":%s}\n",
        committed_id,committed_revision,(unsigned long long)committed_updated,document);
    const char *json=strstr(reply,"\r\n\r\n"); assert(json);
    assert(!strcmp(json+4,expected) && "transaction snapshot response must contain A's document and revision");

    /* The response must not resurrect a deleted file or roll back B. A's
     * next HTTP update carries its own revision and must be rejected. */
    snprintf(body,sizeof body,"{\"revision\":%u,\"conversation\":%s}",committed_revision,document);
    assert(history_request(access,&user,"POST",committed_id,body,reply)==(deleting ? 404u : 409u));
    LmbChat stored;
    if (deleting) assert(lmb_chat_load(history_dir,committed_id,&stored)==LMB_CHAT_MISSING);
    else {
        assert(!lmb_chat_load(history_dir,committed_id,&stored));
        assert(stored.revision==committed_revision+1 && !strcmp(stored.data,other_document));
        lmb_chat_free(&stored); assert(!unlinkat(history_dir,committed_id,0));
    }
    assert(!unlinkat(history_dir,"access.lock",0)); close(history_dir);
    int histories=openat(access,"history",O_RDONLY|O_DIRECTORY); assert(histories>=0);
    char ns[65]; lmb_hex(ns,user.digest,32); assert(!unlinkat(histories,ns,AT_REMOVEDIR)); close(histories);
    assert(!unlinkat(access,"history",AT_REMOVEDIR)); close(access); assert(!rmdir(root));
    printf("CHAT HISTORY SNAPSHOT: PASS (%s, concurrent %s)\n",creating ? "create" : "update",deleting ? "delete" : "update");
}

int main(int argc, char **argv) {
    if (argc==3) {
        assert((!strcmp(argv[1],"0") || !strcmp(argv[1],"1")) &&
               (!strcmp(argv[2],"0") || !strcmp(argv[2],"1")));
        history_race(atoi(argv[1]),atoi(argv[2]));
    } else {
        assert(argc==1);
        for (int creating=0;creating<2;creating++) for (int removing=0;removing<2;removing++)
            history_race(creating,removing);
    }
    return 0;
}
