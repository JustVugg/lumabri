# Household release acceptance

This is a verification ledger, not a declaration that the full roadmap is
finished. Tests against tiny checkpoints do not certify large models, GPU
execution, cross-platform numerical compatibility, or concurrent capacity.

## Current release work

The managed-inference roadmap supersedes the older single-chat LAN checklist
below. Historical observations remain here as evidence, not current promises.

### Roadmap status, October 17, 2026

This table separates implementation from release acceptance. The detailed PR
entries below record the exact evidence; an open PR is not an integrated release.

| Step | Delivered or implemented | Remaining acceptance boundary |
| --- | --- | --- |
| 2 — Inventory and measurements | Runtime/backend checks; memory, workload, link, preparation and model observations; declared machine costs | Missing facts stay unknown. Pinned distributed adapters expose CPU execution, not an enabled household GPU path. |
| 3 — Several resident models | Independent reservations, private state, selective unload and effective owned CPU locks | Final-package memory-pressure and sustained workload checks on the deployment machines. |
| 4 — Controlled concurrency | Bounded compute/session admission, isolated users, cancellation, timing and 1/2/4/8-slot gates | Per-deployment latency/capacity envelope; configured slots are not a single-user speed guarantee. |
| 5 — Joint placement and cost | Bounded joint memory/cost search, measured portfolio selection/application, static-versus-managed comparison (#224) and shared capacity API (#225) | Browser capacity controls #226 are under review. New placements still require preparation and measurement; declared cost is not billed savings. |
| 6 — Shared service and interfaces | Permanent keepers, common preparation/retirement operations, TUI, authenticated API, web chat and private history | The HTTP service is loopback-only; remote/datacenter ingress is not certified. |
| 7 — Recovery | Approved-replica replay; unavailable-capacity replacement, same-turn wait and durable exit/release proofs implemented in #227–230 | Final-head native CI and ordered merges for those PRs; final-package fault tests. No instantaneous KV migration. |
| 8 — Authorized elasticity | Opt-in replica growth, drain, retirement, scale-to-zero/wake, budgets and cooldown | Sustained burst/idle validation on actual machines; failed ownership remains charged without proof. |
| 9 — Provider and actual savings | No paid resources created | Explicitly deferred provider choice, account access and spending authority; implementation and billing comparison remain. |
| 10 — Release | Native CI, allowlisted package assembly and hash verification | Final installed-package LAN/datacenter, sustained-load, update and rollback evidence. Native Windows inference and notarized macOS distribution are not delivered. |

The model source stays local to the requesting installation. Neither this table
nor tiny-checkpoint CI claims that arbitrary models are downloaded automatically,
that every added machine increases single-chat speed, or that the full roadmap
is complete. See [deployment acceptance](../PRODUCTION.md#deployment-acceptance).

- [ ] Sustained managed workload (release candidate, not yet merged): on October
  17, the complete local gate passed a 1,800.57-second timed workload after
  preparation and warm-up. Two independently resident identities derived from
  the same reduced OLMoE weights served 9,908 complete requests, explicitly
  refused 4,248 overload requests and handled 1,416 client disconnects after
  visible text; there were no unexpected failed turns. All 15,572 outcomes
  remain in the report. Sixty-one quiescent resource samples showed unchanged
  allocation reservations and process identities; aggregate RSS moved from
  88,522,752 to 90,681,344 bytes, with a maximum of 91,009,024 bytes. Four engines
  reported effective owned-payload locks. Runtime source was `f46f9c9`, with
  actual binary hashes retained in `managed-soak.json`; the workload script was
  `7e957f5`. CI runs only a ten-second smoke, not this 30-minute acceptance run.
  These are Linux loopback observations, not full-size model capacity, physical
  topology, a no-leak proof or a production SLO.

- [ ] Verified failed-allocation reclamation (#230, under review): immutable
  birth records and inherited kernel lifetime locks distinguish a terminated
  allocation from an unreachable process. Authenticated proof survives keeper
  restart and slot reuse without signalling journal PIDs or replaying approval.
  Retirement v2 separates these proofs from live idle seals; all participants
  must confirm before accounting releases the exact old charge. Unit tests
  cover fork/exec descendants, reaped parents, unsafe/missing records, owner
  authentication, held reservations, v1 migration and arbitrary replica-index
  withdrawal. The October 17 local reduced-OLMoE gate passed replacement after
  a real Segment crash, reclamation of the exact old allocation, continued
  inference on the replacement and an independent model, and refusal to free
  either of two frozen live hosts or exceed the replica limit. Fourteen
  prepared engines reported effective native resident locks with no fallback.
  ASan/UBSan/leak checks pass for lifetime and encrypted proof handling.
  A second complete local gate also passed same-turn replay after a real
  visible prefix, with byte-identical oracle output, no duplicated text and
  the same reclamation/ceiling checks. The test-only codec proxy preserves the
  lifetime descriptor in its real Edge child, matching production inheritance.
  Native CI and merge remain pending; this entry is not release certification.

- [ ] Durable donor retirement receipts (#229, under review): an exact sealed
  allocation's release acknowledgement survives slot reuse and keeper restart.
  A persisted intent alone is not proof; only the original authenticated
  requester can replay a completed receipt with the same process seal. Retired
  IDs cannot load new work. The bounded private store refuses unsafe entries,
  identity replacement, sync failure and automatic evidence eviction. Unit,
  encrypted control and focused ASan/UBSan/leak checks pass. A local real
  reduced-OLMoE flow retires one allocation, reloads an approved new one on the
  donor, then queries the old release directly without disrupting the new
  inference. Native CI and merge are pending. This does not yet reclaim an
  arbitrary failed allocation whose owned-process exit was never proven.

- [ ] Same-turn elastic recovery (#228, under review): optional bounded API wait
  captures an already enabled policy's scope/revision and keeps the original
  model/numeric contract. The web exposes the wait and Stop; policy/credential
  revocation, deadline and disconnect prevent subsequent disclosure. Local
  reduced-OLMoE tests killed an owned Segment after a real visible prefix,
  prepared a new approved replica and completed the same HTTP turn with
  byte-identical oracle output and no duplicate text. Failed ownership remains
  charged; this is not automatic reclamation or KV migration. API/browser,
  policy unit and ASan/UBSan/leak checks pass. The final local flow also passes
  on `99490c9`, after the shared admission/HTTP fixes, with twelve effectively
  locked engines. Native CI and merge of that final head are pending.

- [ ] Unavailable replica replenishment (#227, under review): bounded authenticated
  host/Segment probes distinguish accepting, draining, unreachable and unknown.
  Three separated failures can restore an opt-in policy's available minimum;
  failed allocations remain charged to both replica and memory limits. Unknown
  or stale health prevents retiring the healthy replacement. Policy state
  migrates from v1 without losing reservations and resets failure evidence on
  route/observer changes. Unit, API/browser regression and focused ASan/UBSan
  with leak detection pass. The first local real reduced-OLMoE gate killed an
  owned Segment process, prepared one authorized replacement and resumed
  inference while the second model remained usable, with twelve effectively
  locked engines across the complete lifecycle. The expanded final gate passed
  through the actual web interface as well, then killed the replacement host:
  the hard replica ceiling refused a third allocation, retained every budget
  charge and kept the independent model serving. Native CI and merge remain
  pending. Same-turn opt-in replay and authoritative failed-allocation
  reclamation are the separate candidates above, not claims of this PR alone.

- [x] Shared capacity management API (#225): authenticated historical
  listing, explicit inference measurement with progress/cancellation, live
  checking, whole-portfolio comparison and observation removal. The CLI and
  HTTP consume the same C operations; no command execution or stdout parsing.
  The October 16 local reduced-OLMoE placement gate passes streamed results,
  concurrent-probe refusal, disconnect cleanup, unchanged resident budgets,
  CLI/HTTP selection agreement and separation from production-demand counters.
  Strict API authorization/schema tests and all API unit tests pass. The
  four-slot managed-replica/recovery/portfolio gate also passes with nine
  prepared engines under effective owned-payload locks. All eight checks passed
  on `194e608` before normal merge `2a58669`. This is not whole-release or
  production-capacity certification.

- [ ] Browser capacity controls (#226, under review): explicit ordered allocations,
  workload limits and confirmation; streamed progress and cancellation;
  historical listings, live checks and joint-plan comparison; separate atomic
  model-route application. The browser binds application to exact observation
  content as well as model revisions, so a removed/recreated record name cannot
  silently substitute another plan. Read-only operator, permission revocation,
  malformed stream, stale polling, navigation and mobile regression tests pass.
  The October 16 connected browser gate ran real two-model reduced-OLMoE
  measurement, comparison, route application and private chat through both
  models, with eight effective owned-payload locks. An incorrect observation
  identity was refused without changing either route. The complete placement
  gate then passed policy-driven selection, explicit retirement and evidence
  invalidation. These are loopback results, not datacenter throughput or bills.
  Linux CI now runs that browser path; native CI and merge remain pending.

- [x] Persistent manager and allocation keepers (#183): TUI detach and manager
  restart do not replay approvals or reload live resident allocations.
- [x] Colibri 1.12.1 integration (#184), resource/link/preparation observations
  (#185), independent resident model allocations (#186), owned resident CPU
  text adapter for DeepSeek V4.1 (#187). V4.1 reduced-checkpoint inference is
  real inference; a full-size V4.1 deployment is not claimed without its data
  and hardware.
- [x] OS memory evidence and pressure checks (#188): pageout/compression is
  charged and can revoke readiness. Optional locking fails closed. Sampled
  zero swap is not a guarantee of pinned residency on every platform.
- [x] Bounded shared-model conversations (#189): implementation includes explicit
  1–8-slot approval and memory, isolated conversation/sampler state, whole-turn
  FIFO, bounded cancellation/reset and BUSY. Local OLMoE reduced-checkpoint
  gates pass at 1/2/4/8 slots; the complete 7.4-GB checkpoint passes with two
  conversations in the installed Linux test package. Those were correctness
  runs under other test load, not a throughput guarantee. Native Linux and
  macOS Intel/ARM CI passed before normal merge.
- [x] Same-user machine-wide bounded compute admission (#190), workload/cost
  inventory and read-only joint preview (#191), joint all-party preparation
  (#192), workload-bound measurements and acknowledged session retirement
  (#193), shared strict reply decoder (#194), authenticated resident API (#195).
  A cost-aware preview is not yet an automatic global placement/cost controller.
- [x] Permanent API keeper (#196): authenticated same-user control, independent
  TUI/manager lifetime, isolated stop and crash/occupied-port handling.
- [x] Bundled web/private history (#197): real OLMoE, separate users,
  TUI coexistence and saved replies; native CI passed before normal merge.
- [x] Managed model identities over independently approved resident replicas
  (#198), with exact-allocation shared admission across aliases (#199):
  bounded operator registry, immutable checkpoint/numeric semantics and
  pre-submission availability routing. This is not automatic global placement,
  mid-response recovery or elasticity. Real OLMoE replica gates and native CI
  passed before normal merge.
- [x] Session observations (#200): completed API turns update the same
  provenance-bound evidence as the TUI; all eight checks passed before merge.
- [x] Read-only operator workspace (#201) and measured/declared-cost replica
  preferences (#202): all eight checks passed on each final head before their
  ordered normal merges. These are not global optimization or proven savings.
- [x] Bounded managed API replay (#203): all eight checks passed on the final
  head before normal merge. Replay validates the visible prefix on an already
  approved replica; it does not restart keepers or prepare capacity.
- [x] Authenticated host drain/resume (#204): all eight checks passed on the
  final head before normal merge. One-slot and multi-slot hosts finish
  admitted work and refuse new turns. This retains weights and is not
  node-level drain, automatic eviction or server release.
- [x] Owner-authenticated Segment control (#205): live KV/admitted Hybrid counts and
  fenced drain/resume through the allocation keeper's inherited child channel.
  Local one/four-slot OLMoE, wrong-owner/root/allocation checks, protocol and
  admission tests pass. All eight native CI checks passed before normal merge; this does
  not implement coordinated retirement, RAM eviction or server release.
- [x] Coordinated allocation retirement (#206): irreversible idle fences, retained
  conversation cleanup, exact-owner keeper release and a durable operation
  journal. Local four-slot OLMoE reduced-checkpoint flow passes with two models
  on the same donors, selective retirement and reconciliation after lost
  release acknowledgements. Focused tests refuse retirement with retained KV,
  admitted Hybrid work or a missing codec reset acknowledgement. Full regression,
  warning-as-error rebuild and focused ASan/UBSan/leak checks pass locally.
  Native Linux CI exposed a completed-turn/host-reset admission race. A delayed
  codec-reset regression and the local one-slot OLMoE two-replica flow pass
  with graceful remote close before recycling the API permit. The Darwin
  half-close path preserves that cleanup barrier. All eight checks passed on
  final head `1e1a61e` before normal merge `941a2d0`. Automatic scaling policy
  and cloud/server release are separate work.
- [x] Shared management (#207): explicit digest-bound management grants, fenced
  allocation inspect/drain/resume/retire via HTTP and browser, and the same
  typed retirement coordinator in the resident-library TUI. Ordinary inference
  and read-only operator permissions do not authorize mutation. This does not
  yet unify every preparation/configuration action through HTTP. Full local
  regression, warning-as-error build, focused sanitizers and the real OLMoE
  two-model/four-slot API flow passed. Native CI exposed a stale library-test
  expectation for forced unload. The revised real OLMoE library gate passes
  locally with one, two and eight slots, including inert cancelled confirmation
  and CLI reconciliation of the TUI's shared journal. All eight checks passed
  on final head `6c8df57` before normal merge `c7c468c`.
- [ ] Complete managed-release acceptance, distinct from the merged component
  gates below: deployment-specific capacity, sustained workload, physical
  topology, installed-package lifecycle and actual cost evidence. Provider
  credentials and spending authority remain explicitly deferred by the operator.
- [x] Gateway per-response latency (#208): time to first visible text, generation
  and cleanup-inclusive completion, and bounded exact token-notification gap
  p50/p95. Replay/missing telemetry do not fabricate token timing. Local
  two-model OLMoE reduced-checkpoint flow passes solo baselines and mixed
  2/4/8-client bursts, preserving each rejection in the artifact. Unit and
  focused sanitizer tests pass. Terminal engine errors also await actual host
  reset before recycling an API permit; a delayed-reset regression and real
  one-slot recovery gate pass. All eight checks passed on final head `e6953b2`
  before normal merge `34ac5ed`. The first macOS ARM no-OpenMP job correctly
  refused OS-compressed model pages; the repeated native gate passed without
  weakening residency enforcement. This is not a certified production capacity figure.
- [x] Persisted joint capacity observations and bounded selection (#209): explicit
  local-owner C probe uses real gateway admission/cleanup on approved models;
  records all outcomes, per-model latency tails and exact runtime/workload
  identity. Local two-model OLMoE gate passes two-client success, eight-client
  overload and comparison with missing costs; unit and ASan/UBSan/leak gates
  pass. Recommendations are limited to comparable measured portfolios and do
  not apply placements or promise SLOs. All eight native checks passed on
  `d9815d0` before normal merge `9dc87d3`. The first macOS ARM no-OpenMP attempt
  refused OS-compressed pages; the repeated gate passed without weakening the
  guard. New-placement search, automatic policy and sustained cost validation
  are separate work.
- [x] Explicit atomic measured-portfolio application (#210): one shared owner/management
  operation for CLI and authenticated HTTP revalidates the whole observed model
  mix and all route revisions before one registry publication. Real OLMoE
  reduced-checkpoint flows pass at one and four slots, switching to an actual
  independently approved replica, serving both models and retaining all RAM
  allocations. Unauthorized/stale/conflicting requests remain inert. Registry
  unit tests and ASan/UBSan/leak checks cover private bounded snapshots, legacy
  import, interrupted staging, concurrent readers and an open-before-rename
  regression. All eight native checks passed on `a3c7078` before normal merge
  `6d95f99`. This does not prepare new
  placements, change admission limits or implement automatic scaling.
- [x] Bounded resident-portfolio controller (#211): opt-in management policy in the
  permanent API keeper, known-currency cost ceiling, measured joint latency,
  hysteresis/cooldown and switch-cost horizon. Full route/record hashes and
  a durable publication intent fence manual edits and reconcile interrupted
  changes without replay. Local real OLMoE two-model/four-slot flow passes
  automatic selection after host drain, actual subsequent generation,
  service restart, manual revision hold, missing evidence and disabling with
  the route registry unavailable. API regression and focused ASan/UBSan/leak
  tests pass. All eight native checks passed on `7a43173` before normal merge
  `01e6e98`. This only routes among approved
  resident candidates; it does not prepare/retire capacity or demonstrate
  billed savings. See [policy scope and commands](PORTFOLIO_POLICY.md).

- [x] Shared model preparation (#212): management catalogue, exact reviewed plans,
  explicit selected nodes, per-model donor consent, persistent idempotent
  operation receipts, status/cancel and web controls. The TUI and API use the
  same detached preparation keeper; READY models survive a later rejection.
  Local reduced OLMoE passes completion/rejection/cancellation through HTTP,
  real browser review/start/cancel, API and manager restart, repeat-submit
  without duplicate allocations, and real inference from each retained model.
  The original joint TUI flow and API isolation regression pass; focused
  ASan/UBSan/leak checks pass. All eight native checks passed on `6545478`
  before normal merge `449a74a`. The ARM no-OpenMP job initially refused
  inference after the runner compressed process pages; its unchanged rerun
  passed, without disabling the residency guard.
  This does not create cloud resources or grant inference access implicitly.
  See [preparation API](MODEL_PREPARATION_API.md).

- [x] Bounded repeat authorization on the donor (#213): local-owner TUI/CLI consent,
  verified content identity independent of allocation names, exact placement,
  expiry, quotas and pre-COMMIT revision checks. Revoke without evicting active
  weights; a new donor keeper requires new consent. The local tiny-OLMoE gate
  passes CLI/TUI grants, verified signatures and content, retirement/reload,
  real generation, over-quota cancellation and revocation without eviction.
  API isolation regression and focused ASan/UBSan/leak tests pass. Native tests
  exposed fragmented approval frames and Darwin CRCRLF completion lines;
  both parsers now have deterministic regressions. All eight checks passed
  on `4531e4e` before normal merge `a91175a`. The ARM jobs initially refused
  compressed process pages; unchanged repeats passed with the guard intact.
  This is an elasticity prerequisite, not the automatic replica controller.
  See [donor authorization](DONOR_AUTHORIZATION.md).

- [x] Per-model gateway demand (#214): bounded in-memory windows and per-worker events,
  operator-only endpoint and web workspace, live admission counts, explicit
  rejections/interruption and sampled latency tails. Local API/auth/restart,
  browser and focused sanitizer gates pass. Two real reduced OLMoE models
  pass four-session coexistence and 2/4/8-client bursts with the observer;
  no prompt or identity text is stored in the metrics. All eight native checks
  passed on `46976b2` before normal merge `d63fa8d`. Darwin datagram disconnects
  have a fork/exit regression; missing terminal events remain interruptions.
  This is traffic evidence, not automatic replica scaling or a
  production capacity guarantee. See [scope and limits](GATEWAY_DEMAND.md).

- [x] Elastic resident lifecycle (#219): explicit selected
  machines and allocation ownership, min/max replicas, byte budget, demand
  windows and cooldown, shared preparation with exact receipts, publication,
  drain/release and dormant wake. Local real reduced-OLMoE passes growth under
  rejected demand, inference on the added replica, another independent model,
  API restart, drain while a real admitted generation waits for a donor CPU
  permit, release after completion, scale to zero and
  wake using the same logical ID and operation. Focused tests cover lost-RPC
  completion, private-state bounds and operator reconciliation without losing
  uncertain reservations. Warning-as-error, API regression and focused
  ASan/UBSan/leak tests pass. All eight native checks passed on `83bdbee` before
  normal merge `51f9119`. The updated local real-OLMoE flow also verified ten
  prepared engines under the owned-allocation lock contract, including wake;
  no physical LAN performance, cloud provisioning or billed savings are claimed.
  See [elastic scope and commands](ELASTIC_REPLICAS.md).

- [x] Web replica controls (#220): explicit source,
  computer selection, byte-exact memory budget and lifecycle adoption review;
  revision-fenced apply, pause, resume and reconciliation. Read-only operators
  can inspect policies without gaining management rights. Browser regressions
  cover stale polling success/error after a newer action, navigation,
  revocation, cancelled confirmation and mobile layout. The real OLMoE gate
  also drives configuration through the browser when `LUMABRI_TEST_BROWSER=1`;
  the October 15 local reduced-OLMoE run passed approved preparation, growth,
  inference on the added replica, another independent model, API restart,
  admitted-generation drain, scale to zero and demand wake after the browser
  closed, with effective owned-allocation locks verified across all ten engine
  preparations. All eight native checks passed on `0ce84f6` before normal merge
  `640cc16`. This is implementation evidence, not full release certification.

- [x] Conversation-history race fixes (#217, #218) and resumed Segment
  discovery/remote retirement (#221). #221 passed all eight native checks on
  `a95bfc4` before normal merge `4a15d61`: resumed routes are refreshed before
  opening a turn; an unavailable remote CLOSE quarantines only its slot,
  keeps healthy weights resident and cannot produce a false retirement seal.
- [x] Owned CPU allocation locking (#222): native page locks for engine
  payloads and retained raw tables, with refused lock failures and build-time
  allocator auditing. Local reduced OLMoE passes two resident models, isolated
  conversations, API use and selective release. All eight native checks passed
  on `a1e6ebb` before normal merge `237c3e7`. The elastic lifecycle gate explicitly selects this contract and
  verifies effective locks across preparation, reload and wake; it does not
  weaken the separately tested observation guard or claim GPU/full-size support.

- [x] Joint placement search extension (#223): alternate node subsets,
  known-price ordering and residual-memory range refits, with explicit subset,
  state and refit limits. Deterministic regressions reproduce the old expensive
  single-node choice and false rejection of two models needing different cuts;
  32-node/eight-model bounds, allocation accounting and focused sanitizers pass.
  Local reduced OLMoE passes two-model, two-slot management-API preparation and
  real inference after TUI detach under owned CPU allocation locks. Shared
  review/revalidation and TUI regression gates pass. All eight native checks
  passed on `0196a42` before normal merge `d8e4104`;
  this is not yet requirements-based performance placement or billed savings.
  The first macOS ARM/OpenMP gate refused a compressed idle replica under the
  observation-only policy. Long-lived managed-replica CI now explicitly uses
  the owned CPU allocation lock contract and verifies every prepared engine's
  effective locks, as the elastic gate already does. Compression-refusal tests
  remain in place; no residency protection or numerical assertion is disabled.

- [x] Connected static/managed comparison (#224): two real reduced
  OLMoE checkpoints, two explicitly approved placements, observed joint timing,
  declared-price selection and automatic portfolio publication. The October 16
  local gate passes actual chat after selection, unchanged RAM on rerouting,
  selective retirement, a completely freed donor and invalidation/renewal of
  co-resident workload evidence. Four-client BUSY outcomes remain in the report;
  six of six requests pass on the admitted two-client workload in each plan.
  Eight engines prove effective owned CPU locks, including the repeated local
  gate on `2a59e9d`. Native Intel initially exposed an unexpected 503 during a
  cold concurrent lock acquisition. The new exclusive-create regression and
  native Intel/ARM workspace gates pass. All eight final-head checks passed on
  `2a59e9d` before normal merge `51ba958`. HTTP early rejections have a bounded half-close regression
  so unread request bytes do not erase the error response with a TCP reset.
  Prices are test declarations; no paid machine is stopped or billed saving
  claimed. See [comparison gate](PORTFOLIO_POLICY.md#static-versus-managed-placement-gate).

- [x] Runtime/preflight PR #151: installed engine thread-capacity query; no-OpenMP
  single-thread execution; adaptive RAM reserve; acknowledged actionable
  failure; pre-index donor reachability; build-option invalidation.
- [x] Native Intel/ARM CI with and without OpenMP and isolated two-donor flow (#151).
- [ ] Rerun the physical Windows/WSL + macOS 12.6 trial on the final build.

## Historical LAN gate inventory (superseded by managed release work above)

This older inventory is retained for context, not as the current completion
count or evidence that a later-tested path remains unimplemented. The current
resident product does not use checkpoint disk reads during inference.

1. Actual two-compute-node physical LAN chain: local oracle, split oracle,
   per-node layer ranges and memory, fixed-thread and full-hardware timings.
   Physical Tiny chat has now run with PC [0,2) and Mac [2,4) computing.
   Exact physical local/split token comparison remains pending. Mac deployment
   and approval are user-operated; CI loopback is separate.
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

- The next physical OLMoE trial used PC [0,11) and Mac [11,16). Both segments
  reached READY and Edge started in 49.3 seconds with cache reuse. The host
  no longer expired the active request, but the first prompt failed after
  304.4 seconds at the Segment RUN timeout; recovery found the original RUN
  still busy. The PC thread was observed waiting for filesystem journal
  commits, with a CAS directory descriptor open. CAS publication previously
  forced file and directory sync for every MiB before returning the weights.
  Publication now remains atomic and hash-verified without those per-chunk
  sync calls; the durable mirror ordering is untouched. Native regression
  reproduces the old calls and checks their absence plus damaged-cache repair.
  Physical OLMoE generation and the measured speedup remain open; this change
  does not extend the Segment timeout or certify timeout recovery.

- Household preparation now observes bounded, versioned source progress
  records. Indexing has a byte denominator, percentage and measured-rate ETA;
  stale estimates become unavailable. Transfer shows an activity bar and
  cumulative MB served by the requester, explicitly without a percentage or
  remaining-time estimate: demand-loaded tensors, reused donor caches and
  retries mean the checkpoint size is not a transfer denominator. Loading
  segments and starting Edge remain separate phases; indexing at 100% is not
  chat READY. This does not yet provide per-donor received-byte accounting or
  an end-to-end download ETA. No household wire change or Colibri patch is
  required for this observer.

- Real OLMoE (7.42 GB) passed household selection/approval for PC [0,13)
  and Mac [13,16), but the cold-start trial was cancelled before generation.
  The runtime sizing inspector used `fopen` for safetensors headers; the shim
  materialized whole files for stdio, causing unintended full-weight transfer
  on each donor before engine initialization. Bounded descriptor `pread` now
  preserves header/layout checks without triggering that stdio policy.
  Regression coverage includes short reads, EINTR, EOF/EIO, truncation and
  native encrypted CAS inspection of a 64-MiB payload whose unused middle
  must remain cold. A fresh isolated mirror of the actual 7.42-GB OLMoE
  materialized 41,947,136 bytes for preflight and returned the same [0,13)
  memory estimate as the local source (6,264,782,848 bytes, context 512).
  This is not yet a successful real-model LAN generation.

- The subsequent physical OLMoE trial passed preflight on both donors and
  started Edge, with PC [0,13) and Mac [13,16). Its first 26-token prompt
  did not complete: the host logged `idle session expired` during the silent
  cold prefill. Source shutdown and subsequent block EIO were consequences
  of that cancellation, not evidence that the Mac rejected the request.
  Hosted now tracks complete SUBMIT through matching DONE/ERROR, parsing DATA
  lengths so generated text cannot imitate a terminal frame. Idle expiry is
  rearmed after completion; active work has a separate absolute deadline
  (`--request-seconds`, default 1800), never extended by progress or client
  traffic. The default idle limit remains 300 seconds. A real encrypted
  bridge regression tests silent prefill beyond idle, BUSY while active,
  post-DONE expiry and an independently bounded stalled request. Successful
  real-model LAN generation and the cause of slow cold loading remain open.

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
   The loopback split rehearsal explicitly disables remote Expert hooks,
   matching the household runtime; Segment's `--direct-only` alone would not
   constrain a Hybrid archive's optional Expert transport. Reported seconds
   cover whole chat invocations, not a decode-speed measurement. Edge thread
   count remains fixed while Segment thread budgets change between phases.
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
   Household placement now retains successful proportional plans and searches
   alternative resident assignments when fixed process floors or an empty Edge
   interval invalidate that split. Only planned participants receive offers;
   every alternative is checked against the unchanged launch reservations.
   This bounded search is not an exhaustive feasibility or speed solver.
   See `HOUSEHOLD_PLACEMENT.md`.
   Per-turn Segment RUN observations now separate prefill/decode counts and
   client round-trip times for each approved range, without additional tokens.
   Recovered turns invalidate the per-range profile rather than mixing owners
   and replay costs. See `SEGMENT_STAGE_OBSERVATIONS.md`.
   Persisted measured-cost placement remains pending; this does not enable disk mode or
   certify additional model families.
4. PR #157 merged: versioned measurements collected during ordinary Segment chat,
   separating prefill, decode traversals and first visible text. No extra model
   run is required. PR #159 merged: completed household turns connect to private
   content/runtime-bound records and catalogue lookup. Context, selected
   computers and donor restarts invalidate old speeds; the last observed
   workload is shown separately from the configured context limit. Placement
   cost calibration remains pending. An optional quick-calibration action now
   requests normal donor approval, generates at most eight test tokens with a
   20-second inference deadline, saves only valid complete metrics and releases
   its plan. Transfer/loading comes first and is not covered by that deadline.
   The catalogue advice follow-up highlights
   lowest RAM reservation, largest resident checkpoint and fastest observed
   run among eligible plans; it does not rank answer quality or invent speed
   before a first measurement. See `CALIBRATION_RECORDS.md` and
   `CATALOGUE_ADVICE.md` for the exact contracts.
5. PR #158 merged: reusable adapter-scoped working mirrors and shared verified
   CAS chunks, with separate approval and fresh engine sessions on reuse.
   Cache-directory allocation is exclusive even across different homes.
   Workspace `/storage` adds allocated-block inspection and explicitly confirmed
   clearing of unused default household caches. Clearing shares the runtime
   lease, preserves source folders/settings, refuses unsafe trees and supports
   cooperative cancellation. It is not a total cache quota or automatic eviction.
   Model acquisition, cache-aware disk admission/eviction and verified smaller
   disk working sets remain pending. See `HOUSEHOLD_WEIGHT_CACHE.md`.
6. Household CPU approval is now explicit in both Colibri engine opens, rather
   than leaving backend selection automatic. Returned capabilities and selected
   routes must match; an inherited GPU preference cannot expand an accepted RAM
   budget. See `HOUSEHOLD_BACKENDS.md`. Actual upstream GPU integration, VRAM
   budgeting and native execution proof remain pending; no GPU kernel is added.
7. Two independent household chats on disjoint donors now have an end-to-end
   gate: simultaneous generation, separate engines and leases, closing one
   while continuing the other, and explicit `BUSY` for an overlapping request.
   An unrelated request must not reset an owner's pending approval choice.
   Shared-donor multi-session scheduling, 1/2/4/8 capacity measurements and
   actual replay after failure remain pending. See `HOUSEHOLD_ADMISSION.md`.
8. Native candidate assembly now has an explicit binary allowlist, retained
   upstream notices, exact file hashes and dependency reports. The installed
   `bin`/`lib` layout is exercised by the same two-donor approval, generation,
   cache/calibration and lost-donor tests. macOS candidates use system libraries
   only (no OpenMP). Engine build preparation now copies only recognized source
   inputs rather than every checkpoint/output in the Colibri checkout; failures
   preserve the previous generated tree. See `ENGINE_SOURCE_BUILD.md`.
   Native Windows, older/clean-machine compatibility,
   physical LAN acceptance and dependency-backed repository cleanup remain
   pending. Candidate artifacts are not a completed release gate.
