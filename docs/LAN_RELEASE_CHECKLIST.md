# Household release acceptance

This is a verification ledger, not a declaration that the full roadmap is
finished. Tests against tiny checkpoints do not certify large models, GPU
execution, cross-platform numerical compatibility, or concurrent capacity.

## Current release work

- [x] Runtime/preflight PR #151: installed engine thread-capacity query; no-OpenMP
  single-thread execution; adaptive RAM reserve; acknowledged actionable
  failure; pre-index donor reachability; build-option invalidation.
- [x] Native Intel/ARM CI with and without OpenMP and isolated two-donor flow (#151).
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
   and placement informed by measured execution costs, not just RAM totals.
4. Persist real chat measurements against exact checkpoint/build/hardware/
   topology/context keys; bounded optional warm calibration, stale-data UI,
   resource-based recommendations. Do not estimate tok/s from Tiny's speed.
5. Authorized model acquisition, resumable transfer and reusable content cache.
   Disk execution requires a verified adapter working-set contract; keep the
   current memory guard until that exists. Checkpoint source selection must
   not require every chatting device to download a complete model.
6. Consume actual Colibri GPU backends with VRAM/scratch/state budgeting and
   execution tests. Capability bits are not implementation. Colibri remains
   read-only: models without a working upstream Edge/Segment GPU backend stay
   on CPU and explicitly say so. No missing GPU kernels are to be invented or
   patched into Colibri. Future compatible backends require Lumabri integration
   and conformance tests, not a promise that every GPU/model already works.
7. Multi-session admission based on memory and measured compute capacity;
   isolated state, fair scheduling, 1/2/4/8-session tests and node-loss replay
   with no duplicate or silently lost user-visible output.
8. Native Windows model runtime and packaging; macOS Intel/ARM and Linux
   release artifacts tested from clean machines. WSL is not native Windows.
   Incremental repository cleanup backed by dependency/build/test evidence:
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

## Delivery tracking

One PR per numbered gate above; all merges preserve commits (no squash), and
require green checks on the exact reviewed head. Partial implementation is
not a completed gate.

1. PR #152 merged: approved layer-allocation view, named execution evidence,
   two-donor checks; physical two-computer oracle and timings still required.
2. Pending: verified family sizing and checkpoint conformance.
3. PR #153: shared conservative resident admission for catalogue, request
   and launch, with a stat-only checkpoint preview and a signed-inventory
   recheck before offers. Edge and Segment budgets are separately MiB-aligned.
   Follow-up: checked tensor/state arithmetic and aggregate budgets reject
   overflow instead of wrapping into apparently small allocations.
   Measured-cost placement remains pending; this does not enable disk mode or
   certify additional model families.
4. Pending: connected, persisted lightweight calibration and advice.
5. Pending: model acquisition, cache reuse and verified disk execution.
6. Pending: upstream-only GPU capability integration, CPU otherwise.
7. Pending: concurrent sessions and actual replay after failure.
8. Pending: native releases, platform validation and dependency-backed cleanup.
