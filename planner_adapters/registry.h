#ifndef LUMABRI_PLAN_REGISTRY_H
#define LUMABRI_PLAN_REGISTRY_H
#include "qwen36.h"
#include "inkling.h"
#include "kimi.h"
#include "glm.h"
#include "glm53.h"
#include "qwen38.h"

typedef int (*LmbMemoryDescribe)(const char *, const char *, const char *, LmbModelShape *);
typedef struct { const char *adapter; LmbMemoryDescribe describe; } LmbMemoryContract;

/* Keep family-specific source inspection out of the catalogue and transport.
 * Older inspectors do not need the outer multimodal configuration. */
#define LMB_MEMORY_WRAPPER(name, expression) \
    static int name(const char *root, const char *whole, const char *cfg, LmbModelShape *m) { \
        (void)whole; (void)cfg; return (expression); \
    }
LMB_MEMORY_WRAPPER(lmb_describe_qwen36, lmb_qwen36_memory(root,m))
LMB_MEMORY_WRAPPER(lmb_describe_inkling, lmb_inkling_memory(root,cfg,m))
LMB_MEMORY_WRAPPER(lmb_describe_kimi, lmb_kimi_memory(root,cfg,m))
LMB_MEMORY_WRAPPER(lmb_describe_glm, lmb_glm_memory(root,cfg,m))
#undef LMB_MEMORY_WRAPPER

/* NULL is an explicit legacy formula, not a fallback for an unknown adapter.
 * model_family_test.sh compares these ids to Colibri's registry as well. */
static const LmbMemoryContract LMB_MEMORY_CONTRACTS[] = {
    {"olmoe", NULL},
    {"deepseek_v4", NULL},
    {"qwen36", lmb_describe_qwen36},
    {"inkling", lmb_describe_inkling},
    {"kimi", lmb_describe_kimi},
    {"glm", lmb_describe_glm},
    {"glm53", lmb_glm53_memory},
    {"qwen38", lmb_qwen38_memory},
};

static int lmb_describe_memory(const char *id, const char *root, const char *whole,
                               const char *cfg, LmbModelShape *m) {
    for(size_t i=0;i<sizeof LMB_MEMORY_CONTRACTS/sizeof *LMB_MEMORY_CONTRACTS;i++) {
        const LmbMemoryContract *c=&LMB_MEMORY_CONTRACTS[i];
        if(!strcmp(id,c->adapter)) return c->describe ? c->describe(root,whole,cfg,m) : 0;
    }
    return -1;
}
#endif
