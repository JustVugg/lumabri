/* Real encrypted host control and admission, deterministic codec for fault
 * timing. Real model arithmetic is covered by the household service flow. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>
#include <sys/mman.h>

static void codec(int input, int output, int release, const _Atomic int *reset_fault) {
    FILE *in=fdopen(input,"r"), *out=fdopen(output,"w"); assert(in && out);
    setvbuf(in,NULL,_IONBF,0); setvbuf(out,NULL,_IONBF,0);
    char line[512],nonce[65]; unsigned id,slot; size_t size;
    while (fgets(line,sizeof line,in)) {
        if (sscanf(line,"RESET_SLOT %64s %u",nonce,&slot)==2 || sscanf(line,"RESET %64s",nonce)==1)
            fprintf(out,"RESET_DONE %s\n",atomic_load(reset_fault) ? "wrong-nonce" : nonce);
        else if (sscanf(line,"SUBMIT %u %u %zu",&id,&slot,&size)==3) {
            char prompt[32]={0}; assert(size<sizeof prompt && fread(prompt,1,size,in)==size && fgetc(in)=='\n');
            fprintf(out,"ACCEPT %u\n",id);
            if (!strcmp(prompt,"hold")) { char permit; assert(read(release,&permit,1)==1); }
            fprintf(out,"DATA %u 2\nok\nDONE %u STAT 1 0\n",id,id);
        } else assert(!"unexpected codec input");
    }
    fclose(in); fclose(out); close(release); _exit(0);
}
static void expect_frame(int fd, const char *needle) {
    char text[8192]=""; size_t used=0;
    while (!strstr(text,needle)) {
        LmbMsg msg={0}; assert(!lmb_recv_bounded(fd,&msg,4096,3000) && msg.op==LMB_HOST_STREAM);
        assert(msg.pay_len<sizeof text-used);
        memcpy(text+used,msg.pay,msg.pay_len); used+=msg.pay_len; text[used]=0; lmb_msg_free(&msg);
    }
}
static int connect_client(const char *address, int busy) {
    int fd=lmb_connect_ms_io(address,2000,3000); assert(fd>=0);
    assert(!lmb_auth(fd) && !lmb_send(fd,LMB_HOST_HELLO,NULL,0,NULL,0));
    LmbMsg msg={0}; assert(!lmb_recv(fd,&msg) && msg.op==LMB_HOST_HELLO_R);
    LmbCur cur={msg.body,msg.body_len,0}; char field[128]; uint32_t available;
    for (unsigned i=0;i<3;i++) assert(!lmb_cur_str(&cur,field,sizeof field));
    assert(!lmb_cur_u32(&cur,&available) && (busy ? !available : available>0));
    lmb_msg_free(&msg); return fd;
}
static void send_prompt(int fd, unsigned id, const char *prompt) {
    char frame[128]; int n=snprintf(frame,sizeof frame,"SUBMIT %u 0 %zu 8 0 1\n%s\n",id,strlen(prompt),prompt);
    assert(n>0 && (size_t)n<sizeof frame && !lmb_send(fd,LMB_HOST_STREAM,NULL,0,frame,(uint32_t)n));
}
static void run_pool(unsigned slots, int reject_retirement_reset) {
    char tmp[]="/tmp/lmb-host-drain-XXXXXX"; assert(mkdtemp(tmp));
    char host_file[256],client_file[256],other_file[256],known[256];
    snprintf(host_file,sizeof host_file,"%s/host.key",tmp); snprintf(client_file,sizeof client_file,"%s/client.key",tmp);
    snprintf(other_file,sizeof other_file,"%s/other.key",tmp); snprintf(known,sizeof known,"%s/known",tmp);
    uint8_t secret[64],host_key[32],client_key[32]; char host_hex[65],root[65];
    assert(!lmb_peer_identity(host_file,secret,host_key) && !lmb_peer_identity(client_file,secret,client_key));
    lmb_hex(host_hex,host_key,32); memset(root,'a',64); root[64]=0;
    setenv("LUMABRI_ENCRYPT","1",1); setenv("LUMABRI_KNOWN_HOSTS",known,1);
    setenv("LUMABRI_TOKEN","private-host-drain-test",1); unsetenv("LUMABRI_READY_FD");
    int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(struct sockaddr *)&addr,sizeof addr) && !listen(listener,8));
    socklen_t length=sizeof addr; assert(!getsockname(listener,(struct sockaddr *)&addr,&length));
    char address[64]; snprintf(address,sizeof address,"127.0.0.1:%u",ntohs(addr.sin_port));
    int release[2]; assert(!pipe(release));
    _Atomic int *reset_fault=mmap(NULL,sizeof *reset_fault,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(reset_fault!=MAP_FAILED); atomic_init(reset_fault,0);
    pid_t host=fork(); assert(host>=0);
    if (!host) {
        close(release[1]); setenv("LUMABRI_PEER_KEY",host_file,1); assert(!lmb_secure_init());
        install_chat_signal_handlers();
        int to[2],from[2]; assert(!pipe(to) && !pipe(from));
        pid_t child=fork(); assert(child>=0);
        if (!child) { close(to[1]); close(from[0]); close(listener); codec(to[0],from[1],release[0],reset_fault); }
        close(to[0]); close(from[1]); close(release[0]);
        Engine engine={.pid=child,.to=to[1],.from=from[0],.segment=1,.reset_supported=1,.session_slots=slots,
            .numeric_abi=1,.numeric_class="test-drain-cpu"};
        HostState state={.engine=&engine,.slots=slots,.max_frame=1024,.max_new=8,.idle_seconds=10,
            .request_seconds=20,.client_key=client_key,.model_root=root};
        int rc=host_sessions_run(listener,&state,5000);
        close(listener); close(engine.to); close(engine.from); kill(child,SIGTERM); waitpid(child,NULL,0); _exit(rc);
    }
    close(listener); close(release[0]);
    /* A different authenticated household member has the token but no right
     * to query or drain this requester's host. */
    pid_t outsider=fork(); assert(outsider>=0);
    if (!outsider) {
        setenv("LUMABRI_PEER_KEY",other_file,1); assert(!lmb_secure_init()); LmbHostControl state;
        assert(lmb_host_control_rpc(address,host_hex,root,0,NULL,&state)<0); _exit(0);
    }
    int status; assert(waitpid(outsider,&status,0)==outsider && WIFEXITED(status) && !WEXITSTATUS(status));
    setenv("LUMABRI_PEER_KEY",client_file,1); assert(!lmb_secure_init());
    LmbHostControl original,drained,observed;
    assert(!lmb_host_control_rpc(address,host_hex,root,0,NULL,&original) && !original.draining && original.revision==1);
    char wrong[65]; memset(wrong,'b',64); wrong[64]=0;
    assert(lmb_host_control_rpc(address,host_hex,wrong,0,NULL,&observed)<0);
    assert(lmb_host_control_rpc(address,wrong,root,0,NULL,&observed)<0);
    int a=connect_client(address,0), b=-1,c=-1;
    send_prompt(a,1,"hold"); expect_frame(a,"ACCEPT 1\n");
    if (slots>1) {
        b=connect_client(address,0); send_prompt(b,2,"hi"); expect_frame(b,"QUEUED"); c=connect_client(address,0);
    }
    assert(!lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_DRAIN,&original,&drained));
    assert(drained.draining && drained.requests==(slots>1 ? 2u : 1u) && drained.revision==2);
    assert(lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_RETIRE,&drained,&observed)==LMB_HOST_CONTROL_CONFLICT);
    assert(!lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_DRAIN,&original,&observed));
    assert(observed.revision==drained.revision); /* retry is idempotent */
    int busy=connect_client(address,1); lmb_close(busy);
    Engine rejected; int max_new=8;
    assert(host_connect(address,NULL,host_hex,root,&rejected,&max_new,0)==HOST_CONNECT_BUSY);
    assert(rejected.to==-1 && rejected.from==-1);
    assert(host_connect(address,NULL,host_hex,wrong,&rejected,&max_new,0)<0);
    assert(write(release[1],"y",1)==1); expect_frame(a,"DONE 1 ");
    if (b>=0) expect_frame(b,"DONE 2 ");
    double deadline=nowd()+5;
    do {
        assert(!lmb_host_control_rpc(address,host_hex,root,0,NULL,&observed));
        assert(nowd()<deadline); if (observed.connections) (void)poll(NULL,0,10);
    } while (observed.connections);
    assert(observed.draining && !observed.requests);
    LmbMsg msg={0}; assert(lmb_recv(a,&msg)); lmb_msg_free(&msg); lmb_close(a);
    if (b>=0) lmb_close(b);
    if (c>=0) { assert(lmb_recv(c,&msg)); lmb_msg_free(&msg); lmb_close(c); }
    assert(lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_RESUME,&original,&observed)==LMB_HOST_CONTROL_CONFLICT);
    assert(!lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_RESUME,&drained,&observed));
    assert(!observed.draining && observed.revision==3);
    assert(lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_DRAIN,&original,&drained)==LMB_HOST_CONTROL_CONFLICT);
    drained=observed; drained.instance[0]^=1;
    assert(lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_DRAIN,&drained,&original)==LMB_HOST_CONTROL_CONFLICT);
    a=connect_client(address,0); send_prompt(a,3,"hi"); expect_frame(a,"DONE 3 "); lmb_close(a);
    assert(!lmb_host_control_rpc(address,host_hex,root,0,NULL,&original));
    assert(!lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_DRAIN,&original,&drained));
    uint64_t seal_deadline=lmb_io_monotonic_ms()+5000;
    if (reject_retirement_reset) atomic_store(reset_fault,1);
    do {
        assert(lmb_io_monotonic_ms()<seal_deadline);
        int rc=lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_RETIRE,&drained,&observed);
        if (reject_retirement_reset && rc<0) {
            close(release[1]);
            assert(waitpid(host,&status,0)==host && WIFEXITED(status) && WEXITSTATUS(status));
            assert(!munmap(reset_fault,sizeof *reset_fault));
            puts("HOST RETIREMENT: PASS (missing valid codec reset acknowledgement never yields an idle seal)");
            return;
        }
        assert(!reject_retirement_reset || rc); /* no false success */
        assert(!rc || (rc==LMB_HOST_CONTROL_CONFLICT && observed.revision==drained.revision && observed.connections));
        if (!rc) break;
        (void)poll(NULL,0,10);
    } while (1);
    assert(observed.draining==LMB_HOST_CONTROL_RETIRED && !observed.connections && !observed.requests);
    assert(!lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_RETIRE,&drained,&original));
    assert(original.revision==observed.revision);
    assert(lmb_host_control_rpc(address,host_hex,root,LMB_HOST_CONTROL_RESUME,&observed,&original)==LMB_HOST_CONTROL_CONFLICT);
    busy=connect_client(address,1); lmb_close(busy);
    close(release[1]); kill(host,SIGTERM);
    assert(waitpid(host,&status,0)==host && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!munmap(reset_fault,sizeof *reset_fault));
    puts("HOST DRAIN: PASS (pinned owner/root, active and queued turns finish, idle clients close, BUSY, revision/instance fencing, idempotence, resume)");
}
static void retirement_journal_test(void) {
    char path[]="/tmp/lmb-retirement-journal-XXXXXX"; assert(mkdtemp(path));
    int access=lmb_api_access_dir(path,0); assert(access>=0);
    int dir=lmb_retirement_dir(access); assert(dir>=0);
    int lock=lmb_api_access_lock(dir); assert(lock>=0);
    LmbRetirement state={.plan_hash={1},.count=2,.host={.instance={1},.revision=1},
        .nodes={{.instance={2},.revision=1},{.instance={3},.revision=1}}}, readback;
    char name[80]; snprintf(name,sizeof name,"%064u.retire",1u);
    assert(lmb_retirement_load(dir,name,&readback)==1);
    assert(!lmb_retirement_store(dir,name,&state));
    assert(!lmb_retirement_load(dir,name,&readback) && readback.count==2 && !readback.phase &&
        readback.host.instance[0]==1 && readback.nodes[1].instance[0]==3);
    state.cursor=1; assert(lmb_retirement_store(dir,name,&state)); state.cursor=0;
    state.phase=LMB_RET_HOST_SEAL; assert(lmb_retirement_store(dir,name,&state));
    state.phase=LMB_RET_RELEASE; assert(lmb_retirement_store(dir,name,&state)); /* no invented seal */
    state.phase=LMB_RET_NODE_DRAIN; state.host.draining=LMB_HOST_CONTROL_RETIRED; state.host.revision=3;
    assert(!lmb_retirement_store(dir,name,&state));
    state.phase=LMB_RET_RELEASE;
    for (unsigned i=0;i<2;i++) { state.nodes[i].draining=LMB_NODE_CONTROL_RETIRED; state.nodes[i].revision=3; }
    assert(!lmb_retirement_store(dir,name,&state));
    state.released=1; state.cursor=1; assert(!lmb_retirement_store(dir,name,&state));
    assert(!lmb_retirement_load(dir,name,&readback) && readback.released==1);
    state.cursor=2; assert(lmb_retirement_store(dir,name,&state)); state.cursor=1;
    LmbResidentPlan plan={0}; plan.execution.count=2; plan.execution.nodes[0].edge=1;
    assert(lmb_retirement_run(dir,name,&plan,&state)<0); /* Edge cannot be acknowledged before its peers. */
    state.phase=LMB_RET_DONE; assert(lmb_retirement_store(dir,name,&state)); /* partial != complete */
    state.released=3; state.cursor=2; assert(!lmb_retirement_store(dir,name,&state));
    assert(!linkat(dir,name,dir,"hardlink",0));
    assert(lmb_retirement_load(dir,name,&readback)<0);
    assert(!unlinkat(dir,"hardlink",0));
    assert(!symlinkat(name,dir,"pending"));
    assert(lmb_retirement_store(dir,name,&state)<0);
    assert(!unlinkat(dir,"pending",0));
    assert(!lmb_retirement_load(dir,name,&readback) && readback.phase==LMB_RET_DONE);
    assert(!unlinkat(dir,name,0)); assert(!unlinkat(dir,"access.lock",0));
    close(lock); close(dir); assert(!unlinkat(access,"retirements",AT_REMOVEDIR)); close(access); assert(!rmdir(path));
    puts("RETIREMENT JOURNAL: PASS (atomic progress, exact fences, partial release retained, unsafe files refused)");
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    retirement_journal_test();
    fflush(stdout);
    /* The one-slot pool also uses a nonblocking pipe. A prompt larger than
     * pipe capacity must wait for writable space, not fail with EAGAIN. */
    int channel[2]; assert(!pipe(channel));
    assert(!fcntl(channel[1],F_SETFL,fcntl(channel[1],F_GETFL,0)|O_NONBLOCK));
    pid_t reader=fork(); assert(reader>=0);
    if (!reader) {
        close(channel[1]); size_t total=0; char bytes[4096]; ssize_t n;
        while ((n=read(channel[0],bytes,sizeof bytes))>0) total+=(size_t)n;
        assert(total==(1u<<20)); close(channel[0]); _exit(0);
    }
    close(channel[0]); char *large=calloc(1,1u<<20); assert(large);
    Engine writer={.to=channel[1],.session_slots=1};
    assert(!host_engine_write(&writer,large,1u<<20)); free(large); close(channel[1]);
    int reader_status;
    assert(waitpid(reader,&reader_status,0)==reader && WIFEXITED(reader_status) && !WEXITSTATUS(reader_status));
    LmbHostControl exhausted={.revision=UINT64_MAX}; uint8_t instance[32]={0};
    assert(lmb_host_control_apply(&exhausted,LMB_HOST_CONTROL_DRAIN,instance,UINT64_MAX));
    assert(!exhausted.draining && exhausted.revision==UINT64_MAX);
    assert(lmb_host_control_apply(&exhausted,99,instance,UINT64_MAX));
    /* Separate processes avoid inheriting cached encryption identities. */
    for (unsigned trial=0;trial<3;trial++) {
        pid_t child=fork(); assert(child>=0);
        if (!child) { run_pool(trial ? 3 : 1,trial==2); fflush(stdout); _exit(0); }
        int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    return 0;
}
