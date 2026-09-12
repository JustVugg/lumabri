# Concurrent OLMoE Hybrid

The OLMoE glue can issue a remote prefix of the router's selected experts and
compute the remaining suffix locally while those requests are in flight.
The local callback uses the engine's existing row kernel, including its ISA
branches, and borrows the MoE's existing scratch on the engine thread. It does
not launch an extra local thread pool or change routing and normalization.
Colibri itself remains unchanged; hooks are applied to Lumabri's build copy.

All results are accumulated in router order, not arrival order. Until every
result succeeds, the caller's output is unchanged. Callback allocation or
execution failure, malformed replies and exhausted replicas close outstanding
requests and discard temporary outputs. Segment's existing complete local
fallback then recomputes the layer. A failed split is not counted as a
completed concurrent round.

For a controlled standalone Hybrid experiment, set
`LUMABRI_HYBRID_LOCAL_EXPERTS=N` on the Segment process. `N` must be at least
one and smaller than top-k; it is the number computed locally per routing row.
Absent, zero, malformed or out-of-range values retain the all-remote behavior.
The other adapters keep their existing execution paths. This is a split
configuration, not an autotuner or a guarantee that remote work is faster.

The exit report distinguishes remote calls, completed concurrent rounds and
local calls within those rounds. The transport round duration includes local
computation; it must not be interpreted as isolated network wait time.

## Verification

`test_hybrid_parallel` uses socket pairs and condition variables: all remote
requests must arrive before local work starts, and no remote reply is released
until the local callback starts. This proves dispatch/compute overlap without
a timing threshold. It also checks bitwise router-order accumulation, callback
failure, malformed replies, no partial output and balanced in-flight counters.
Linux and both native macOS workspace jobs run this test.

`tests/integration/segment_hybrid_test.sh` compares the same OLMoE Segment
runtime before and after a resident Expert donor joins. It requires identical
greedy token IDs, actual remote calls and actual concurrent local calls. It
then stops the donor and requires the same token IDs through bounded fallback.
`MODEL_DIR` can select an existing compatible test fixture without copying its
weights. This is a kernel/protocol test, not a physical LAN speed benchmark.

## Household boundary — not enabled yet

Household offers currently approve disjoint whole-layer Segment allocations.
Those allocations do **not** authorize the additional overlapping expert
residency needed for this Hybrid experiment. Household launch therefore keeps
`LUMABRI_NO_EXEC=1`; this diagnostic setting cannot bypass donor approval.

Before enabling household Hybrid, the reviewed plan and immutable offers must
describe accelerator roles and expert ownership, charge duplicate weights and
scratch to each computer's budget, pin RPCs to approved participant identities
and checkpoint roots, prepare and seal those weights before READY, and retain
the matching full local fallback. Older packages must reject that allocation
mode rather than silently interpreting it as a disjoint Segment plan.
The two-donor approval/loading/chat test must exercise this new mode before
calling it available from the TUI or measuring PC+Mac speedup.
