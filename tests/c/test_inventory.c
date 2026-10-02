#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "lumabri_inventory.h"
#include <assert.h>

static void optional_facts(void) {
    unsetenv("LUMABRI_COST_PER_HOUR"); unsetenv("LUMABRI_COST_CURRENCY");
    unsetenv("LUMABRI_ESTIMATED_POWER_WATTS");
    LmbResourceFacts f = lmb_resource_facts_local(-1);
    assert(!f.known && lmb_resource_facts_valid(&f));
    setenv("LUMABRI_COST_PER_HOUR", "0", 1); setenv("LUMABRI_COST_CURRENCY", "EUR", 1);
    setenv("LUMABRI_ESTIMATED_POWER_WATTS", "12.125", 1);
    f = lmb_resource_facts_local(0);
    assert(f.known == 7 && f.load_milli == 0 && f.price_micro_per_hour == 0 &&
           f.power_milliwatts == 12125 && lmb_resource_facts_valid(&f));
    const char *invalid[] = {"-1", "nan", "inf", " 1", "1e2", "1,5", ".5", "1.", "1.0000001", "1000001", "18446744073709551615"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        setenv("LUMABRI_COST_PER_HOUR", invalid[i], 1);
        f = lmb_resource_facts_local(NAN);
        assert(!(f.known & (LMB_FACT_LOAD | LMB_FACT_PRICE)) && lmb_resource_facts_valid(&f));
    }
    setenv("LUMABRI_COST_PER_HOUR", "1.234567", 1);
    f = lmb_resource_facts_local(2.5); assert(f.price_micro_per_hour == 1234567 && f.load_milli == 2500);
    setenv("LUMABRI_COST_CURRENCY", "eur", 1);
    f = lmb_resource_facts_local(INFINITY); assert(!(f.known & (LMB_FACT_PRICE | LMB_FACT_LOAD)));
    unsetenv("LUMABRI_COST_PER_HOUR"); unsetenv("LUMABRI_COST_CURRENCY");
    unsetenv("LUMABRI_ESTIMATED_POWER_WATTS");
}

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
    optional_facts();
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
    /* V3 runtime identity must describe an actual, addressable donor. A
     * partial or non-canonical identity cannot bind a speed measurement. */
    r = fixture();
    snprintf(r.control_addr, sizeof r.control_addr, "127.0.0.1:47301");
    memset(r.runtime_id, 'a', 64); r.runtime_id[64] = 0;
    r.runtime_threads = 2;
    b = (LmbBuf){0}; assert(!lmb_inventory_pack(&b, &r)); assert(!decode(&b));
    LmbCur cur = {b.p, b.len, 0}; LmbMachineReport got;
    assert(!lmb_inventory_unpack(&cur, &got) && got.runtime_threads == 2 &&
           !strcmp(got.runtime_id, r.runtime_id));
    /* V3 has no optional facts: preserve absence, not an idle/free reading. */
    b.p[0] = 3; b.len -= 24;
    cur = (LmbCur){b.p, b.len, 0};
    assert(!lmb_inventory_unpack(&cur, &got) && !got.facts.known && got.machine.load_one < 0 && cur.off == cur.len);
    b.p[0] = 4; b.len += 24;
    for (size_t i = 0; i < b.len; i++) {
        LmbBuf cut = b; cut.len = i; assert(decode(&cut));
    }
    b.p[0] = 2; assert(decode(&b)); free(b.p);
    for (unsigned i = 0; i < 6; i++) {
        LmbMachineReport invalid = r;
        if (i == 0) invalid.facts.known = 8;
        if (i == 1) invalid.facts.load_milli = 1;
        if (i == 2) { invalid.facts.known = LMB_FACT_LOAD; invalid.facts.load_milli = UINT32_MAX; }
        if (i == 3) { invalid.facts.known = LMB_FACT_PRICE; memcpy(invalid.facts.currency, "eUR", 4); }
        if (i == 4) invalid.facts.price_micro_per_hour = 1;
        if (i == 5) { invalid.facts.known = LMB_FACT_POWER; invalid.facts.power_milliwatts = UINT32_MAX; }
        b = (LmbBuf){0}; assert(!lmb_inventory_pack(&b, &invalid)); assert(decode(&b)); free(b.p);
    }
    for (unsigned i = 0; i < 5; i++) {
        LmbMachineReport invalid = r;
        if (i == 0) invalid.runtime_threads = 0;
        if (i == 1) invalid.runtime_threads = 257;
        if (i == 2) invalid.runtime_id[0] = 'G';
        if (i == 3) invalid.runtime_id[63] = 0;
        if (i == 4) invalid.control_addr[0] = 0;
        b = (LmbBuf){0}; assert(!lmb_inventory_pack(&b, &invalid));
        assert(decode(&b)); free(b.p);
    }
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
