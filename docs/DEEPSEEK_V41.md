# DeepSeek V4.1: implementation gate

Status: **boundary laboratory, not a supported household adapter**. The catalogue
does not claim V4.1 can start. Colibri 1.12.1 has a standalone V4.1 engine, but
no Edge/Segment adapter for it. It must not be dispatched to the V4 adapter.

The owner does not currently have the full V4.1 checkpoint. No large download or
large-model/LAN/GPU certification is implied by these tests.

## What is proved

`make test-v41-range ENGINE=/path/to/colibri/c` builds a small upstream-format
fixture, tests the unmodified C engine against the upstream PyTorch reference,
then compares local and split layer execution. All five cuts in its six layers
are exercised for 48 steps, both with a one-token and an eight-token initial
prefill. The comparison is exact **logits**, not merely greedy token equality.

The disposable build copy changes loading and the range boundary, not layer
arithmetic. It loads only each range's dense/expert/Engram weights, never Edge
weights or another range's weights. The laboratory fully prepares expert caches
and packed Engram tables before generation. A read guard rejects subsequent
weight `pread` calls; expert misses must remain unchanged. No disk-streaming
fallback is involved. The reference and split executions use the same scheduling
policy, threads and pinned source.

Negative controls remove cross-layer attention state or mHC `pre_mix` and must
diverge. This prevents an accidentally insensitive fixture from claiming a
boundary is correct. Malformed/truncated delta state must be rejected before
live state changes. The range gate now destroys all its models and checks
ASan/UBSan **with leak detection enabled**. The lifetime helper shares immutable
weights, but gives each conversation its own rings, compressed KV, Engram row
cache/history, publication state and expert-cache metadata. An interleaved
two-conversation test, reset and destroy/recreate reproduce exact logits without
weight reads. This is a tested lifetime primitive, not yet the public ABI wrapper
or proof that every allocation-failure path is handled.

`make test-v41-transport-existing` then executes two ranges in **separate
processes**, over authenticated encrypted local socket pairs with pinned test
identities and the existing Segment RUN codec/session table. Two interleaved
conversations each reproduce 48 exact logits blocks. Stale/future owners,
truncated and malformed delta frames are refused; a duplicate request returns
its cached response without a second kernel invocation. Tail feedback is
separate per conversation. These are local sockets, not physical LAN evidence,
and the harness is not the product `segment_node` or approval flow.

`make test-v41-abi-existing` registers an experimental wrapper **only inside
the test executable**, through the actual Colibri Edge/Segment ABI. Tokenization,
embedding, logits/greedy selection, two isolated sessions, cancellation before a
token, malformed/out-of-order input and close/reopen run at all five layer cuts.
The wrapper loads only Edge weights or the assigned range, not a whole model
per donor. It exposes single-row text CPU execution and no snapshot capability.
Positive memory budgets are explicitly refused, never silently ignored: the
bounded checkpoint inspector and admission contract are not implemented yet.
The underlying CLI loader's fatal-error paths must also be contained before it
can become a product adapter. Neither this wrapper nor its experimental
registration is linked into the normal Lumabri binaries.

## Required state

The residual streams alone are insufficient. Each cut needs mHC `pre_mix`,
compressed KV and index-key updates from foreign owner layers, the currently
published index owner, shared top-k indices and the candidate mask. A later
layer's publication can be read by an earlier layer on the next token: the
end of the chain must feed state back to its beginning. Changing this rule
with `V41_INDEX_OWNER` changes the model and is not an acceptable shortcut.

`engine_patches/v41_boundary.h` encodes bounded single-row deltas: one newly
completed compressed row per owner, a bounded list of selected candidate blocks
and shared top-k indices. Full KV history stays local to each session. The test
proves this codec at every cut for one-token steps, including a later owner's
feedback to the next token. It does **not** claim a batched delta protocol.
The full-state copy in the reference harness is an oracle only, never a network
implementation to ship. Sequential versus batched prefill must remain an
explicit, matched numerical policy when comparing results.

## Still required before product registration

1. Turn the tested Edge/Segment wrapper into a bounded, failure-safe checkpoint
   loader; enforce aggregate engine/session memory and validated snapshots or
   replay. Token-boundary cancellation is tested, not mid-kernel cancellation.
2. Authenticated, allocation-scoped transport for the delta state and feedback,
   including retries and failover; no cross-session or stale-frame reuse.
3. Exact planner tensor/sidecar inspection and memory accounting, including
   resident Engram tables, foreign KV state, temporary work and wire buffers.
4. Registry, package, TUI and native-platform conformance after the above gates.
5. A separately labelled full-checkpoint test when suitable hardware and the
   checkpoint are available. A tiny fixture is not that test.

Source pin and attribution: see [THIRD_PARTY.md](THIRD_PARTY.md). The original
Colibri checkout is read-only; generated engine sources stay under `build/`.
