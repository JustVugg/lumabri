/* Shared native test client for the production hosted codec. */
static LmbSocket native_host_connect(const char *address,int busy) {
    LmbSocket fd=lmb_connect_ms_io(address,2000,5000);assert(fd!=LMB_INVALID_SOCKET);
    LmbBuf auth={0};assert(!lmb_buf_str(&auth,"native-worker-test") &&
        !lmb_send(fd,LMB_AUTH,auth.p,(uint32_t)auth.len,NULL,0));free(auth.p);
    LmbMsg authenticated={0};assert(!lmb_recv(fd,&authenticated) && authenticated.op==LMB_OK &&
        !authenticated.body_len && !authenticated.pay_len);lmb_msg_free(&authenticated);
    assert(!lmb_send(fd,LMB_HOST_HELLO,NULL,0,NULL,0));
    LmbMsg m={0};assert(!lmb_recv(fd,&m) && m.op==LMB_HOST_HELLO_R && !m.pay_len);
    LmbCur c={m.body,m.body_len,0};char text[128];uint32_t slots;
    for (unsigned i=0;i<3;i++) assert(!lmb_cur_str(&c,text,sizeof text));
    assert(!lmb_cur_u32(&c,&slots) && (busy ? !slots : slots>0));
    lmb_msg_free(&m);return fd;
}
static void native_host_send(LmbSocket fd,const char *text) {
    assert(!lmb_send(fd,LMB_HOST_STREAM,NULL,0,text,(uint32_t)strlen(text)));
}
static void native_host_completed(LmbSocket fd,const char *id,int cancelled) {
    HostOutput observed={0};HostInput input={.active=1};snprintf(input.request_id,sizeof input.request_id,"%s",id);
    char text[16384];size_t used=0;int complete=0;
    while (!complete) {
        LmbMsg m={0};assert(!lmb_recv(fd,&m) && m.op==LMB_HOST_STREAM && !m.body_len && m.pay_len);
        assert(used+m.pay_len<sizeof text);
        memcpy(text+used,m.pay,m.pay_len);used+=m.pay_len;text[used]=0;
        assert(!host_output(&observed,&input,(const char *)m.pay,m.pay_len,&complete));
        lmb_msg_free(&m);
    }
    char terminal[100];snprintf(terminal,sizeof terminal,cancelled ? "ERROR %s request cancelled\n" : "DONE %s STAT 8 ",id);
    assert(strstr(text,terminal));
}
