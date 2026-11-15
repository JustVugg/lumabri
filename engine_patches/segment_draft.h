/* Provisional rows for Lumabri's build copy of the Segment adapters.
 *
 * Upstream's ABI runs rows at contiguous positions and has no way to take
 * rows back. Drafts need exactly that: several candidate rows travel the
 * chain in one round, and the rows whose candidate was wrong are discarded
 * (docs/DRAFTS.md). tools/prepare_draft_rows.py adds the two pieces below to
 * the disposable engine copy; the upstream checkout is never modified.
 *
 *   - An adapter that can discard rows stores its rewind function in
 *     reserved_fn[LMB_SEGMENT_REWIND_SLOT]. Rewinding to the current position
 *     is a no-op; a failed rewind leaves the session untouched.
 *   - A run whose reserved_u64[0] is LMB_SEGMENT_RUN_PROVISIONAL may be
 *     rewound to any position inside it afterwards. Adapters with state that
 *     cannot be recomputed backwards keep one copy of it per row.
 */
#ifndef LMB_SEGMENT_DRAFT_H
#define LMB_SEGMENT_DRAFT_H

#include <stddef.h>
#include <stdint.h>

#define LMB_SEGMENT_REWIND_SLOT 0
#define LMB_SEGMENT_RUN_PROVISIONAL UINT64_C(0x4c4d424452414654) /* "LMBDRAFT" */

typedef int (*LmbSegmentRewindFn)(void *session_impl, uint64_t position,
                                  char *error, size_t error_size);

struct ColiSegmentEngine;
struct ColiSegmentSession;
int lmb_segment_engine_draft_rows(const struct ColiSegmentEngine *engine);
int lmb_segment_session_rewind(struct ColiSegmentSession *session,
                               uint64_t position,
                               char *error, size_t error_size);

#endif
