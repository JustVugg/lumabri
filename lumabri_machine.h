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
} LmbMachineProfile;

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
int lmb_governor_set_manual(int paused);
int lmb_governor_manual_paused(void);

#endif
