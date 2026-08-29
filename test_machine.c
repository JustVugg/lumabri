#include "lumabri_machine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    LmbMachineProfile profile;
    assert(lmb_machine_probe(&profile, ".", NULL) == 0);
    assert(profile.logical_cpus > 0);
    assert(profile.physical_cores > 0);
    assert(profile.ram_total_bytes > 0);
    assert(profile.ram_available_bytes > 0);

    assert(lmb_governor_set_manual(0) == 0);
    LmbGovernor governor;
    lmb_governor_init(&governor, 1);
    assert(lmb_governor_poll(&governor) == LMB_GOV_ACTIVE);
    assert(lmb_governor_set_manual(1) == 0);
    assert(lmb_governor_poll(&governor) == LMB_GOV_PAUSED);
    assert(!lmb_governor_accepting(&governor));
    assert(lmb_governor_set_manual(0) == 0);
    assert(lmb_governor_poll(&governor) == LMB_GOV_RECOVERY);
    assert(lmb_governor_poll(&governor) == LMB_GOV_RECOVERY);
    assert(lmb_governor_poll(&governor) == LMB_GOV_ACTIVE);
    assert(lmb_governor_accepting(&governor));
    puts("MACHINE GOVERNOR UNIT: PASS (pause, hysteresis, recovery)");
    return 0;
}
