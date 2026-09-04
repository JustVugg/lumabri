/* The table decides, and it decides the same way every time.
 *
 * The dispatch this replaces was three ladders of strstr(), and its failure
 * was not that it refused things — it was that it never did. "glm53_moe"
 * contains "glm", so a GLM-5.3 checkpoint was handed to the GLM adapter and
 * executed. That is worse than an error: an error stops, a wrong adapter
 * produces numbers.
 *
 * So the claims here are about what the table REFUSES as much as what it
 * resolves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lumabri_families.h"

static int bad;

static void want(const char *model_type, const char *expect_id) {
    const LmbModelFamily *f = lmb_family_for(model_type);
    const char *got = f ? f->segment_id : NULL;
    if (!expect_id && !got) return;
    if (expect_id && got && !strcmp(got, expect_id)) return;
    fprintf(stderr, "model_type \"%s\": got %s, expected %s\n", model_type,
            got ? got : "(refused)", expect_id ? expect_id : "(refused)");
    bad = 1;
}

int main(void) {
    /* the two the table knows exactly */
    want("olmoe", "olmoe");
    want("deepseek_v4", "deepseek_v4");

    /* the families that still match by declared prefix, unchanged */
    want("kimi_k2", "kimi");
    want("inkling", "inkling");
    want("qwen3_moe", "qwen36");
    want("glm4_moe", "glm");

    /* the refusals. Nothing here shares enough with a declared prefix to be
     * claimed, and every one of them used to be swallowed silently. */
    want("", NULL);
    want("llama", NULL);
    want("mixtral", NULL);
    want("deepseek_v3", NULL);   /* "deepseek" was a prefix once: not any more */

    /* a family with no declared model_type is unreachable by detection... */
    const LmbModelFamily *f;
    for (size_t i = 0; i < LMB_FAMILY_UNMAPPED_COUNT; i++) {
        const char *id = LMB_FAMILY_UNMAPPED[i];
        f = lmb_family_by_id(id);
        if (!f) { fprintf(stderr, "unmapped family %s has no row\n", id); bad = 1; continue; }
        if (f->aliases[0] || f->prefixes[0]) {
            fprintf(stderr, "%s is listed as unmapped but claims a model_type\n", id);
            bad = 1;
        }
        /* ...and reachable by explicit name, which is the whole point of
         * registering it. */
        if (lmb_family_override(id) != f) {
            fprintf(stderr, "%s cannot be selected explicitly\n", id);
            bad = 1;
        }
    }

    /* a name Colibri does not register is refused, not ignored */
    if (lmb_family_override("qwen39")) {
        fprintf(stderr, "an unknown adapter name was accepted\n");
        bad = 1;
    }

    /* the longest declared match wins, so a more specific family can always
     * be added later without disturbing the broader one */
    if (lmb_family_for("qwen36_moe") != lmb_family_by_id("qwen36")) {
        fprintf(stderr, "longest-match did not prefer the specific family\n");
        bad = 1;
    }

    printf("MODEL FAMILY TABLE: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
