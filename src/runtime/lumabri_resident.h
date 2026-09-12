/* Household inference owns its weights in engine RAM, not a file mirror.
 * This boundary is separate from the public Colibri ABI. */
#ifndef LUMABRI_RESIDENT_H
#define LUMABRI_RESIDENT_H
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>

static int lmb_resident_required(void) {
    const char *s = getenv("LUMABRI_RESIDENT_REQUIRED");
    return s && !strcmp(s, "1");
}

/* Sealing disables external checkpoint-weight reads. Explicitly retained
 * raw tensor ranges remain readable from RAM for unchanged upstream kernels.
 * A missing hook cannot silently fall back to inference from disk. */
static int lmb_resident_seal(void) {
    if (!lmb_resident_required()) return 0;
    int (*seal)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "lmb_weights_seal");
    if (!seal || seal()) {
        fprintf(stderr, "[resident] cannot seal weight input; refusing READY\n");
        return -1;
    }
    return 0;
}
#endif
