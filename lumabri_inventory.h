/* Signed, leased machine inventory. Resource reports are observations, not
 * reservations or proof that a model/backend can execute on a worker. */
#ifndef LUMABRI_INVENTORY_H
#define LUMABRI_INVENTORY_H
#include "lumabri_proto.h"
#include "lumabri_machine.h"

#define LMB_INVENTORY_VERSION 1u
#define LMB_INVENTORY_MAX 32u
#define LMB_INVENTORY_TTL_MS 15000u
#define LMB_INVENTORY_HEARTBEAT_MS 5000u

typedef struct {
    uint8_t identity[32];
    LmbMachineProfile machine;
    uint64_t ram_budget_bytes;
    uint32_t age_ms;
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
    return lmb_buf_u64(b, r->ram_budget_bytes);
}

static LMB_MAYBE_UNUSED int lmb_inventory_unpack(LmbCur *c,
                                                 LmbMachineReport *r) {
    memset(r, 0, sizeof *r);
    LmbMachineProfile *m = &r->machine;
    uint32_t version;
    if (lmb_cur_u32(c, &version) || version != LMB_INVENTORY_VERSION ||
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
    if (lmb_cur_u64(c, &r->ram_budget_bytes)) return -1;
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
              lmb_cur_u32(&c, &version) || version != LMB_INVENTORY_VERSION ||
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
