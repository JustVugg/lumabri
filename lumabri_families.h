/* lumabri_families.h — which Colibri adapter runs which checkpoint.
 *
 * Lumabri mirrors Colibri's authoritative family registry here because the
 * runtime ABI exposes adapter registrations, not checkpoint model_type
 * aliases. model_family_test.sh compares the two registries directly, so an
 * alias or adapter added upstream cannot remain silently unmapped here.
 *
 * It used to be declared as three separate ladders of strstr(), and that is
 * not a mapping, it is a guess that always succeeds:
 *
 *     if (strstr(model_type, "glm"))  return "glm";
 *     if (strstr(model_type, "qwen")) return "qwen36";
 *
 * A GLM-5.3 checkpoint contains "glm", so it was handed to the GLM adapter.
 * A Qwen3.8 contains "qwen", so it was handed to qwen36. Not refused, not
 * warned about: executed by the wrong engine. And since neither `glm53` nor
 * `qwen38` was ever registered, those two adapters could not be reached at
 * all, by any spelling.
 *
 * So: one table, exact model_type aliases and NO prefix matching or fallback.
 * A checkpoint whose model_type nobody has declared is an
 * explicit error naming the string, not a silent hand-off to whichever row
 * happened to share three letters with it.
 *
 * The aliases below come from Colibri's authoritative family registry. */
#ifndef LUMABRI_FAMILIES_H
#define LUMABRI_FAMILIES_H

#include <stddef.h>
#include <string.h>

/* Header-only helpers: a translation unit uses the few it needs. */
#if defined(__GNUC__)
#define LMB_UNUSED __attribute__((unused))
#else
#define LMB_UNUSED
#endif

typedef struct {
    const char *segment_id;   /* Colibri Segment and Edge adapter id */
    const char *engine;       /* monolithic engine binary basename */
    const char *p2p_engine;   /* Lumabri chatter basename, NULL when absent */
    const char *expert_node;  /* phase-2 executor basename, NULL when none */
    const char *config_section; /* "root" or "text_config" */
    const char *aliases[6];     /* exact config.json model_type values */
} LmbModelFamily;

/* Every adapter Colibri registers has a row here. The parity test fails when
 * Colibri gains one that does not. */
static const LmbModelFamily LMB_FAMILIES[] = {
    { "olmoe",       "olmoe",    "olmoe", "expert_node", "root",
      { "olmoe", NULL } },
    { "deepseek_v4", "deepseek_v4", "deepseek", "expert_node_deepseek", "root",
      { "deepseek_v4", NULL } },
    { "kimi",        "kimi_k3",  "kimi_k3", "expert_node_kimi", "text_config",
      { "kimi_k3", "kimi_linear", NULL } },
    { "inkling",     "inkling",  "inkling", "expert_node_inkling", "text_config",
      { "inkling_mm_model", "inkling", NULL } },
    { "qwen36",      "qwen36",   "qwen36", "expert_node_qwen36", "text_config",
      { "qwen3_5_moe", "qwen3_5_moe_text", NULL } },
    { "glm",         "colibri",  "colibri", "expert_node_glm", "root",
      { "glm_moe_dsa", "glm5_moe", "glm", NULL } },
    { "glm53",       "glm53",    NULL, NULL, "text_config",
      { "glm5_next", "glm5_next_text", NULL } },
    { "qwen38",      "qwen38",   NULL, NULL, "text_config",
      { "qwen4_exp", "qwen4_exp_text", NULL } },
};
#define LMB_FAMILY_COUNT (sizeof LMB_FAMILIES / sizeof *LMB_FAMILIES)

static LMB_UNUSED const LmbModelFamily *lmb_family_by_id(const char *segment_id) {
    if (!segment_id || !segment_id[0]) return NULL;
    for (size_t i = 0; i < LMB_FAMILY_COUNT; i++)
        if (!strcmp(LMB_FAMILIES[i].segment_id, segment_id))
            return &LMB_FAMILIES[i];
    return NULL;
}

/* NULL means "no family claims this exact string". */
static LMB_UNUSED const LmbModelFamily *lmb_family_for(const char *model_type) {
    if (!model_type || !model_type[0]) return NULL;
    for (size_t i = 0; i < LMB_FAMILY_COUNT; i++) {
        const LmbModelFamily *f = &LMB_FAMILIES[i];
        for (int a = 0; a < 6 && f->aliases[a]; a++)
            if (!strcmp(model_type, f->aliases[a])) return f;
    }
    return NULL;
}

/* An operator with a checkpoint we have not mapped names the adapter
 * directly. Returns NULL when the name is not one Colibri registers, so a
 * typo is refused instead of silently ignored. */
static LMB_UNUSED const LmbModelFamily *lmb_family_override(const char *segment_id) {
    return lmb_family_by_id(segment_id);
}

#endif /* LUMABRI_FAMILIES_H */
