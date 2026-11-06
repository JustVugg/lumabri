/* Discovery and execution are different facts. Upstream metadata can recognize
 * a model without granting a runtime, memory contract, or modality to it. */
#ifndef LMB_CAPABILITIES_H
#define LMB_CAPABILITIES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum {
    LMB_OP_CHAT = 1u, LMB_OP_IMAGE_INPUT = 2u, LMB_OP_AUDIO_INPUT = 4u,
    LMB_OP_GENERATE_IMAGE = 8u, LMB_OP_DECIDE = 16u,
    LMB_OP_TOOLS = 32u, LMB_OP_LOGPROBS = 64u
};
typedef struct {
    const char *id, *engine, *modality, *aliases;
    uint32_t upstream_operations, upstream_max_context;
} LmbFamilyCapabilities;
#include "lumabri_colibri_capabilities_data.h"

static inline const LmbFamilyCapabilities *lmb_capability_for(const char *type) {
    if (!type || !*type || strchr(type,'|')) return NULL;
    size_t n=strlen(type);
    for (size_t i=0;i<sizeof LMB_COLIBRI_CAPABILITIES/sizeof *LMB_COLIBRI_CAPABILITIES;i++) {
        const LmbFamilyCapabilities *c=&LMB_COLIBRI_CAPABILITIES[i];
        for (const char *p=c->aliases; *p; ) {
            const char *end=strchr(++p,'|');
            if (!end) break;
            if ((size_t)(end-p)==n && !memcmp(p,type,n)) return c;
            p=end;
        }
    }
    return NULL;
}

enum { LMB_FORMAT_SAFETENSORS = 1u, LMB_FORMAT_GGUF = 2u, LMB_FORMAT_OTHER = 4u };
static inline const char *lmb_weight_format_name(uint32_t mask) {
    return mask==LMB_FORMAT_SAFETENSORS ? "safetensors" :
        mask==LMB_FORMAT_GGUF ? "gguf" : !mask ? "unknown" :
        mask==LMB_FORMAT_OTHER ? "other" : "mixed";
}
#endif
