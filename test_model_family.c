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
    want("olmoe", "olmoe");
    want("deepseek_v4", "deepseek_v4");
    want("kimi_k3", "kimi");
    want("kimi_linear", "kimi");
    want("inkling_mm_model", "inkling");
    want("inkling", "inkling");
    want("qwen3_5_moe", "qwen36");
    want("qwen3_5_moe_text", "qwen36");
    want("qwen4_exp", "qwen38");
    want("qwen4_exp_text", "qwen38");
    want("glm_moe_dsa", "glm");
    want("glm5_moe", "glm");
    want("glm", "glm");
    want("glm5_next", "glm53");
    want("glm5_next_text", "glm53");

    /* the refusals. Nothing here shares enough with a declared prefix to be
     * claimed, and every one of them used to be swallowed silently. */
    want("", NULL);
    want("llama", NULL);
    want("mixtral", NULL);
    want("deepseek_v3", NULL);   /* "deepseek" was a prefix once: not any more */
    want("kimi_future", NULL);
    want("glm53_moe", NULL);
    want("qwen3_moe", NULL);
    want("qwen36_moe", NULL);

    const LmbModelFamily *deepseek = lmb_family_by_id("deepseek_v4");
    if (!deepseek || strcmp(deepseek->engine, "deepseek_v4") ||
        !deepseek->p2p_engine || strcmp(deepseek->p2p_engine, "deepseek")) {
        fprintf(stderr, "DeepSeek artifact/P2P names drifted apart\n");
        bad = 1;
    }
    const LmbModelFamily *glm53 = lmb_family_by_id("glm53");
    const LmbModelFamily *qwen38 = lmb_family_by_id("qwen38");
    if (!glm53 || glm53->p2p_engine || glm53->expert_node ||
        !qwen38 || qwen38->p2p_engine || qwen38->expert_node) {
        fprintf(stderr, "an adapter without an Expert path advertised one\n");
        bad = 1;
    }

    /* a name Colibri does not register is refused, not ignored */
    if (lmb_family_override("qwen39")) {
        fprintf(stderr, "an unknown adapter name was accepted\n");
        bad = 1;
    }

    printf("MODEL FAMILY TABLE: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
