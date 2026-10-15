/* Deterministic guard contract only; native allocation/locking is tested by
 * test_engine_memory and actual OLMoE integration, not these stubs. */
#define _GNU_SOURCE
#define LMB_ENGINE_MEMORY_OVERRIDE_H
#include "src/runtime/lumabri_resident_guard.h"
#include <assert.h>

static LmbEngineMemory owned;
static int raw_bad;
void lmb_engine_memory_snapshot(LmbEngineMemory *out) { *out=owned; }
static int raw_status(uint64_t *bytes) { *bytes=4096; return raw_bad; }
static LmbResidentGuard guard(void) {
    LmbResidentGuard g={.enabled=1,.engine_locked=1,.retained_lock_status=raw_status};
    return g;
}
int main(void) {
    LmbProcessMemory m={.known=LMB_PM_RSS|LMB_PM_COMPRESSED,.rss=16384,.compressed=4096};
    owned=(LmbEngineMemory){.enabled=1,.allocations=1,.requested_bytes=8,.locked_bytes=4096};
    LmbResidentGuard g=guard();
    assert(!lmb_resident_guard_observe(&g,&m)); /* unrelated pages remain reported */
    assert(m.compressed==4096);
    g.retained_lock_status=NULL;
    assert(lmb_resident_guard_observe(&g,&m));
    g.retained_lock_status=raw_status;
    assert(lmb_resident_guard_observe(&g,&m)); /* latched, cannot resurrect */
    for (int fault=0;fault<6;fault++) {
        g=guard(); raw_bad=0;
        owned=(LmbEngineMemory){.enabled=1,.allocations=1,.requested_bytes=8,.locked_bytes=4096};
        if (fault==0) owned.enabled=0;
        if (fault==1) owned.fault=ENOMEM;
        if (fault==2) owned.allocations=0;
        if (fault==3) owned.locked_bytes=7;
        if (fault==4) raw_bad=1;
        if (fault==5) m.known=0;
        assert(lmb_resident_guard_observe(&g,&m));
    }
    puts("ENGINE GUARD: PASS (owned locks and retained tables required, latched failure, unknown evidence refused)");
    return 0;
}
