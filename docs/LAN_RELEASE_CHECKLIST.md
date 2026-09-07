# Household release acceptance

This is a verification ledger, not a declaration that the full roadmap is
finished. Tests against tiny checkpoints do not certify large models, GPU
execution, cross-platform numerical compatibility, or concurrent capacity.

## Current release work

- [ ] Runtime/preflight PR: installed engine thread-capacity query; no-OpenMP
  single-thread execution; adaptive RAM reserve; acknowledged actionable
  failure; pre-index donor reachability; build-option invalidation.
- [ ] Native Intel/ARM CI with and without OpenMP and isolated two-donor flow.
- [ ] Rerun the physical Windows/WSL + macOS 12.6 trial on the final build.

## Remaining roadmap gates

1. Actual two-compute-node physical LAN chain: local oracle, split oracle,
   per-node layer ranges and memory, fixed-thread and full-hardware timings.
   Current physical trial has PC source and Mac computation, not two compute
   nodes. Mac deployment/approval is user-operated; CI loopback is separate.
2. Complete verified sizing for every registered family and supported weight
   encoding. Currently only OLMoE and DeepSeek V4 enable sizing. Require real
   checkpoint/adapter conformance before promoting support.
3. Consistent planner/runtime reservations, explicit local-compute consent,
   reusable content cache, authorized model acquisition and resumable transfer.
   Disk execution requires a verified adapter working-set contract; keep the
   current memory guard until that exists.
4. Persist real chat measurements against exact checkpoint/build/hardware/
   topology/context keys; bounded optional warm calibration, stale-data UI,
   resource-based recommendations. Do not estimate tok/s from Tiny's speed.
5. Real GPU backend execution and VRAM/scratch/state budgeting. Capability bits
   are not implementation. The pinned OLMoE Edge/Segment adapter rejects
   non-CPU backends; implementing one is upstream runtime work, not a planner
   toggle. Colibri remains read-only under the agreed boundary.
6. Multi-session admission based on memory and measured compute capacity;
   isolated state, fair scheduling, 1/2/4/8-session tests and node-loss replay
   with no duplicate or silently lost user-visible output.
7. Native Windows model runtime and packaging; macOS Intel/ARM and Linux
   release artifacts tested from clean machines. WSL is not native Windows.
8. Incremental repository cleanup backed by dependency/build/test evidence;
   remove only genuinely unused paths, preserve model adapters and regression
   tests. Keep public-network trust/credits separate from the LAN product.

## Evidence already obtained

- PR #150: native Intel/ARM macOS CI, Linux regression gates, Windows firewall
  helper contracts; Tiny signed CAS, all-party approval, generation and cleanup.
- User household trial: macOS 12.6 Intel Mac with 8 GiB RAM, Windows/WSL source;
  two completed synthetic Tiny responses. Reported ~52 tok/s is Tiny-only.
- Physical trial exposed missing no-OpenMP coverage, a fixed 4 GiB reserve,
  inbound application firewall permissions and insufficient failure details.

No purchase, remote Mac shell access, model-license acceptance or public
service deployment is inferred from implementation authority. Native hardware
and compatible checkpoints are required to close their respective gates.
