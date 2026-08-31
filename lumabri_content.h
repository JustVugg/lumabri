#ifndef LUMABRI_CONTENT_H
#define LUMABRI_CONTENT_H

#include <string.h>

/* Files created and mutated by a running engine belong to that machine, not
 * to the signed model inventory. Publishing one makes its next heartbeat
 * look like a model-integrity violation. Keep the prefix future-proof: every
 * current Colibri runtime artifact uses .coli_* (.coli_usage, .coli_kv,
 * .coli_ssd and .coli_ckpt), while checkpoint/model files do not. */
static inline int lmb_content_runtime_local_name(const char *name) {
    return name && (!strncmp(name, ".coli_", 6) ||
                    !strcmp(name, ".lumabri_hashes"));
}

#endif
