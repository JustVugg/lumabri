/* lumabri_segment.h — model-neutral Segment v2 wire and session contract.
 *
 * This layer knows nothing about KV, MLA, recurrent or convolutional state.
 * Colibri adapters own those bytes. Lumabri owns the session identity,
 * ordering, lease, fencing epoch, route generation and bounded wire shapes.
 */
#ifndef LUMABRI_SEGMENT_H
#define LUMABRI_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

#define LMB_SEG_ID_BYTES             16u
#define LMB_SEG_ROOT_BYTES           32u
#define LMB_SEG_ENGINE_MAX           64u
#define LMB_SEG_SCHEMA_MAX          128u
#define LMB_SEG_NUMERIC_CLASS_MAX    96u
#define LMB_SEG_MAX_ROWS           4096u
#define LMB_SEG_MAX_STATE_WIDTH (1u << 20)
#define LMB_SEG_MAX_CONTEXT     (1u << 24)
#define LMB_SEG_MIN_TTL_MS          1000u
#define LMB_SEG_MAX_TTL_MS       3600000u
#define LMB_SEG_HISTORY_SLOTS        16u

typedef struct { uint8_t bytes[LMB_SEG_ID_BYTES]; } LmbSegId;

typedef enum {
    LMB_SEG_DTYPE_INVALID = 0,
    LMB_SEG_DTYPE_F32 = 1,
    LMB_SEG_DTYPE_F16 = 2,
    LMB_SEG_DTYPE_BF16 = 3,
} LmbSegDType;

enum {
    LMB_SEG_CAP_TOKEN_IDS = UINT64_C(1) << 0,
    LMB_SEG_CAP_SNAPSHOT = UINT64_C(1) << 1,
    LMB_SEG_CAP_RANGE_NATIVE = UINT64_C(1) << 2,
    LMB_SEG_CAP_MULTI_SESSION = UINT64_C(1) << 3,
    LMB_SEG_CAP_CPU = UINT64_C(1) << 8,
    LMB_SEG_CAP_CUDA = UINT64_C(1) << 9,
    LMB_SEG_CAP_HIP = UINT64_C(1) << 10,
    LMB_SEG_CAP_METAL = UINT64_C(1) << 11,
    LMB_SEG_CAP_VULKAN = UINT64_C(1) << 12,
};

#define LMB_SEG_CAP_KNOWN_MASK                                           \
    (LMB_SEG_CAP_TOKEN_IDS | LMB_SEG_CAP_SNAPSHOT |                      \
     LMB_SEG_CAP_RANGE_NATIVE | LMB_SEG_CAP_MULTI_SESSION |              \
     LMB_SEG_CAP_CPU | LMB_SEG_CAP_CUDA | LMB_SEG_CAP_HIP |             \
     LMB_SEG_CAP_METAL | LMB_SEG_CAP_VULKAN)

enum {
    LMB_SEG_XFER_BEGIN = 1u << 0,
    LMB_SEG_XFER_END = 1u << 1,
};

typedef enum {
    LMB_SEG_STATUS_OK = 0,
    LMB_SEG_STATUS_DUPLICATE = 1,
    LMB_SEG_STATUS_BAD_REQUEST = 2,
    LMB_SEG_STATUS_NOT_FOUND = 3,
    LMB_SEG_STATUS_STALE_OWNER = 4,
    LMB_SEG_STATUS_OUT_OF_ORDER = 5,
    LMB_SEG_STATUS_CONFLICT = 6,
    LMB_SEG_STATUS_EXPIRED = 7,
    LMB_SEG_STATUS_BUSY = 8,
    LMB_SEG_STATUS_UNSUPPORTED = 9,
    LMB_SEG_STATUS_QUOTA = 10,
    LMB_SEG_STATUS_NEEDS_RESTORE = 11,
    LMB_SEG_STATUS_INTERNAL = 12,
} LmbSegStatus;

typedef struct {
    LmbSegId lease_id;
    uint64_t fencing_epoch;
    uint64_t route_generation;
} LmbSegOwner;

typedef struct {
    LmbSegId session_id;
    LmbSegId request_id;
    LmbSegOwner owner;
    uint8_t model_root[LMB_SEG_ROOT_BYTES];
    uint8_t tokenizer_root[LMB_SEG_ROOT_BYTES];
    uint32_t layer_begin;       /* inclusive */
    uint32_t layer_end;         /* exclusive */
    uint32_t context_tokens;
    uint32_t max_rows;
    uint32_t state_dtype;
    uint32_t state_width;
    uint32_t ttl_ms;
    uint64_t capabilities;
    char engine_id[LMB_SEG_ENGINE_MAX];
    char state_schema[LMB_SEG_SCHEMA_MAX];
    char numeric_class[LMB_SEG_NUMERIC_CLASS_MAX];
} LmbSegOpen;

typedef struct {
    LmbSegId session_id;
    LmbSegId request_id;
    LmbSegOwner owner;
    uint64_t sequence;
    uint64_t position;
    uint32_t rows;
    uint32_t token_count;
    const int32_t *token_ids;
} LmbSegRun;

/* Snapshot and restore are chunked. snapshot_size is the complete stream;
 * offset/chunk_len describe this frame. sequence and position name the
 * committed state represented by the snapshot, not a new inference run. */
typedef struct {
    LmbSegId session_id;
    LmbSegId request_id;
    LmbSegOwner owner;
    uint64_t sequence;
    uint64_t position;
    uint64_t snapshot_size;
    uint64_t offset;
    uint32_t chunk_len;
    uint32_t flags;
} LmbSegTransfer;

