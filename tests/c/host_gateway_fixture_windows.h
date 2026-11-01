/* The same production host pool used by Unix, over native Windows pipes and
 * sockets, with a real Edge child and the real Segment worker. */
#include <math.h>
#include <signal.h>
#include "lumabri_run_gate.h"
#include "lumabri_ready.h"
#include "src/runtime/lumabri_session_limits.h"
static volatile sig_atomic_t g_stopping;
static double nowd(void) { return GetTickCount64()/1000.; }
#include "src/runtime/lumabri_host_protocol.h"
#include "src/runtime/lumabri_host_sessions.h"

typedef struct {LmbSocket listener;HostState state;int result;} NativeHostFixture;
static void *native_host_run(void *arg) {
    NativeHostFixture *h=arg;h->result=host_sessions_run(h->listener,&h->state,1000);return NULL;
}
#include "host_client_fixture_windows.h"
static void fixture_native_host(WorkerFixture *f,const char *binary,const char *directory,const char *tracker) {
    NativeChatFixture chat;native_chat_start(&chat,binary,directory,tracker,f,1);
    LmbHostEngine engine={.to=chat.input,.from=chat.output,.segment=1};
    char line[2048];
    do {
        native_chat_line(&chat,line,sizeof line);
        if (!strcmp(line,"LUMABRI_RESET 1")) engine.reset_supported=1;
        if (!strcmp(line,"LUMABRI_SAMPLING GREEDY")) engine.greedy_only=1;
        if (!strcmp(line,"LUMABRI_REQUEST_SEED 1")) engine.request_seed_supported=1;
        (void)sscanf(line,"LUMABRI_SLOTS %u",&engine.session_slots);
        (void)sscanf(line,"LUMABRI_NUMERIC %u %96[^\n]",&engine.numeric_abi,engine.numeric_class);
    } while (strcmp(line,"STAT 0 0 0 0"));
    assert(engine.reset_supported && engine.session_slots==2 && engine.numeric_abi);
    uint64_t reads=atomic_load(&f->checkpoint.reads);
    LmbSocket listener=socket(AF_INET,SOCK_STREAM,0);assert(listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(struct sockaddr *)&address,sizeof address) && !listen(listener,8));
    int length=sizeof address;assert(!getsockname(listener,(struct sockaddr *)&address,&length));
    char location[64];snprintf(location,sizeof location,"127.0.0.1:%u",ntohs(address.sin_port));
    NativeHostFixture host={.listener=listener,.state={.engine=&engine,.model_type="olmoe",.engine_kind="olmoe",
        .slots=2,.max_frame=1024,.max_new=256,.idle_seconds=30,.request_seconds=30},.result=-1};
    pthread_t thread;assert(!pthread_create(&thread,NULL,native_host_run,&host));
    LmbSocket a=native_host_connect(location,0),b=native_host_connect(location,0),c=native_host_connect(location,1);
    lmb_close(c);
    native_host_send(a,"SUBMIT 1 0 2 8 0 1\nhi\n");native_host_completed(a,"1",0);
    native_host_send(b,"SUBMIT 2 0 2 8 0 1\nhi\n");native_host_completed(b,"2",0);
    native_host_send(a,"SUBMIT 3 0 2 256 0 1\nhi\n");
    /* Wait for ACCEPT rather than cancelling a queued request: both phases
     * have deliberately distinct protocol/lifetime semantics. */
    char seen[4096]="";size_t at=0;
    while (!strstr(seen,"ACCEPT 3\n")) {
        LmbMsg m={0};assert(!lmb_recv(a,&m) && m.op==LMB_HOST_STREAM && at+m.pay_len<sizeof seen);
        memcpy(seen+at,m.pay,m.pay_len);at+=m.pay_len;seen[at]=0;lmb_msg_free(&m);
    }
    native_host_send(a,"CANCEL 3\n");native_host_completed(a,"3",1);
    native_host_send(a,"SUBMIT 4 0 2 8 0 1\nhi\n");native_host_completed(a,"4",0);
    native_host_send(a,"SUBMIT 5 0 1025 8 0 1\n");
    LmbMsg m={0};assert(lmb_recv(a,&m));lmb_msg_free(&m);lmb_close(a);
    native_host_send(b,"SUBMIT 6 0 2 8 0 1\nhi\n");native_host_completed(b,"6",0);
    lmb_close(b);g_stopping=1;pthread_join(thread,NULL);g_stopping=0;
    assert(!host.result);lmb_socket_close(listener);native_chat_stop(&chat,0);
    assert(atomic_load(&f->checkpoint.reads)==reads);
    puts("NATIVE HOST: PASS (shared production gateway, encrypted sockets, real Edge/Segment, bounded slots, cancellation/reuse, frame refusal, no weight reads)");
}
