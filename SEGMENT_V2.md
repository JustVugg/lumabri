# Segment protocol v2

Segment v2 is Lumabri's model-neutral state and ownership protocol for running
a contiguous range of transformer layers on a peer. It does not encode a
particular model's KV layout. Colibri owns the model math, weights, accelerator
and opaque state; Lumabri owns transport, session identity, ordering, leases,
fencing and route lifecycle.

The wire/session contract, tracker discovery, direct/relay executor and native
interactive path are implemented. `segment_node` dispatches `SEG_OPEN`,
`SEG_RUN`, `SEG_SNAPSHOT`, `SEG_RESTORE`, `SEG_CLOSE` and `SEG_HEALTH` through
Colibri's public ABI; `segment_chat` keeps Edge math local and binds a complete
route before inference. Snapshot bytes are chunked without a whole-checkpoint
frame limit, and persistent chats restore a compatible chain plus replay token
deltas after a selected peer fails. See
[SEGMENT_DIRECT.md](SEGMENT_DIRECT.md).

## Tracker discovery

A signed `SEG_REGISTER` heartbeat advertises a half-open layer range together
with model/tokenizer roots, engine, state schema, numeric class, dtype/width,
backend capability, resident RAM/VRAM, session capacity, queue, inflight and
draining state. Expert and Segment advertisements may share one compute name,
key and control connection, but retain independent addresses and liveness.

`SEG_ROUTES` asks for one exact model/schema/numeric class and a requested
layer interval. The tracker returns every compatible live range and replica,
plus:

- a random lease ID and monotonic fencing epoch per executor;
- a monotonic route generation for the complete placement;
- data-plane transport capability (`DIRECT`, `RELAY`, or both). Relay is
  published only for a live signed SREG control tunnel; relay-only peers never
  leak their loopback listener as a direct endpoint;
- whether the returned ranges contain an exact executable chain. Interval
  union alone is insufficient because overlapping executors would run a layer
  twice; replicas may overlap, but the selected boundaries must join exactly.

Telemetry-only heartbeats do not churn leases or route generations. A range,
compatibility, address, capability or draining change does; stale/recovered
registrations do as well. Draining peers remain known but are excluded from
new placements. The tracker reserves a durable generation epoch beside its
peer-binding database, so a restart cannot make a cached generation current
again or move either monotonic fencing value backwards.

`LmbSegDiscovery` polls this control plane on a dedicated thread and publishes
a fixed-size immutable `LmbSegRouteSnapshot`. The inference thread copies that
snapshot; it never asks the tracker. A session binds the selected generation
at `SEG_OPEN`, so later discovery does not silently move a running chat.

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

The executor keeps one permanent mutex per session slot. A multi-gigabyte
adapter restore holds only that session mutex: lookup releases the node-wide
session-table mutex before waiting, and `CLOSE` reports `BUSY` for the one slot
instead of blocking unrelated `OPEN`, `CLOSE`, snapshot or run traffic. The
same lock prevents a close from destroying adapter state during a run.

The table records the last 16 committed request identities and digests. A
duplicate is never executed twice. The executor retains the most recent
committed activation response for a safe immediate retry. An older recognized
duplicate returns `NEEDS_RESTORE`; the gateway restores the common checkpoint
and replays the missing token suffix rather than ever calling the model twice
on ambiguous state.

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

`test_segment_v2.c` and `test_segment_discovery.c` run the same contracts
against representative tiny schemas
for:

- standard KV;
- MLA/DSA state;
- global/sliding KV plus convolutional state;
- MLA/KDA/AttnRes recurrent state;
- DeltaNet plus convolution ring state;
- mHC/compressed-attention/compressor/indexer state.

They also cover truncation, version skew, overflow and shape bounds, two-chat
isolation, quota, duplicate and conflicting requests, abort, out-of-order
execution, fence, restore, stale owners, snapshot checks, close and TTL reap.
The real-tracker discovery gate additionally covers signed registration,
compatibility filtering, replicas, complete chains, telemetry stability,
draining, lease rotation and asynchronous snapshots. These are protocol and
control-plane fixtures. `segment_direct_test.sh` adds the real data plane: two
peers per family, real prefill/decode, persistent sampled sessions and three
greedy oracle tokens for all six models, plus overlapping real sessions. The
relay gate forces every stateful operation through tracker tunnels; the
failover gate kills a selected peer between turns and requires checkpoint
restore plus replay before generation can continue.
