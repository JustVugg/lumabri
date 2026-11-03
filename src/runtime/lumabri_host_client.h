/* Shared pinned Hosted connection. No model bootstrap or checkpoint access. */
#ifndef LUMABRI_HOST_CLIENT_H
#define LUMABRI_HOST_CLIENT_H
#include "lumabri_secure.h"
#include "lumabri_inventory.h"
#include "src/runtime/lumabri_chat_format.h"
#include <limits.h>

#define HOST_CONNECT_BUSY 1
#define HOST_CONNECT_ROOT_MISMATCH 2
typedef struct {
    char model_type[64], engine[64], backend[32], numeric_class[97], root[65];
    uint32_t sessions, speed_milli, max_new, max_frame, codec, sampling, numeric_abi;
    EngKind kind;
} LmbHostGreeting;

static int lmb_host_greeting(const LmbMsg *m,LmbHostGreeting *g) {
    memset(g,0,sizeof *g);g->kind=EK_INVALID;
    if (m->op!=LMB_HOST_HELLO_R || m->pay_len) return -1;
    LmbCur c={m->body,m->body_len,0};uint32_t tag;uint8_t digest[32];
    if (lmb_inventory_string(&c,g->model_type,sizeof g->model_type) || !g->model_type[0] ||
        lmb_inventory_string(&c,g->engine,sizeof g->engine) ||
        lmb_inventory_string(&c,g->backend,sizeof g->backend) ||
        lmb_cur_u32(&c,&g->sessions) || lmb_cur_u32(&c,&g->speed_milli)) return -1;
    if (c.off<c.len && (lmb_cur_u32(&c,&g->max_new) || g->max_new>INT_MAX ||
        lmb_cur_u32(&c,&g->max_frame))) return -1;
    if (c.off<c.len && (lmb_cur_u32(&c,&g->codec) || g->codec!=2)) return -1;
    if (c.off<c.len && (lmb_cur_u32(&c,&g->sampling) || (g->sampling&~7u))) return -1;
    if (c.off<c.len && (lmb_cur_u32(&c,&tag) || tag!=0x314d554e ||
        lmb_cur_u32(&c,&g->numeric_abi) || !g->numeric_abi ||
        lmb_inventory_string(&c,g->numeric_class,sizeof g->numeric_class) || !g->numeric_class[0])) return -1;
    if (c.off<c.len && (lmb_cur_u32(&c,&tag) || tag!=0x31544f52 ||
        lmb_inventory_string(&c,g->root,sizeof g->root) || strlen(g->root)!=64 ||
        lmb_unhex(digest,g->root,32))) return -1;
    g->kind=engine_kind_of(g->engine);
    return c.off!=c.len || g->kind==EK_INVALID ? -1 : 0;
}

/* BUSY is authenticated. An identity/root mismatch fails before any prompt.
 * Socket ownership transfers only on success; all failures close here. */
static int lmb_host_client_open(const char *address,const char *model_type,
        const char *expected_key,const char *expected_root,LmbSocket *socket,
        LmbHostGreeting *g) {
    *socket=LMB_INVALID_SOCKET;
    uint8_t key[32],root[32];
    if ((expected_key && (strlen(expected_key)!=64 || lmb_unhex(key,expected_key,32))) ||
        (expected_root && (!expected_key || strlen(expected_root)!=64 || lmb_unhex(root,expected_root,32)))) return -1;
    LmbSocket fd=lmb_connect_ms_io(address,3000,3000);
    if (fd==LMB_INVALID_SOCKET) return -1;
    LmbMsg m={0};
    int bad=(expected_key && (!lmb_secure_enabled() || !lmb_secure_peer_matches(fd,key))) ||
        lmb_auth(fd) || lmb_send(fd,LMB_HOST_HELLO,NULL,0,NULL,0) ||
        lmb_recv_bounded(fd,&m,4096,3000) || lmb_host_greeting(&m,g) ||
        (model_type && *model_type && strcmp(model_type,g->model_type));
    lmb_msg_free(&m);
    if (!bad && expected_root && strcmp(expected_root,g->root)) {lmb_close(fd);return HOST_CONNECT_ROOT_MISMATCH;}
    if (bad || !g->sessions) {lmb_close(fd);return bad ? -1 : HOST_CONNECT_BUSY;}
    if(lmb_socket_timeout(fd,lmb_env_int("LUMABRI_IO_TIMEOUT_MS",LMB_DEFAULT_IO_TIMEOUT_MS,100,3600000))) {
        lmb_close(fd);return -1;
    }
    *socket=fd;return 0;
}
#endif
