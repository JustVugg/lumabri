/* Real encrypted host control and admission, deterministic codec for fault
 * timing. Real model arithmetic is covered by the household service flow. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

static void codec(int input, int output, int release) {
    FILE *in=fdopen(input,"r"), *out=fdopen(output,"w"); assert(in && out);
    setvbuf(in,NULL,_IONBF,0); setvbuf(out,NULL,_IONBF,0);
    char line[512],nonce[65]; unsigned id,slot; size_t size;
    while (fgets(line,sizeof line,in)) {
        if (sscanf(line,"RESET_SLOT %64s %u",nonce,&slot)==2 || sscanf(line,"RESET %64s",nonce)==1)
            fprintf(out,"RESET_DONE %s\n",nonce);
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
static void run_pool(unsigned slots) {
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
    pid_t host=fork(); assert(host>=0);
    if (!host) {
        close(release[1]); setenv("LUMABRI_PEER_KEY",host_file,1); assert(!lmb_secure_init());
        install_chat_signal_handlers();
        int to[2],from[2]; assert(!pipe(to) && !pipe(from));
        pid_t child=fork(); assert(child>=0);
        if (!child) { close(to[1]); close(from[0]); close(listener); codec(to[0],from[1],release[0]); }
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
    close(release[1]); kill(host,SIGTERM);
    assert(waitpid(host,&status,0)==host && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("HOST DRAIN: PASS (pinned owner/root, active and queued turns finish, idle clients close, BUSY, revision/instance fencing, idempotence, resume)");
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
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
    for (unsigned slots=1;slots<=3;slots+=2) {
        pid_t child=fork(); assert(child>=0);
        if (!child) { run_pool(slots); fflush(stdout); _exit(0); }
        int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    return 0;
}
