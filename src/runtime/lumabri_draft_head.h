/* A draft head that runs at the Edge.
 *
 * Some checkpoints carry a small head trained to guess the next tokens from
 * the model's own final state (Qwen3.8's MTP head: one more attention layer).
 * The Edge receives every final row, so the head lives there: it reads the
 * rows the chain has committed and proposes the tokens after them. A
 * proposal is only ever a candidate for a provisional row (docs/DRAFTS.md);
 * the head cannot change a reply.
 *
 * The engine's build copy registers one binding when it has loaded a head.
 * State is per conversation: a session owns the head's attention rows.
 */
#ifndef LUMABRI_DRAFT_HEAD_H
#define LUMABRI_DRAFT_HEAD_H

#include <stddef.h>
#include <stdint.h>

/* ColiEdgeEngineOptions.reserved_u64[0]: load the checkpoint's draft head
 * with the Edge, resident. An Edge without one, and every family that has
 * none, ignores it. */
#define LMB_EDGE_OPEN_DRAFT_HEAD UINT64_C(0x4c4d424d54504844) /* "LMBMTPHD" */
#define LMB_DRAFT_HEAD_MAX 3u /* proposals one call makes: the depth the head is trained for */

typedef struct {
    void *owner;
    /* 0 and *session on success. context_tokens bounds every position. */
    int (*session_create)(void *owner, uint32_t context_tokens, void **session);
    void (*session_destroy)(void *owner, void *session);
    /* `rows` final rows at `position` onwards, computed by feeding
     * tokens[0..rows) there, all committed. Position 0 starts the
     * conversation again. A position that neither continues nor restarts
     * leaves the head silent until the next restart; it never guesses. */
    int (*commit)(void *owner, void *session, uint64_t position, uint32_t rows,
                  const int32_t *tokens, const float *streams);
    /* Proposals for the tokens after `token`, which is about to be fed at
     * `position`. 0 when the head has not read up to `position`. */
    uint32_t (*propose)(void *owner, void *session, int32_t token, uint64_t position,
                        int32_t *out, uint32_t want);
} LmbDraftHeadBinding;

int lmb_draft_head_register(const LmbDraftHeadBinding *binding);
void lmb_draft_head_unregister(void *owner);
const LmbDraftHeadBinding *lmb_draft_head(void);

#endif
