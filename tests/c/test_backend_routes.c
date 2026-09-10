/* Exercise the production route and replica selectors, not a reimplementation.
 * Backend adverts below are fixtures; no GPU execution is claimed. */
#define main lmb_segment_cli_main
#include "../../segment_chat.c"
#undef main
#include <assert.h>

int main(void) {
    LmbSegRouteSnapshot snapshot = {0};
    RemoteSegment chain[LMB_SEG_ROUTE_MAX];
    size_t count = 0;
    snapshot.count = 2;
    for (unsigned i = 0; i < 2; i++) {
        LmbSegRouteEntry *e = &snapshot.entries[i];
        snprintf(e->advert.peer_name, sizeof e->advert.peer_name, "node-%u", i);
        e->advert.layer_end = 4;
        e->advert.max_context = 64;
        e->advert.max_rows = 1;
        e->transport = LMB_SEG_TRANSPORT_DIRECT;
        e->predicted_us = i ? 1 : 1000;
        e->advert.capabilities = i ? LMB_SEG_CAP_CUDA : LMB_SEG_CAP_CPU;
    }
    segment_backend_mask = LMB_SEG_CAP_CPU;
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(count == 1 && !strcmp(chain[0].route.advert.peer_name, "node-0"));
    assert(!route_same_range(&snapshot.entries[1], &snapshot.entries[0], 64, 1));
    assert(route_same_range(&snapshot.entries[0], &snapshot.entries[0], 64, 1));

    /* A mixed advert is not evidence of CPU-only execution either. */
    snapshot.entries[0].advert.capabilities |= LMB_SEG_CAP_METAL;
    assert(select_chain(&snapshot, 4, 64, 1, chain, &count));
    snapshot.entries[0].advert.capabilities = 0;
    assert(select_chain(&snapshot, 4, 64, 1, chain, &count));

    /* Memory/coverage pressure cannot override approval at a boundary. */
    snapshot.entries[0].advert.capabilities = LMB_SEG_CAP_CPU;
    snapshot.entries[0].advert.layer_end = 2;
    snapshot.entries[1].advert.layer_begin = 2;
    assert(select_chain(&snapshot, 4, 64, 1, chain, &count));
    snapshot.entries[1].advert.capabilities = LMB_SEG_CAP_CPU;
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count) && count == 2);

    /* The low-level automatic policy remains unchanged. */
    snapshot.entries[0].advert.layer_end = 4;
    snapshot.entries[1].advert.layer_begin = 0;
    snapshot.entries[1].advert.capabilities = LMB_SEG_CAP_CUDA;
    segment_backend_mask = 0;
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(count == 1 && !strcmp(chain[0].route.advert.peer_name, "node-1"));
    assert(route_same_range(&snapshot.entries[1], &snapshot.entries[0], 64, 1));
    puts("BACKEND ROUTES: PASS (CPU approval survives selection, missing coverage and replica checks)");
    return 0;
}
