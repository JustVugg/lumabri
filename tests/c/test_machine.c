#include "lumabri_machine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    LmbMachineProfile profile;
    assert(lmb_machine_probe(&profile, ".", NULL) == 0);
    assert(profile.logical_cpus > 0);
    assert(profile.physical_cores > 0);
    assert(profile.ram_total_bytes > 0);
    assert(profile.ram_available_bytes > 0);

    /* Never alter the real user's persistent pause state while testing. */
    char home[] = "/tmp/lumabri-machine.XXXXXX";
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(unsetenv("LUMABRI_GOVERNOR_FILE") == 0);
    assert(lmb_governor_set_manual(0) == 0);
    LmbGovernor governor;
    lmb_governor_init(&governor, 1);
    assert(lmb_governor_poll(&governor) == LMB_GOV_ACTIVE);
    assert(lmb_governor_reason(&governor) == LMB_GOV_REASON_NONE);
    assert(lmb_governor_set_manual(1) == 0);
    assert(lmb_governor_poll(&governor) == LMB_GOV_PAUSED);
    assert(lmb_governor_reason(&governor) == LMB_GOV_REASON_MANUAL);
    assert(!lmb_governor_accepting(&governor));
    assert(!lmb_governor_abort_inflight(&governor));
    atomic_store(&governor.reason, LMB_GOV_REASON_RAM_CRITICAL);
    assert(lmb_governor_abort_inflight(&governor));
    atomic_store(&governor.reason, LMB_GOV_REASON_SWAP_CRITICAL);
    assert(lmb_governor_abort_inflight(&governor));
    atomic_store(&governor.state, LMB_GOV_PRESSURE);
    assert(!lmb_governor_abort_inflight(&governor));
    assert(lmb_governor_set_manual(0) == 0);
    assert(lmb_governor_poll(&governor) == LMB_GOV_RECOVERY);
    assert(lmb_governor_reason(&governor) == LMB_GOV_REASON_RECOVERY);
    assert(lmb_governor_poll(&governor) == LMB_GOV_RECOVERY);
    assert(lmb_governor_poll(&governor) == LMB_GOV_ACTIVE);
    assert(lmb_governor_reason(&governor) == LMB_GOV_REASON_NONE);
    assert(lmb_governor_accepting(&governor));
    char state_dir[1200], state_file[1280];
    snprintf(state_dir, sizeof state_dir, "%s/.lumabri", home);
    snprintf(state_file, sizeof state_file, "%s/governor.state", state_dir);
    assert(unlink(state_file) == 0);
    assert(rmdir(state_dir) == 0);
    assert(rmdir(home) == 0);
    puts("MACHINE GOVERNOR UNIT: PASS (pause, drain, critical abort, recovery)");
    return 0;
}