typedef struct {
    LmbSegId session_id;
    LmbSegId request_id;
    LmbSegOwner owner;
    uint64_t sequence;
} LmbSegControl;

typedef struct {
    LmbSegId session_id;
    LmbSegId request_id;
    uint32_t status;
    uint32_t flags;
    uint64_t next_sequence;
    uint64_t next_position;
    uint64_t fencing_epoch;
    uint64_t route_generation;
} LmbSegReply;

typedef struct {
    LmbSegReply reply;
    uint64_t snapshot_size;
    uint64_t offset;
    uint32_t chunk_len;
    uint32_t transfer_flags;
} LmbSegTransferReply;

int lmb_seg_id_is_zero(const LmbSegId *id);
int lmb_seg_id_equal(const LmbSegId *a, const LmbSegId *b);
size_t lmb_seg_dtype_size(uint32_t dtype);
int lmb_seg_open_valid(const LmbSegOpen *open);
int lmb_seg_run_payload_bytes(const LmbSegOpen *open,
                              const LmbSegRun *run, size_t *bytes);

int lmb_seg_open_encode(const LmbSegOpen *open,
                        uint8_t **body, uint32_t *body_len);
int lmb_seg_open_decode(const void *body, size_t body_len, LmbSegOpen *open);
int lmb_seg_run_encode(const LmbSegRun *run,
                       uint8_t **body, uint32_t *body_len);
int lmb_seg_run_decode(const void *body, size_t body_len, LmbSegRun *run,
                       int32_t *token_ids, size_t token_capacity);
int lmb_seg_transfer_encode(const LmbSegTransfer *transfer,
                            uint8_t **body, uint32_t *body_len);
int lmb_seg_transfer_decode(const void *body, size_t body_len,
                            LmbSegTransfer *transfer);
int lmb_seg_control_encode(const LmbSegControl *control,
                           uint8_t **body, uint32_t *body_len);
int lmb_seg_control_decode(const void *body, size_t body_len,
                           LmbSegControl *control);
int lmb_seg_reply_encode(const LmbSegReply *reply,
                         uint8_t **body, uint32_t *body_len);
int lmb_seg_reply_decode(const void *body, size_t body_len,
                         LmbSegReply *reply);
int lmb_seg_transfer_reply_encode(const LmbSegTransferReply *reply,
                                  uint8_t **body, uint32_t *body_len);
int lmb_seg_transfer_reply_decode(const void *body, size_t body_len,
                                  LmbSegTransferReply *reply);

/* Thread-safe metadata table. It serializes each session without serializing
 * unrelated chats. The engine adapter remains responsible for opaque model
 * state and for caching a committed response when DUPLICATE is returned;
 * duplicate requests must never be executed a second time. */
typedef struct LmbSegTable LmbSegTable;

LmbSegTable *lmb_seg_table_create(size_t capacity);
void lmb_seg_table_destroy(LmbSegTable *table);

/* Call table_open only after the local Colibri adapter has matched the
 * requested roots/range/schema/numeric class and actual capabilities. The
 * table enforces protocol invariants; it cannot attest model residency. */
LmbSegStatus lmb_seg_table_open(LmbSegTable *table,
                                const LmbSegOpen *open, uint64_t now_ms);
LmbSegStatus lmb_seg_table_run_begin(LmbSegTable *table,
                                     const LmbSegRun *run,
                                     const void *payload,
                                     size_t payload_bytes, uint64_t now_ms);
LmbSegStatus lmb_seg_table_run_commit(LmbSegTable *table,
                                      const LmbSegRun *run, uint64_t now_ms);
LmbSegStatus lmb_seg_table_run_abort(LmbSegTable *table,
                                     const LmbSegRun *run);
LmbSegStatus lmb_seg_table_snapshot_check(LmbSegTable *table,
                                          const LmbSegTransfer *transfer,
                                          uint64_t now_ms);
/* Commit an already validated and restored complete snapshot. Chunk assembly
 * and adapter streaming land with replay; the ownership/state transition is
 * defined here now so old owners are fenced consistently. */
LmbSegStatus lmb_seg_table_restore_commit(LmbSegTable *table,
                                          const LmbSegTransfer *transfer,
                                          uint64_t now_ms);
/* Multi-frame restore transaction. begin marks the session busy before any
 * adapter state changes; finish either publishes the new sequence/position or
 * releases the reservation. Unlike restore_commit, snapshot_size may exceed
 * the size of one wire frame. */
LmbSegStatus lmb_seg_table_restore_begin(LmbSegTable *table,
                                         const LmbSegTransfer *transfer,
                                         uint64_t now_ms);
LmbSegStatus lmb_seg_table_restore_finish(LmbSegTable *table,
                                          const LmbSegTransfer *transfer,
                                          int commit, uint64_t now_ms);
LmbSegStatus lmb_seg_table_fence(LmbSegTable *table,
                                 const LmbSegId *session_id,
                                 const LmbSegOwner *new_owner,
                                 uint64_t now_ms);
LmbSegStatus lmb_seg_table_close(LmbSegTable *table,
                                 const LmbSegControl *control);
LmbSegStatus lmb_seg_table_state(LmbSegTable *table,
                                 const LmbSegId *session_id,
                                 uint64_t *next_sequence,
                                 uint64_t *next_position,
                                 int *needs_restore);
/* Removes one expired, idle session and writes its id. Call until it returns
 * zero, then let the adapter destroy the matching opaque model state. */
int lmb_seg_table_reap_expired(LmbSegTable *table, uint64_t now_ms,
                               LmbSegId *session_id);

#endif /* LUMABRI_SEGMENT_H */
