# Segment protocol v2

Segment v2 is Lumibri's model-neutral state and ownership protocol for running
a contiguous range of transformer layers on a peer. It does not encode a
particular model's KV layout. Colibri owns the model math, weights, accelerator
and opaque state; Lumibri owns transport, session identity, ordering, leases,
fencing and route lifecycle.

This change defines the wire and session contract only. No production binary
dispatches the new opcodes and no model is advertised as Segment-capable yet.
Public activation is gated on GLM, Inkling, Kimi K3, Qwen3.6, OLMoE and
DeepSeek V4 all passing the same conformance suite.

## Wire operations

The shared frame opcodes are:

```text
SEG_OPEN       SEG_OPEN_R
SEG_RUN        SEG_RUN_R
SEG_SNAPSHOT   SEG_SNAPSHOT_R
SEG_RESTORE    SEG_RESTORE_R
SEG_CLOSE      SEG_CLOSE_R
SEG_HEALTH     SEG_HEALTH_R
```

Every body starts with `SEG2` and protocol version 2. State-mutating requests
carry all of:

- 128-bit session, request and lease identifiers;
- a monotonically increasing fencing epoch;
- a monotonically increasing route generation;
- sequence and position where applicable.

`SEG_OPEN` additionally commits to the model and tokenizer roots, half-open
layer range, engine, state schema, numeric class, dtype, state width, context,
capabilities and TTL. Unknown capability bits, unbounded shapes and malformed
or trailing data are rejected before an engine sees the request.

The executor must compare those requested fields with capabilities returned by
the local Colibri adapter before inserting the session into `LmbSegTable`. The
table validates protocol state; it never treats a client's capability claim as
proof that weights or a backend are resident.

`SEG_RUN` carries token IDs in its bounded body when the state schema requires
them. Boundary activations remain in the frame payload. The exact payload size
is derived with overflow checks from rows, dtype and state width.

Snapshots are chunked. Each chunk names the complete snapshot size, offset,
length and begin/end flags, so the existing per-frame payload limit never
becomes a limit on a complete KV, recurrent or convolutional state.

The body mapping is fixed for v2:

| Operation | Body | Payload |
|---|---|---|
| `SEG_OPEN` | open envelope | none |
| `SEG_RUN` | run envelope and optional token IDs | activation input |
| `SEG_SNAPSHOT` | control envelope at the expected sequence | none |
| `SEG_SNAPSHOT_R` | reply plus transfer metadata | snapshot chunk |
| `SEG_RESTORE` | transfer metadata | snapshot chunk |
| `SEG_CLOSE`, `SEG_HEALTH` | control envelope | none |

Responses echo session and request IDs. A run response carries the resulting
boundary activation only when its status permits commit.

## Session rules

`LmbSegTable` is a thread-safe metadata state machine. It provides:

- isolated sequence and position for every chat;
- a bounded number of sessions;
- collision-resistant hash lookup with a maximum 50% table load;
- one inflight state mutation per session, while different sessions may run
  their engine work concurrently;
- strictly ordered positions and sequences;
- TTL expiry and incremental cleanup;
- cryptographic request digests for duplicate recognition;
- stale-owner rejection after fencing;
- a mandatory restore boundary after ownership changes.

A successful `run_begin` reserves the next transition. The adapter executes
outside the table lock, then calls `run_commit` or `run_abort`. Commit alone
advances sequence and position.

The table records the last 16 committed request identities and digests. A
duplicate is never executed twice. The future network executor must retain the
corresponding response while it is retryable, or return an explicit
duplicate-without-payload failure and trigger replay; it must never call the
model again for the same committed request.

## Fencing

An owner is `(lease_id, fencing_epoch, route_generation)`. A takeover must not
decrease either monotonic counter and must increase at least one. Crossing
updates, where one counter increases while the other decreases, are conflicts.

After a valid fence:

1. the old owner is rejected;
2. the new owner cannot run immediately;
3. a compatible snapshot must be restored;
4. restore commits the new sequence and position;
5. only then may the new owner execute.

The client must also bind one route generation for a request and reject a late
result from an older generation. Executor fencing and client-side commit
fencing are both required during a network partition.

## Conformance coverage

`test_segment_v2.c` runs the same codec against representative tiny schemas
for:

- standard KV;
- MLA/DSA state;
- global/sliding KV plus convolutional state;
- MLA/KDA/AttnRes recurrent state;
- DeltaNet plus convolution ring state;
- mHC/compressed-attention/compressor/indexer state.

It also covers truncation, version skew, overflow and shape bounds, two-chat
isolation, quota, duplicate and conflicting requests, abort, out-of-order
execution, fence, restore, stale owners, snapshot checks, close and TTL reap.
These are protocol fixtures, not claims that the model adapters are complete.
