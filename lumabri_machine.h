#ifndef LUMABRI_MACHINE_H
#define LUMABRI_MACHINE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    char hostname[64];
    char os[64];
    char arch[32];
    char cpu_model[128];
    char isa[32];
    uint32_t logical_cpus;
    uint32_t physical_cores;
    uint32_t numa_nodes;
    uint64_t ram_total_bytes;
    uint64_t ram_available_bytes;
    uint64_t swap_total_bytes;
    uint64_t swap_free_bytes;
    uint32_t gpu_count;
    uint64_t vram_total_bytes;
    uint64_t vram_available_bytes;
    uint64_t disk_available_bytes;
    uint32_t network_interfaces;
    uint32_t public_ipv4;
    double load_one;
    double tracker_rtt_ms;
    /* Three things a planner needs and a profiler did not answer.
     *
     * A GPU that /sys reports is not a GPU the engine can use: Colibri's
     * Segment adapters advertise CPU only until an adapter exposes a real
     * backend, so "has a card" and "runs on the card" are different claims
     * and only the second may be shown as a fast host.
     *
     * Disk read speed decides whether a range that does not fit RAM is a
     * usable disk mode or a promise; a slow spinning disk is not an NVMe.
     *
     * LAN bandwidth decides how long distributing the weights takes, which
     * is the number the catalogue quotes as "ready in". None of them can be
     * assumed, so zero means unmeasured and prints as such. */
    uint32_t gpu_backends;        /* LMB_GPU_* bits the ENGINE can use */
    uint64_t disk_read_bps;       /* measured sequential read, 0 = unmeasured */
    uint64_t lan_bps;             /* measured to the nearest peer, 0 = unmeasured */
} LmbMachineProfile;

/* Backends an engine build can actually reach on this machine. Detected from
 * the binary's own capabilities, never from the presence of a device node. */
enum {
    LMB_GPU_NONE   = 0,
    LMB_GPU_CUDA   = 1u << 0,
    LMB_GPU_HIP    = 1u << 1,
    LMB_GPU_METAL  = 1u << 2,
    LMB_GPU_VULKAN = 1u << 3,
};

typedef enum {
    LMB_GOV_ACTIVE = 0,
    LMB_GOV_PRESSURE = 1,
    LMB_GOV_PAUSED = 2,
    LMB_GOV_RECOVERY = 3,
} LmbGovernorState;

typedef enum {
    LMB_GOV_REASON_NONE = 0,
    LMB_GOV_REASON_MANUAL,
    LMB_GOV_REASON_RAM_PRESSURE,
    LMB_GOV_REASON_RAM_CRITICAL,
    LMB_GOV_REASON_SWAP_CRITICAL,
    LMB_GOV_REASON_RECOVERY,
} LmbGovernorReason;

typedef struct {
    uint64_t ram_reserve_bytes;
    unsigned recovery_ticks;
    _Atomic int state;
    _Atomic int reason;
} LmbGovernor;

int lmb_machine_probe(LmbMachineProfile *profile, const char *disk_path,
                      const char *tracker);
/* Parse Linux /proc/meminfo. Exposed so compatibility fixtures can exercise
 * kernels that predate MemAvailable without replacing the process procfs. */
int lmb_machine_read_meminfo(FILE *stream, uint64_t *total,
                             uint64_t *available, uint64_t *swap_total,
                             uint64_t *swap_free);
void lmb_machine_print(FILE *out, const LmbMachineProfile *profile, int json);
uint64_t lmb_machine_available_ram(void);
uint64_t lmb_machine_total_ram(void);
/* One automatic compute donor owns the machine's donable RAM. The returned
 * descriptor is the lease and must stay open for the donation lifetime. */
int lmb_machine_compute_lease_acquire(const char *model, const char *tracker,
                                      char *owner, size_t owner_size);
const char *lmb_governor_state_name(LmbGovernorState state);
const char *lmb_governor_reason_name(LmbGovernorReason reason);
void lmb_governor_init(LmbGovernor *governor, uint64_t ram_reserve_bytes);
LmbGovernorState lmb_governor_poll(LmbGovernor *governor);
LmbGovernorState lmb_governor_state(const LmbGovernor *governor);
LmbGovernorReason lmb_governor_reason(const LmbGovernor *governor);
int lmb_governor_accepting(const LmbGovernor *governor);
/* Existing work drains through ordinary pressure and an operator pause.  Only
 * a genuinely critical RAM/swap condition may abort an in-flight kernel. */
int lmb_governor_abort_inflight(const LmbGovernor *governor);
int lmb_governor_set_manual(int paused);
int lmb_governor_manual_paused(void);

#endif
