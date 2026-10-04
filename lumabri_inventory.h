/* Signed, leased machine inventory. Resource reports are observations, not
 * reservations or proof that a model/backend can execute on a worker. */
#ifndef LUMABRI_INVENTORY_H
#define LUMABRI_INVENTORY_H
#include "lumabri_proto.h"
#include "lumabri_machine.h"
#include "src/planner/lumabri_resource_facts.h"
#include "src/planner/lumabri_workload_facts.h"

#define LMB_INVENTORY_VERSION 5u
#define LMB_INVENTORY_MAX 32u
#define LMB_INVENTORY_TTL_MS 15000u
#define LMB_INVENTORY_HEARTBEAT_MS 5000u

typedef struct {
    uint8_t identity[32];
    LmbMachineProfile machine;
    uint64_t ram_budget_bytes;
    uint32_t age_ms;
    char control_addr[64];      /* empty for an inventory-only worker */
    char runtime_id[65];        /* executable hashes + active donor epoch; empty = unknown */
    uint32_t runtime_threads;   /* queried installed engine, not detected CPU cores */
    LmbResourceFacts facts;
    LmbWorkloadFacts workload;
} LmbMachineReport;

/* Lengths and control characters are checked before any field reaches a
 * terminal. An authenticated machine name is still untrusted display input. */
static LMB_MAYBE_UNUSED int lmb_inventory_text(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || *p >= 127) return -1;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_inventory_string(LmbCur *c, char *out, size_t cap) {
    uint16_t n;
    if (lmb_cur_u16(c, &n) || n >= cap || c->off > c->len ||
        n > c->len - c->off || memchr(c->p + c->off, 0, n)) return -1;
    memcpy(out, c->p + c->off, n); out[n] = 0; c->off += n;
    return lmb_inventory_text(out);
}

static LMB_MAYBE_UNUSED int lmb_inventory_pack(LmbBuf *b,
                                               const LmbMachineReport *r) {
    const LmbMachineProfile *m = &r->machine;
    if (lmb_buf_u32(b, LMB_INVENTORY_VERSION) ||
        lmb_buf_bytes(b, r->identity, sizeof r->identity)) return -1;
#define INV_STR(f) if (lmb_buf_str(b, m->f)) return -1
    INV_STR(hostname); INV_STR(os); INV_STR(arch); INV_STR(cpu_model); INV_STR(isa);
#undef INV_STR
#define INV_U32(f) if (lmb_buf_u32(b, m->f)) return -1
    INV_U32(logical_cpus); INV_U32(physical_cores); INV_U32(numa_nodes);
    INV_U32(gpu_count); INV_U32(gpu_backends);
#undef INV_U32
#define INV_U64(f) if (lmb_buf_u64(b, m->f)) return -1
    INV_U64(ram_total_bytes); INV_U64(ram_available_bytes);
    INV_U64(vram_total_bytes); INV_U64(vram_available_bytes);
    INV_U64(disk_available_bytes); INV_U64(disk_read_bps);
#undef INV_U64
    return lmb_buf_u64(b, r->ram_budget_bytes) || lmb_buf_str(b, r->control_addr) ||
           lmb_buf_str(b, r->runtime_id) || lmb_buf_u32(b, r->runtime_threads) ||
           lmb_buf_u32(b, r->facts.known) || lmb_buf_u32(b, r->facts.load_milli) ||
           lmb_buf_u64(b, r->facts.price_micro_per_hour) ||
           lmb_buf_u32(b, r->facts.power_milliwatts) ||
           lmb_buf_bytes(b, r->facts.currency, 4) ||
           lmb_buf_u32(b, r->workload.known) || lmb_buf_u32(b, r->workload.allocations) ||
           lmb_buf_u32(b, r->workload.compute_policy) || lmb_buf_u32(b, r->workload.active) ||
           lmb_buf_u32(b, r->workload.queued) || lmb_buf_u64(b, r->workload.reserved_bytes) ||
           lmb_buf_bytes(b, r->workload.allocation_set, 32);
}

