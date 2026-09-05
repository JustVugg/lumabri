#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "lumabri_inventory.h"
#include <assert.h>

static LmbMachineReport fixture(void) {
    LmbMachineReport r = {0};
    r.identity[0] = 1;
    snprintf(r.machine.hostname, sizeof r.machine.hostname, "test-worker");
    r.machine.logical_cpus = 8; r.machine.physical_cores = 4;
    r.machine.ram_total_bytes = 16ull << 30;
    r.machine.ram_available_bytes = 12ull << 30;
    r.ram_budget_bytes = 8ull << 30;
    return r;
}

static int decode(const LmbBuf *b) {
    LmbCur c = {b->p, b->len, 0}; LmbMachineReport r;
    return lmb_inventory_unpack(&c, &r) || c.off != c.len;
}

int main(int argc, char **argv) {
    LmbMachineReport r = fixture();
    LmbBuf b = {0};
    assert(!lmb_inventory_pack(&b, &r));
    assert(!decode(&b));
    for (size_t i = 0; i < b.len; i++) {
        LmbBuf cut = b; cut.len = i; assert(decode(&cut));
    }
    free(b.p); b = (LmbBuf){0};
    r.ram_budget_bytes = r.machine.ram_total_bytes + 1;
    assert(!lmb_inventory_pack(&b, &r)); assert(decode(&b));
    free(b.p); b = (LmbBuf){0}; r = fixture();
    r.machine.hostname[0] = '\033';
    assert(!lmb_inventory_pack(&b, &r)); assert(decode(&b));
    free(b.p); b = (LmbBuf){0}; r = fixture();
    assert(!lmb_inventory_pack(&b, &r));
    b.p[4 + 32 + 2] = 0; /* embedded NUL in the declared machine name */
    assert(decode(&b)); free(b.p);
    if (argc == 2) {
        assert(!lmb_secure_init());
        int fd = lmb_connect(argv[1]); assert(fd >= 0); assert(!lmb_auth(fd));
        LmbMsg m = {0};
        assert(!lmb_send(fd, LMB_CHALLENGE, NULL, 0, NULL, 0));
        assert(!lmb_recv(fd, &m) && m.op == LMB_CHALLENGE_R);
        lmb_msg_free(&m);
        char path[1024]; uint8_t sk[64], bad_signature[64] = {0};
        r = fixture();
        assert(!lmb_peer_identity(lmb_peer_key_path(path, sizeof path), sk, r.identity));
        b = (LmbBuf){0}; assert(!lmb_inventory_pack(&b, &r));
        assert(!lmb_buf_bytes(&b, bad_signature, sizeof bad_signature));
        assert(!lmb_send(fd, LMB_MACHINE_REPORT, b.p, (uint32_t)b.len, NULL, 0));
        assert(lmb_recv(fd, &m) || m.op != LMB_OK);
        lmb_msg_free(&m); lmb_close(fd); free(b.p);
    }
    puts("INVENTORY VALIDATION: PASS");
    return 0;
}
