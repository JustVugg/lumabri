/* lumabri_families.h — which Colibri adapter runs which checkpoint.
 *
 * Colibri does not answer this question. Only DeepSeek V4 checks
 * `model_type` at all (`require_string(root, "model_type", "deepseek_v4")`);
 * the other seven adapters never read it, so there is no table in the engine
 * to consult and the mapping has to be declared here.
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
 * So: one table, exact aliases and declared prefixes, longest match wins,
 * and NO fallback. A checkpoint whose model_type nobody has declared is an
 * explicit error naming the string, not a silent hand-off to whichever row
 * happened to share three letters with it.
 *
 * The `aliases` of a family may legitimately be empty. That means the
 * adapter is registered and usable, but we do not yet know what its
 * checkpoints write in config.json — it is reachable only through an
 * explicit override. Those families are named in LMB_FAMILY_UNMAPPED, which
 * the parity test keeps shrink-only: it may never grow without someone
 * looking at a real checkpoint. */
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
    const char *expert_node;  /* phase-2 executor basename, NULL when none */
    /* exact config.json model_type values, then declared prefixes. Longest
     * match wins, so a more specific family always beats a broader one. */
    const char *aliases[6];
    const char *prefixes[4];
} LmbModelFamily;

/* Every adapter Colibri registers has a row here. The parity test fails when
 * Colibri gains one that does not. */
static const LmbModelFamily LMB_FAMILIES[] = {
    { "olmoe",       "olmoe",    "expert_node",
      { "olmoe", NULL }, { NULL } },
    { "deepseek_v4", "deepseek", "expert_node_deepseek",
      { "deepseek_v4", NULL }, { NULL } },
    { "kimi",        "kimi_k3",  "expert_node_kimi",
      { NULL }, { "kimi", NULL } },
    { "inkling",     "inkling",  "expert_node_inkling",
      { NULL }, { "inkling", NULL } },
    { "qwen36",      "qwen36",   "expert_node_qwen36",
      { NULL }, { "qwen3_", "qwen36", NULL } },
    { "glm",         "colibri",  "expert_node_glm",
      { NULL }, { "glm", NULL } },
    /* Registered and usable, but no checkpoint of theirs has been read yet,
     * so nothing is claimed about their model_type. Reachable through the
     * explicit override until someone looks at one. */
    { "glm53",       "colibri",  "expert_node_glm",   { NULL }, { NULL } },
    { "qwen38",      "qwen36",   "expert_node_qwen36", { NULL }, { NULL } },
};
#define LMB_FAMILY_COUNT (sizeof LMB_FAMILIES / sizeof *LMB_FAMILIES)

/* Shrink-only. A row belongs here exactly while its aliases and prefixes are
 * both empty; the parity test enforces both directions, so this list cannot
 * quietly grow when a new adapter arrives. */
static const char *const LMB_FAMILY_UNMAPPED[] = { "glm53", "qwen38" };
#define LMB_FAMILY_UNMAPPED_COUNT \
    (sizeof LMB_FAMILY_UNMAPPED / sizeof *LMB_FAMILY_UNMAPPED)

static LMB_UNUSED const LmbModelFamily *lmb_family_by_id(const char *segment_id) {
    if (!segment_id || !segment_id[0]) return NULL;
    for (size_t i = 0; i < LMB_FAMILY_COUNT; i++)
        if (!strcmp(LMB_FAMILIES[i].segment_id, segment_id))
            return &LMB_FAMILIES[i];
    return NULL;
}

/* The checkpoint's model_type decides, and the longest declared match wins.
 * NULL means "no family claims this string" — the caller must say so out
 * loud rather than pick one. */
static LMB_UNUSED const LmbModelFamily *lmb_family_for(const char *model_type) {
    if (!model_type || !model_type[0]) return NULL;
    const LmbModelFamily *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < LMB_FAMILY_COUNT; i++) {
        const LmbModelFamily *f = &LMB_FAMILIES[i];
        for (int a = 0; a < 6 && f->aliases[a]; a++)
            if (!strcmp(model_type, f->aliases[a])) return f;   /* exact wins */
        for (int p = 0; p < 4 && f->prefixes[p]; p++) {
            size_t n = strlen(f->prefixes[p]);
            if (strncmp(model_type, f->prefixes[p], n)) continue;
            if (n > best_len) { best = f; best_len = n; }
        }
    }
    return best;
}

/* An operator with a checkpoint we have not mapped names the adapter
 * directly. Returns NULL when the name is not one Colibri registers, so a
 * typo is refused instead of silently ignored. */
static LMB_UNUSED const LmbModelFamily *lmb_family_override(const char *segment_id) {
    return lmb_family_by_id(segment_id);
}

#endif /* LUMABRI_FAMILIES_H */
