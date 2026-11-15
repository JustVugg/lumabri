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
 *   - reserved_fn[LMB_SEGMENT_DRAFT_ROOM_SLOT] says how many rows one
 *     provisional run of a session may carry inside that session's memory
 *     limit, and binds the session to it: a larger provisional run is refused.
 *     Whatever a rollback needs beyond the ordinary session state (Qwen3.8:
 *     one copy of the recurrent state per row but the last) is counted here,
 *     so drafts never use memory the session's reservation did not cover.
 *   - A run whose reserved_u64[0] is LMB_SEGMENT_RUN_PROVISIONAL may be
 *     rewound to any position inside it afterwards. Adapters with state that
 *     cannot be recomputed backwards keep one copy of it per row.
 */
#ifndef LMB_SEGMENT_DRAFT_H
#define LMB_SEGMENT_DRAFT_H

#include <stddef.h>
#include <stdint.h>

#define LMB_SEGMENT_REWIND_SLOT 0
#define LMB_SEGMENT_DRAFT_ROOM_SLOT 1
#define LMB_SEGMENT_DRAFT_MAX_ROWS 16u
#define LMB_SEGMENT_RUN_PROVISIONAL UINT64_C(0x4c4d424452414654) /* "LMBDRAFT" */

typedef int (*LmbSegmentRewindFn)(void *session_impl, uint64_t position,
                                  char *error, size_t error_size);

/* memory_limit_bytes == 0 means the session has no limit. *rows is 0 when
 * not even one draft beside the certain row fits, else 2..MAX_ROWS. */
typedef int (*LmbSegmentDraftRoomFn)(void *session_impl,
                                     uint64_t memory_limit_bytes,
                                     uint32_t *rows);

struct ColiSegmentEngine;
struct ColiSegmentSession;
int lmb_segment_engine_draft_rows(const struct ColiSegmentEngine *engine);
int lmb_segment_session_draft_room(struct ColiSegmentSession *session,
                                   uint64_t memory_limit_bytes,
                                   uint32_t *rows);
int lmb_segment_session_rewind(struct ColiSegmentSession *session,
                               uint64_t position,
                               char *error, size_t error_size);

#endif
