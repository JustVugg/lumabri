/* Prints "<resident bytes> <working set bytes>" for one range of a
 * checkpoint, so a test can derive its budgets from the same arithmetic the
 * node uses instead of hard-coding numbers that drift. */
#include <stdio.h>
#include <stdlib.h>
#include "lumabri_planner.h"

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s MODEL_DIR BEGIN END CONTEXT SESSIONS\n", argv[0]);
        return 2;
    }
    LmbModelShape m;
    if (lmb_shape_from_config(argv[1], &m)) {
        fprintf(stderr, "%s: cannot read a shape from config.json\n", argv[1]);
        return 1;
    }
    LmbRangeCost c = lmb_estimate_segment(&m, (uint32_t)atoi(argv[2]),
                                          (uint32_t)atoi(argv[3]),
                                          (uint32_t)atoi(argv[4]),
                                          (uint32_t)atoi(argv[5]));
    if (!c.ok) { fprintf(stderr, "that range cannot be estimated\n"); return 1; }
    uint64_t live = c.state_bytes + c.scratch_bytes;
    printf("%llu %llu\n", (unsigned long long)(c.resident_bytes + live),
           (unsigned long long)(c.working_set_bytes + live));
    return 0;
}
