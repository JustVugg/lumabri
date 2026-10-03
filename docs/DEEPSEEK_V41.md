# DeepSeek V4.1: resident household adapter

Lumabri provides a **text-only CPU Edge/Segment adapter** for the pinned Colibri
1.12.1 V4.1 engine. It is registered separately from DeepSeek V4 and participates
in the normal planner, donor approval, resident preparation and hosted chat flow.
Colibri's source checkout is unchanged. The Lumabri-owned wrapper links a
hash-checked build copy of the engine into the household binaries.

## Execution and admission contract

- Only the assigned layers are loaded on each donor. Edge owns embedding/head;
  no donor loads the entire model as a fallback.
- Dense weights, all assigned experts and packed Engram tables are prepared in
  RAM before READY. Inference does not fetch checkpoint or Engram rows from disk.
- The planner inspects tensor headers and the Engram sidecar before loading. It
  verifies shapes, table extents, token-map/hash metadata and required tensors.
  Unsupported layouts and incomplete checkpoints are refused explicitly.
- Memory admission includes resident weights, metadata, temporary work and each
  conversation's state, including foreign compressed-KV owners. Engine/session
  budgets are enforced, including a second session's additional reservation.
- Each conversation has independent attention, Engram history and publication
  state. Immutable weights are shared. Reset/reopen does not reload the weights.
- Prefill and decode use **single-row execution**. Multi-row RUN is refused.
  Cancellation is checked between tokens, not in the middle of a kernel.
- GPU, vision, DSpark, disk streaming and state snapshots are not advertised.
  Recovery uses the existing history replay path, not a fabricated snapshot.
  Loader failures stay in the supervised runtime subprocess.

## Why the boundary carries more than a residual

Each cut needs mHC `pre_mix`, compressed-KV/index updates from foreign owners,
the published index owner, shared top-k indices and candidate blocks. A later
layer's publication can affect an earlier layer on the next token. Consequently
the production chat chain feeds the successfully committed tail state back to
its beginning, separately for every conversation. A new/replayed chain rebuilds
this state. `V41_INDEX_OWNER` overrides are refused rather than changing the
model silently.

`engine_patches/v41_boundary.h` validates bounded single-row deltas before
modifying state. Full KV history remains local. The schema and numeric class
prevent accidental routing through an incompatible V4 or batched adapter.

## Verification

The reduced checkpoint is an upstream-format V4.1 model with real inference,
compressed attention, cross-layer owners and Engram tables. Its size allows the
same checks to run routinely; it is not evidence of full-checkpoint throughput.

| Gate | What it verifies |
| --- | --- |
| `test-v41-range` | Upstream reference comparison; exact logits at all five cuts of six layers for 48 steps; negative controls for missing attention/mHC state |
| `test-v41-transport-existing` | Encrypted separate-process RUN, two conversations, duplicate-request handling and malformed/stale state rejection |
| `test-v41-abi-existing` | Actual Edge/Segment ABI, positive memory budgets, aggregate session admission, reset/reopen and no post-preparation weight reads |
| `test-v41-memory-existing` | Resident memory accounting and malformed checkpoint/sidecar rejection before loading |
| `v41_product_test.py` | Production TCP nodes versus the independent sequential token oracle; real TUI approval and resident generation; a second private conversation with the weight source offline and no reload or weight-mirror writes |

Build and run (after installing the upstream tiny-fixture generator's Python
dependencies):

```sh
make household ENGINE=/path/to/colibri/c
make test-v41-range ENGINE=/path/to/colibri/c
make test-v41-transport-existing test-v41-abi-existing test-v41-memory-existing ENGINE=/path/to/colibri/c
python3 tests/integration/v41_product_test.py
```

CI runs the native gates on Linux and macOS Intel/Apple Silicon, with and
without OpenMP on macOS. Linux additionally uses address/undefined-behaviour
sanitizers with leak detection. A green run validates that exact revision and
test matrix, not every hardware configuration.

**Scale evidence remains separate:** the full V4.1 checkpoint and sufficient
resident RAM/VRAM are not available in the current test household. No full-model
tok/s, large-model LAN measurement, or V4.1 GPU support is claimed. This does not
prevent testing and shipping the working text inference integration.

Source pin and attribution: [THIRD_PARTY.md](THIRD_PARTY.md).
