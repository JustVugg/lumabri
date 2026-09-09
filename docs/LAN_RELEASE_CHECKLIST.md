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
   encoding. OLMoE, DeepSeek V4, converted Qwen3.6, Inkling and float-dense/MXFP4
   Kimi, float-source GLM, text-only float-source GLM5.3 and BF16/float/block-FP8-expert
   Qwen3.8 enable conservative sizing. Require real
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
2. PR #155 merged initial contracts; gate 2 remains open. Qwen3.6 has an explicit CPU resident contract (float32 dense
   and Edge, int8 expert slots even for packed int4 files, grouped scales,
   per-layer attention/DeltaNet state). Real synthetic int8 and grouped-int4
   fixtures exercise approval, two compute donors, generation and cleanup.
   Metadata alone is insufficient: the bounded header inspector requires Edge,
   per-layer dense/attention/DeltaNet tensors, every merged expert and the exact
   row/group scale count. Source payload sizes, not the potentially stale ebits
   metadata flag, distinguish int8 from packed int4. Missing or mismatched
   tensors are refused before any donor allocation.
   Inkling adds header-based retained weights, packed int4/int8 experts, and
   separate full/sliding attention and convolution state; float and int4
   fixtures cover the independent oracle and household path.
   Kimi adds validated MXFP4 banks, fixed KDA and context-sized MLA/DSA state,
   and the full AttnRes boundary width. Prepared U8 dense containers are not
   yet admitted by this contract. Native cache policy may reserve less than
   the upper bound; full warm residency still needs execution evidence.
   Its native 3.7 GB policy floor is included in admission. Standard ARM CI
   validates low-memory refusal, not two-range Kimi execution; sufficient-RAM
   native ARM execution remains open. No overcommit override is enabled.
   GLM float source adds latent MLA/DSA state and separate miss/load workspace;
   prepared quantized GLM containers remain unverified.
   GLM5.3 accounts for its engine-wide replicated session state and context-sized
   workspace. Its stateless Edge context convention is handled by the caller.
   Packed/vision GLM5.3 and the native pool=1 path remain unverified.
   Qwen3.8 adds DeltaNet/QSA state, hyper-connection boundaries, checked I64
   PLE layout metadata and a full source-cache allowance for its row-read PLE
   tables. This budget is not a claim of warm or pinned residency and does
   not enable a smaller disk working set. Its greedy-only Edge is tested
   through the public selection ABI, not an assumed LOGITS capability.
   Segment now reports that capability before READY, and Hosted forwards an
   optional sampling-capability word. The client explicitly displays greedy
   decoding and sends temperature zero; explicit stochastic Segment CLI
   requests still fail on backends without logits. Upgrade host and client
   together: an older client rejects the extended greedy-only greeting.
   Real BF16 fixture: one/two-range independent greedy oracle (8 tokens,
   repeated with fresh sessions), direct TCP split equality, TUI approvals,
   generation and lost-donor cleanup pass locally. Streaming defers incomplete
   decoder prefixes but requires final decoding to succeed; it never rewrites
   already emitted bytes or reports a permanent decoder error as success.
   The historical OLMoE formula is replaced with validated merged-int8 expert
   payloads and row scales, f32-expanded dense/Edge weights and full-MHA state.
   Unconverted HF experts and GQA geometry are not compatible with the pinned
   runtime and are rejected before offers. Header-only negative fixtures are
   explicitly separate from the real one/two-range token oracle.
   DeepSeek V4 now validates native FP8 dense/scale tensors, mHC boundaries,
   packed FP4 expert geometry and the store's same-shard/two-contiguous-bank
   layout. Large token-to-expert I64 router tables are header-inspected, not
   read as metadata. Resident allowances distinguish raw expert scales from
   expanded dense scales, window/compressor/indexer state, snapshot/growth
   allowance and conservative native workspace. No disk or GPU mode is enabled.
   Local synthetic short/compressed/long independent oracles pass with one
   and two ranges and fresh sessions (8/4/4 generated tokens respectively).
   A separate byte-tokenizer copy exercises ordinary text through the TUI;
   the upstream special-token oracle fixture is preserved unchanged. Approvals,
   two actual ranges, generation and lost-donor cleanup pass locally.
   Qwen3.8 block-FP8 experts additionally validate partial 128x128 scale
   geometry and complete sidecars. Conservative admission bounds both native
   FP8 and optional f32 expansion, including shared and per-slot scales.
   A real quantized synthetic fixture is compared with a Transformers oracle
   using the exact reconstructed weights, not the original BF16 model. Both
   native and expanded paths pass one/two-range token tests; direct TCP split
   and TUI approval/generation/lost-donor cleanup pass locally. Timing is
   withheld on the loaded test host. Native macOS checks run in CI.
   Additional encodings, vision and large checkpoints
   remain untested; gate 2 is not complete merely because all families have
   an initial memory contract.
3. PR #153: shared conservative resident admission for catalogue, request
   and launch, with a stat-only checkpoint preview and a signed-inventory
   recheck before offers. Edge and Segment budgets are separately MiB-aligned.
   Follow-up: checked tensor/state arithmetic and aggregate budgets reject
   overflow instead of wrapping into apparently small allocations.
   Measured-cost placement remains pending; this does not enable disk mode or
   certify additional model families.
4. PR #157 merged: versioned measurements collected during ordinary Segment chat,
   separating prefill, decode traversals and first visible text. No extra model
   run is required. PR #159 connects completed household turns to private
   content/runtime-bound records and catalogue lookup. Context, selected
   computers and donor restarts invalidate old speeds; the last observed
   workload is shown separately from the configured context limit. Placement
   cost calibration and catalogue advice remain pending; this is not the
   entire gate 4. See `CALIBRATION_RECORDS.md` for its exact contract.
5. PR #158 merged: reusable adapter-scoped working mirrors and shared verified
   CAS chunks, with separate approval and fresh engine sessions on reuse.
   Cache-directory allocation is exclusive even across different homes.
   Model acquisition, cache-aware disk admission/eviction and verified smaller
   disk working sets remain pending. See `HOUSEHOLD_WEIGHT_CACHE.md`.
6. Pending: upstream-only GPU capability integration, CPU otherwise.
7. Pending: concurrent sessions and actual replay after failure.
8. Pending: native releases, platform validation and dependency-backed cleanup.
