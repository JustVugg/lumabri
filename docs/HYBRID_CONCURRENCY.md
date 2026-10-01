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

## Approved household Hybrid

For OLMoE with top-k >= 2 and at least two selected computers, the planner
offers Hybrid when the coordinator can hold the **complete resident model**,
Edge, session state and extra execution buffers. Other selected computers
keep their assigned ranges as resident expert accelerators. Duplicate weights
and extra scratch/transport memory are charged to the reviewed reservations.
If this does not fit, ordinary disjoint Segment remains the plan: no borrowing
of unapproved RAM and no weight streaming from disk.

The TUI labels the full coordinator and expert accelerators before consent.
Every participating owner approves an immutable offer. Version-2 coordinator
offers bind allocation ID, checkpoint root, participant public keys and ranges.
Accelerators load and seal their weights first. Their actual endpoints are then
checked against that map before starting the coordinator. Every connection pins
the recipient key; the readiness probe also checks allocation, checkpoint and
numeric class. Public expert discovery, relay, hedges and implicit peer adoption
remain disabled. Older packages fail capability preflight before preparation.

For each covered layer, one selected expert is sent remotely while the other
top-k minus one execute locally, with unchanged router-order accumulation.
Uncovered layers stay local. Bounded RPC failures can recompute from the full
resident local copy. This is not a speedup guarantee: latency or a slow donor
can still dominate. `LUMABRI_HOME_HYBRID=0` selects normal Segment for A/B tests.
Other families keep their existing Segment implementation.

Closing the chat retains approved RAM for another conversation. Owner Stop
unloads it. The private restart hint preserves the Hybrid label, but grants no
new authority: reconnection still checks host identity and checkpoint. Owner
revocation or household donor loss retains the existing stop/cleanup policy;
transparent household failover is not promised.

Installed-package CI uses `home_flow_test.py --resident-default --expect-hybrid
--expect-metrics`: actual remote calls, concurrent coordinator rounds, reviewed
ranges, second conversation with the weight source offline, no reload and no
weight mirror writes. `test_home_expert` exercises the production RPC handler's
scope/shape/numeric checks before the kernel; offer and memory unit tests cover
immutable permissions and duplicate-weight charging.

PC + Intel Mac speed is a separate physical-LAN measurement. Compare identical
model, prompt, context, output length and thread budgets. Report observed
per-chat tok/s, not the sum of the participating machines' standalone speeds.
