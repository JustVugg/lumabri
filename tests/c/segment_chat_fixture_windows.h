/* The actual native Edge executable, private binary codec pipes and the
 * production Segment worker above. No fake tokens or simulated RUN replies. */
typedef struct { LmbOwnedProcess process;HANDLE input,output; } NativeChatFixture;
static void native_chat_read(NativeChatFixture *c,void *out,size_t n) {
    ULONGLONG until=GetTickCount64()+15000;uint8_t *p=out;
    while (n) {
        DWORD available=0,got=0;
        assert(PeekNamedPipe(c->output,NULL,0,NULL,&available,NULL));
        if (!available) {
            assert(GetTickCount64()<until && WaitForSingleObject(c->process.process,0)==WAIT_TIMEOUT);
            Sleep(1);continue;
        }
        DWORD wanted=available<n ? available : (DWORD)n;
        assert(ReadFile(c->output,p,wanted,&got,NULL) && got);
        p+=got;n-=got;
    }
}
static void native_chat_line(NativeChatFixture *c,char *line,size_t cap) {
    for (size_t i=0;i<cap;i++) {
        char ch;native_chat_read(c,&ch,1);
        assert(ch!='\r'); /* CRT text translation corrupts this byte protocol. */
        if (ch=='\n') {line[i]=0;return;}
        line[i]=ch;
    }
    assert(!"unbounded codec header");
}
static void native_chat_write(NativeChatFixture *c,const void *data,size_t n) {
    DWORD done=0;assert(n<=4096 && WriteFile(c->input,data,(DWORD)n,&done,NULL) && done==n);
}
static void native_chat_stop(NativeChatFixture *c,int expected) {
    CloseHandle(c->input);c->input=NULL;
    DWORD code=99;
    assert(!lmb_owned_process_wait(&c->process,15000,&code) && code==(DWORD)expected);
    CloseHandle(c->output);assert(!lmb_owned_process_close(&c->process));
}
static void native_chat_start(NativeChatFixture *c,const char *binary,const char *directory,
    const char *tracker,WorkerFixture *f,int serve) {
    memset(c,0,sizeof *c);
    char checkpoint[6400],key[6600],known[6600],root[65],signer[65],bytes[32],expected[512];
    snprintf(checkpoint,sizeof checkpoint,"%s\\edge-%s",directory,serve ? "serve" : "oracle");
    snprintf(key,sizeof key,"LUMABRI_PEER_KEY=%s\\edge.key",directory);
    snprintf(known,sizeof known,"LUMABRI_KNOWN_HOSTS=%s\\edge.known",directory);
    lmb_hex(root,f->checkpoint.root,32);lmb_hex(signer,f->checkpoint.key,32);
    snprintf(bytes,sizeof bytes,"%llu",(unsigned long long)f->checkpoint.bytes);
    size_t at=0;
    for (unsigned i=0;i<24;i++) at+=(size_t)snprintf(expected+at,sizeof expected-at,"%s%d",i ? "," : "",f->oracle_tokens[i]);
    assert(at<sizeof expected);
    const char *env[]={key,known,"LUMABRI_TOKEN=native-worker-test","LUMABRI_ENCRYPT=1",
        "LUMABRI_RESIDENT_REQUIRED=1","LUMABRI_RESIDENCY_POLICY=engine-locked",
        "LUMABRI_EDGE_RAM_BYTES=134217728","LUMABRI_RAM_RESERVE_MB=256",
        "LUMABRI_ENGINE_BACKEND=cpu","LUMABRI_COMPUTE_SOCKET=","LUMABRI_HOST_SESSIONS=2",
        "PIN=off","PILOT=0","HOT=0","COLI_NO_OMP_TUNE=1"};
    const char *args[]={"--engine","olmoe","--model-dir",checkpoint,"--model","native-worker-olmoe",
        "--tracker",tracker,"--model-root",root,"--tokenizer-root",
        "2222222222222222222222222222222222222222222222222222222222222222",
        "--context","512","--max-rows","1","--direct-only",
        "--checkpoint-signer",signer,"--model-bytes",bytes,
        "--prompt-ids","1","--tokens","24","--expect-ids",expected,"--json"};
    size_t nargs=sizeof args/sizeof *args;
    if (serve) {nargs-=7;args[nargs++]="--serve";args[nargs++]="--sessions";args[nargs++]="2";}
    HANDLE child_input,child_output;
    assert(CreatePipe(&child_input,&c->input,NULL,4096) && CreatePipe(&c->output,&child_output,NULL,4096));
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=nargs,.environment=env,
        .nenvironment=sizeof env/sizeof *env,.input=child_input,.output=child_output,
        .error=GetStdHandle(STD_ERROR_HANDLE)};
    assert(!lmb_owned_process_start(&c->process,&launch));CloseHandle(child_input);CloseHandle(child_output);
}
static void native_chat_turn(NativeChatFixture *c,unsigned id,unsigned slot,char *out,size_t *size,int cancel) {
    /* Embedded LF and control-Z are bytes, not a CRT line or EOF marker. */
    char command[128];int n=snprintf(command,sizeof command,"SUBMIT %u %u 5 %u 0 1\nhi\n\x1a?\n",id,slot,cancel ? 256 : 8);
    native_chat_write(c,command,(size_t)n);*size=0;
    char line[2048];native_chat_line(c,line,sizeof line);
    snprintf(command,sizeof command,"ACCEPT %u",id);assert(!strcmp(line,command));
    if (cancel) {n=snprintf(command,sizeof command,"CANCEL %u\n",cancel==2 ? id+1 : id);native_chat_write(c,command,(size_t)n);}
    for (;;) {
        unsigned received=0;size_t bytes=0;
        native_chat_line(c,line,sizeof line);
        if (sscanf(line,"DATA %u %zu",&received,&bytes)==2) {
            assert(received==id && bytes<=4096-*size);
            native_chat_read(c,out+*size,bytes);*size+=bytes;
            char newline;native_chat_read(c,&newline,1);assert(newline=='\n');continue;
        }
        if (!strncmp(line,"PROGRESS ",9)) continue;
        if (cancel) {
            snprintf(command,sizeof command,"ERROR %u request cancelled",id);assert(!strcmp(line,command));
        } else {
            unsigned tokens=0;assert(sscanf(line,"DONE %u STAT %u",&received,&tokens)==2 && received==id && tokens==8);
        }
        break;
    }
}
static void fixture_native_chat(WorkerFixture *f,const char *binary,const char *directory,const char *tracker) {
    NativeChatFixture chat;
    native_chat_start(&chat,binary,directory,tracker,f,0);
    char line[2048];
    do {native_chat_line(&chat,line,sizeof line);} while (strncmp(line,"{\"token_ids\":[",14));
    native_chat_stop(&chat,0); /* --expect-ids verifies all 24 oracle tokens. */
    native_chat_start(&chat,binary,directory,tracker,f,1);
    do {native_chat_line(&chat,line,sizeof line);} while (strcmp(line,"STAT 0 0 0 0"));
    uint64_t reads=atomic_load(&f->checkpoint.reads);
    char first[4096],second[4096],again[4096];size_t a,b,c;
    native_chat_turn(&chat,1,0,first,&a,0);
    native_chat_turn(&chat,2,1,second,&b,0);
    assert(a && a==b && !memcmp(first,second,a));
    native_chat_write(&chat,"RESET_SLOT turn 0\n",18);
    native_chat_line(&chat,line,sizeof line);assert(!strcmp(line,"RESET_DONE turn"));
    native_chat_turn(&chat,3,0,again,&c,0);assert(c==a && !memcmp(first,again,a));
    native_chat_turn(&chat,4,1,again,&c,1);
    native_chat_turn(&chat,5,1,again,&c,0);assert(c==a && !memcmp(first,again,a));
    native_chat_stop(&chat,0);
    assert(atomic_load(&f->checkpoint.reads)==reads);
    native_chat_start(&chat,binary,directory,tracker,f,1);
    do {native_chat_line(&chat,line,sizeof line);} while (strcmp(line,"STAT 0 0 0 0"));
    reads=atomic_load(&f->checkpoint.reads);
    native_chat_turn(&chat,6,0,again,&c,2);
    native_chat_stop(&chat,1); /* Wrong request ID is a corrupt control stream. */
    assert(atomic_load(&f->checkpoint.reads)==reads);
    puts("NATIVE CHAT: PASS (24 oracle tokens, signed remote Edge, binary streaming, two slots, reset, cancel/reuse, invalid control, EOF, no weight reads after READY)");
}
