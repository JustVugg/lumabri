#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "src/protocol/lumabri_secure_channel.h"
#include <assert.h>
#include <pthread.h>

typedef struct {
    LmbSocket socket;
    uint8_t sk[64],pk[32],expected[32];
    LmbSecure channel;
} Peer;

static void connect_pair(LmbSocket *client,LmbSocket *server) {
    LmbSocket listener=lmb_listen(0); assert(listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in addr; LmbSocketLength length=sizeof addr;
    assert(!getsockname(listener,(struct sockaddr *)&addr,&length));
    char endpoint[64]; snprintf(endpoint,sizeof endpoint,"127.0.0.1:%u",ntohs(addr.sin_port));
    *client=lmb_connect_ms_io(endpoint,1000,2000); assert(*client!=LMB_INVALID_SOCKET);
    *server=lmb_socket_accept(listener,NULL,NULL); assert(*server!=LMB_INVALID_SOCKET);
    assert(!lmb_socket_nodelay(*server)); lmb_close(listener);
}

static void *accept_peer(void *opaque) {
    Peer *peer=opaque;
    assert(!lmb_secure_handshake(peer->socket,0,peer->sk,peer->pk,&peer->channel));
    /* Trust is explicit and checked before receiving application data. */
    assert(peer->channel.have_peer_id && !memcmp(peer->channel.peer_id,peer->expected,32));
    for (unsigned i=0;i<8;i++) {
        LmbMsg m={0}; assert(!lmb_secure_recv(&peer->channel,peer->socket,&m));
        assert(m.op==LMB_SEG_RUN && m.body_len==4 && lmb_get32(m.body)==i);
        assert(!lmb_secure_send(&peer->channel,peer->socket,LMB_SEG_RUN_R,
                               m.body,m.body_len,m.pay,m.pay_len));
        lmb_msg_free(&m);
    }
    return NULL;
}

int main(void) {
    Peer client={0},server={0}; uint8_t seed[32];
    lmb_random(seed,sizeof seed); lmb_sign_keypair(client.pk,client.sk,seed);
    lmb_random(seed,sizeof seed); lmb_sign_keypair(server.pk,server.sk,seed);
    memset(seed,0,sizeof seed);
    memcpy(client.expected,server.pk,32); memcpy(server.expected,client.pk,32);
    connect_pair(&client.socket,&server.socket);
    pthread_t thread; assert(!pthread_create(&thread,NULL,accept_peer,&server));
    assert(!lmb_secure_handshake(client.socket,1,client.sk,client.pk,&client.channel));
    assert(client.channel.have_peer_id && !memcmp(client.channel.peer_id,client.expected,32));
    uint8_t payload[8192]; lmb_random(payload,sizeof payload);
    for (unsigned i=0;i<8;i++) {
        uint8_t body[4]; lmb_put32(body,i);
        assert(!lmb_secure_send(&client.channel,client.socket,LMB_SEG_RUN,body,sizeof body,payload,sizeof payload));
        LmbMsg m={0}; assert(!lmb_secure_recv(&client.channel,client.socket,&m));
        assert(m.op==LMB_SEG_RUN_R && m.body_len==sizeof body && m.pay_len==sizeof payload);
        assert(!memcmp(m.body,body,sizeof body) && !memcmp(m.pay,payload,sizeof payload));
        lmb_msg_free(&m);
    }
    assert(!pthread_join(thread,NULL) && !atomic_load(&lmb_rx_inflight));
    assert(client.channel.tx_ctr==8 && server.channel.rx_ctr==8);

    /* Retain an actual authenticated record, then replay it. No app payload
     * may escape on either an invalid tag or a previously consumed nonce. */
    uint8_t wire[16+4+16]={0},nonce[12],plain[4]={1,2,3,4};
    lmb_put32(wire,LMB_MAGIC); lmb_put32(wire+4,LMB_SEG_RUN); lmb_put32(wire+8,4);
    lmb_sec_nonce(nonce,client.channel.tx_ctr++);
    lc_aead_seal(client.channel.tx_key,nonce,wire,16,plain,sizeof plain,wire+16,wire+20);
    assert(!lmb_write_full(client.socket,wire,sizeof wire));
    LmbMsg m={0}; assert(!lmb_secure_recv(&server.channel,server.socket,&m));
    assert(m.body_len==4 && !memcmp(m.body,plain,4)); lmb_msg_free(&m);
    assert(!lmb_write_full(client.socket,wire,sizeof wire));
    assert(lmb_secure_recv(&server.channel,server.socket,&m)<0 && errno==EBADMSG);
    assert(!m.body && !m.pay && !m.storage && !atomic_load(&lmb_rx_inflight));
    lmb_close(client.socket); lmb_close(server.socket);

    connect_pair(&client.socket,&server.socket);
    lmb_sec_nonce(nonce,server.channel.rx_ctr);
    lc_aead_seal(client.channel.tx_key,nonce,wire,16,plain,sizeof plain,wire+16,wire+20);
    wire[22]^=1;
    assert(!lmb_write_full(client.socket,wire,sizeof wire));
    assert(lmb_secure_recv(&server.channel,server.socket,&m)<0 && errno==EBADMSG);
    assert(!m.body && !m.pay && !atomic_load(&lmb_rx_inflight));
    lmb_close(client.socket); lmb_close(server.socket);
    puts("NATIVE SECURE CHANNEL: PASS (OS entropy, signed identities, encrypted Segment frames, replay and tamper refusal)");
    return 0;
}
