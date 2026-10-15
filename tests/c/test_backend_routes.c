/* Exercise the production route and replica selectors, not a reimplementation.
 * Backend adverts below are fixtures; no GPU execution is claimed. */
#define main lmb_segment_cli_main
#include "../../segment_chat.c"
#undef main
#include <assert.h>

typedef struct {
    LmbSegRouteSnapshot snapshot;
    unsigned calls, nudges, ready_after;
    LmbServeControl *cancel;
} PendingDiscovery;

static int pending_snapshot(void *opaque, LmbSegRouteSnapshot *out, int refresh) {
    PendingDiscovery *pending=opaque;
    pending->calls++; pending->nudges+=(refresh!=0);
    *out=pending->snapshot;
    if (pending->calls<pending->ready_after) out->complete=0;
    if (pending->cancel && pending->calls==2) atomic_store(&pending->cancel->cancelled,1);
    return 1;
}

static void test_resume_selection(const LmbSegRouteSnapshot *valid) {
    PendingDiscovery pending={.snapshot=*valid,.ready_after=3};
    pending.snapshot.complete=1;
    LmbServeControl control={0};
    LmbSegRouteSnapshot snapshot;
    RemoteSegment chain[LMB_SEG_ROUTE_MAX];
    size_t count=99;
    assert(!serve_select_chain(pending_snapshot,&pending,&control,500,4,64,1,&snapshot,chain,&count));
    assert(count==1 && pending.calls==3 && pending.nudges==1);

    pending.calls=pending.nudges=0; pending.ready_after=1;
    assert(!serve_select_chain(pending_snapshot,&pending,&control,500,4,64,1,&snapshot,chain,&count));
    assert(pending.calls==1 && !pending.nudges); /* no extra work on a healthy turn */

    pending.calls=pending.nudges=0; pending.ready_after=UINT_MAX;
    double before=monotonic_seconds();
    assert(serve_select_chain(pending_snapshot,&pending,&control,100,4,64,1,&snapshot,chain,&count)==-1);
    assert(!count && pending.calls>=2 && pending.nudges<=1);
    assert(monotonic_seconds()-before<1.0); /* bounded even with no coverage */

    pending.calls=pending.nudges=0; pending.cancel=&control;
    assert(serve_select_chain(pending_snapshot,&pending,&control,5000,4,64,1,&snapshot,chain,&count)==-2);
    assert(!count && pending.calls==2); /* cancellation does not fetch again */
    atomic_store(&control.cancelled,0);
    pending.cancel=NULL; pending.calls=pending.nudges=0;
    pending.ready_after=1;
    for (uint32_t i=0;i<pending.snapshot.count;i++) pending.snapshot.entries[i].advert.flags|=LMB_SEG_ADVERT_DRAINING;
    assert(serve_select_chain(pending_snapshot,&pending,&control,100,4,64,1,&snapshot,chain,&count)==-1);
    assert(!count); /* a complete snapshot is not permission to use a drained node */
    snapshot=*valid; snapshot.count=LMB_SEG_ROUTE_MAX+1;
    assert(select_chain(&snapshot,4,64,1,chain,&count) && !count);
}

int main(void) {
    size_t stable;
    const char *replacement = "A\xef\xbf\xbd";
    assert(!generation_stable_bytes(NULL, 0, "A", 1, 0, 0, &stable) && !stable);
    assert(!generation_stable_bytes(replacement, 4, "A\xef\xbf\xbdX", 5, 1, 0, &stable) && stable == 1);
    assert(!generation_stable_bytes("A\xef\xbf\xbdX", 5, replacement, 4, 4, 0, &stable) && stable == 4);
    assert(generation_stable_bytes(replacement, 4, "AB", 2, 4, 0, &stable));
    assert(generation_stable_bytes("AB", 2, "AX", 2, 2, 0, &stable));
    assert(!generation_stable_bytes(replacement, 4, replacement, 4, 1, 1, &stable) && stable == 4);
    /* Invalid or recovered profiles expose no partially measured ranges. */
    GenerationResult observed = {.stages_valid = 0, .stage_count = 1};
    observed.stages[0].timing.decode_seconds = NAN;
    FILE *profile_file = tmpfile(); assert(profile_file);
    stage_observations_print(profile_file, &observed);
    rewind(profile_file);
    char output[4096]; size_t length = fread(output, 1, sizeof output - 1, profile_file);
    output[length] = 0; fclose(profile_file);
    assert(strstr(output, "\"valid\":false,\"stages\":[]"));
    assert(!strstr(output, "nan"));
    generation_result_free(&observed);
    assert(!observed.stage_count && !observed.stages_valid);
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
    /* Equal network predictions no longer hide a faster resident executor.
     * Until eight committed decode RUNs, the old route order is stable. */
    segment_backend_mask = LMB_SEG_CAP_CPU;
    for (unsigned i = 0; i < 2; i++) {
        snapshot.entries[i].advert.capabilities = LMB_SEG_CAP_CPU;
        snapshot.entries[i].advert.layer_begin = 0;
        snapshot.entries[i].advert.layer_end = 4;
        snapshot.entries[i].advert.queue_depth = 0;
        snapshot.entries[i].advert.inflight = 0;
        snapshot.entries[i].advert.flags = 0;
        snapshot.entries[i].predicted_us = 50000;
        snapshot.entries[i].compute.samples = 7;
        snapshot.entries[i].compute.ewma_us = i ? 100000 : 500000;
    }
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(!strcmp(chain[0].route.advert.peer_name, "node-0"));
    snapshot.entries[0].compute.samples = 8;
    snapshot.entries[1].compute.samples = 8;
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(!strcmp(chain[0].route.advert.peer_name, "node-1"));
    LmbSegRouteEntry gone = snapshot.entries[0];
    snprintf(gone.advert.peer_name, sizeof gone.advert.peer_name, "gone");
    assert(recovery_route_better(&snapshot.entries[1], &snapshot.entries[0],
                                 &gone, 0));
    snapshot.entries[1].advert.queue_depth = 5;
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(!strcmp(chain[0].route.advert.peer_name, "node-0"));
    assert(recovery_route_better(&snapshot.entries[0], &snapshot.entries[1],
                                 &gone, 0));
    snapshot.entries[1].advert.queue_depth = 0;
    snapshot.entries[1].predicted_us = UINT64_MAX / 4u;
    snapshot.entries[0].advert.flags = LMB_SEG_ADVERT_FALLBACK;
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(!strcmp(chain[0].route.advert.peer_name, "node-0"));
    assert(recovery_route_better(&snapshot.entries[0], &snapshot.entries[1],
                                 &gone, 0));
    snapshot.entries[1].predicted_us = 50000;
    snapshot.entries[1].compute.samples = 0; /* old peer: residency wins */
    assert(!select_chain(&snapshot, 4, 64, 1, chain, &count));
    assert(!strcmp(chain[0].route.advert.peer_name, "node-1"));
    test_resume_selection(&snapshot);
    puts("BACKEND ROUTES: PASS (CPU approval survives selection, missing coverage and replica checks)");
    return 0;
}