static LMB_MAYBE_UNUSED int lmb_inventory_unpack(LmbCur *c,
                                                 LmbMachineReport *r) {
    memset(r, 0, sizeof *r);
    LmbMachineProfile *m = &r->machine;
    uint32_t version;
    if (lmb_cur_u32(c, &version) || version < 3 || version > LMB_INVENTORY_VERSION ||
        c->len - c->off < sizeof r->identity) return -1;
    memcpy(r->identity, c->p + c->off, sizeof r->identity);
    c->off += sizeof r->identity;
#define INV_STR(f) if (lmb_inventory_string(c, m->f, sizeof m->f)) return -1
    INV_STR(hostname); INV_STR(os); INV_STR(arch); INV_STR(cpu_model); INV_STR(isa);
#undef INV_STR
#define INV_U32(f) if (lmb_cur_u32(c, &m->f)) return -1
    INV_U32(logical_cpus); INV_U32(physical_cores); INV_U32(numa_nodes);
    INV_U32(gpu_count); INV_U32(gpu_backends);
#undef INV_U32
#define INV_U64(f) if (lmb_cur_u64(c, &m->f)) return -1
    INV_U64(ram_total_bytes); INV_U64(ram_available_bytes);
    INV_U64(vram_total_bytes); INV_U64(vram_available_bytes);
    INV_U64(disk_available_bytes); INV_U64(disk_read_bps);
#undef INV_U64
    if (lmb_cur_u64(c, &r->ram_budget_bytes) ||
        lmb_inventory_string(c, r->control_addr, sizeof r->control_addr) ||
        lmb_inventory_string(c, r->runtime_id, sizeof r->runtime_id) ||
        lmb_cur_u32(c, &r->runtime_threads)) return -1;
    /* V3 peers remain usable, but missing load/cost data stays unknown. */
    if (version >= 4) {
        if (lmb_cur_u32(c, &r->facts.known) || lmb_cur_u32(c, &r->facts.load_milli) ||
            lmb_cur_u64(c, &r->facts.price_micro_per_hour) ||
            lmb_cur_u32(c, &r->facts.power_milliwatts) || c->off > c->len || c->len - c->off < 4) return -1;
        memcpy(r->facts.currency, c->p + c->off, 4); c->off += 4;
        if (!lmb_resource_facts_valid(&r->facts)) return -1;
    }
    if (version >= 5) {
        LmbWorkloadFacts *w = &r->workload;
        if (lmb_cur_u32(c, &w->known) || lmb_cur_u32(c, &w->allocations) ||
            lmb_cur_u32(c, &w->compute_policy) || lmb_cur_u32(c, &w->active) ||
            lmb_cur_u32(c, &w->queued) || lmb_cur_u64(c, &w->reserved_bytes) ||
            c->off > c->len || c->len-c->off < 32) return -1;
        memcpy(w->allocation_set, c->p+c->off, 32); c->off += 32;
        if (!lmb_workload_valid(w, m->ram_total_bytes)) return -1;
    }
    m->load_one = r->facts.known & LMB_FACT_LOAD ? r->facts.load_milli / 1000.0 : -1;
    if (r->runtime_threads > 256 || (!!r->runtime_threads != !!r->runtime_id[0])) return -1;
    if (r->runtime_id[0]) {
        if (strlen(r->runtime_id) != 64 || !r->control_addr[0]) return -1;
        for (unsigned i = 0; i < 64; i++)
            if (!((r->runtime_id[i] >= '0' && r->runtime_id[i] <= '9') ||
                  (r->runtime_id[i] >= 'a' && r->runtime_id[i] <= 'f'))) return -1;
    }
    uint8_t nonzero = 0;
    for (size_t i = 0; i < sizeof r->identity; i++) nonzero |= r->identity[i];
    if (!nonzero || !m->hostname[0] || !m->logical_cpus ||
        m->logical_cpus > 65536 || m->physical_cores > m->logical_cpus ||
        m->ram_total_bytes > (1ull << 50) || m->vram_total_bytes > (1ull << 50) ||
        m->ram_available_bytes > m->ram_total_bytes ||
        r->ram_budget_bytes > m->ram_available_bytes ||
        m->vram_available_bytes > m->vram_total_bytes) return -1;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_inventory_fetch(const char *tracker,
                                                LmbMachineReport *out,
                                                uint32_t *count) {
    *count = 0;
    LmbMsg m = {0};
    int fd = lmb_connect_ms_io(tracker, 1500, 2000);
    if (fd < 0) return -1;
    int rc = lmb_auth(fd) || lmb_send(fd, LMB_MACHINE_LIST, NULL, 0, NULL, 0) ||
             lmb_recv(fd, &m);
    lmb_close(fd);
    if (rc) { lmb_msg_free(&m); return -1; }
    LmbCur c = {m.body, m.body_len, 0};
    uint32_t version = 0, n = 0;
    int bad = m.op != LMB_MACHINE_LIST_R || m.pay_len ||
              lmb_cur_u32(&c, &version) || (version != 3 && version != LMB_INVENTORY_VERSION) ||
              lmb_cur_u32(&c, &n) || n > LMB_INVENTORY_MAX;
    for (uint32_t i = 0; !bad && i < n; i++) {
        uint32_t age = 0;
        bad = lmb_cur_u32(&c, &age);
        if (!bad) bad = lmb_inventory_unpack(&c, &out[i]);
        out[i].age_ms = age;
        if (age > LMB_INVENTORY_TTL_MS) bad = 1;
    }
    if (c.off != c.len) bad = 1;
    lmb_msg_free(&m);
    if (bad) return -1;
    *count = n;
    return 0;
}
#endif
