# Resident runtime: implementation and release gates

This is an implementation record, not a production-support claim. Household
requests and donors now select resident loading by default. An explicit
`LUMABRI_RESIDENT_REQUIRED=0` is retained for legacy cache regression tests;
the product never falls back to it when resident preparation fails.

The workspace's first action reopens the last accepted resident model after
chat or requester restart. The private, versioned `resident-plan` file contains
the approved allocation, host key and checkpoint root, not weights or chat
text. Reconnection verifies the encrypted peer identity and the host's root
before sending any conversation text. Missing or changed hosts are reported;
they never silently trigger a download or replace a donor's allocation.

Use Explore models to prepare a different model. Donor owners explicitly
unload their existing allocation first (`x`, while continuing to share).
Only metadata, sparse maps, signed hashes and logs require disk headroom;
household admission no longer reserves two full checkpoints on every donor.

## Weight lifetime is not conversation lifetime

The accepted donor allocation owns its weights. Normal chat closure sends
`HOME_DETACH`, not a plan cancellation. A fully prepared allocation also
survives requester disconnection and control-lease expiry. Its compute and
weight-cache leases remain held, and the donor displays retained RAM.

The host's Segment gateway advertises `LUMABRI_RESET 1`. Between clients the
host drains any complete accepted request, sends a private RESET nonce and
waits for its acknowledgement at a codec boundary. The gateway closes the
old remote sessions and clears its conversation and sampler, without closing
the Edge engine or the Segment engines. Generated DATA cannot impersonate
the acknowledgement. A damaged or legacy gateway is not reused with stale KV.

Owner Stop, donor exit, a failed preparation or explicit plan cancellation
release the allocation. A new requester cannot silently repurpose retained
weights: the accepted client identity and allocation remain authoritative.
After the entire plan is ready, a failed chat does not roll back healthy
donors. They retain their weights and reservations until their owner unloads
them, even if another participating computer disappears. This is retention,
not automatic failover: the interrupted chat still reports the missing node.

## Preparation and inference

The preparation loader transfers authenticated, hash-verified weight blocks
directly into engine memory, with a bounded, verified preparation cache.
It holds up to four blocks, never more than 64 MiB per engine; the slot count
shrinks for larger protocol blocks. This prevents interleaved gate/up/down
reads from fetching the same block for every matrix. Both Segment and Edge
reservations explicitly include this temporary allowance. All buffers are
freed when weight input is sealed, before READY. Sparse virtual
file descriptors exist for metadata/offset compatibility; the resident path
does not write weight payloads to the mirror or CAS.

All assigned expert slots are prepared before READY. Kimi embeddings, V4
embedding/head and Qwen3.8 PLE tables are explicitly retained as native raw
tensor ranges where upstream kernels still use pread. Those reads resolve
to anonymous RAM, not to a disk file. No numeric kernels or quantization
rules are changed. Preparation hooks are applied only to disposable build
copies; the upstream Colibri checkout is unchanged.

After preparation the external weight input is sealed. Engine-owned tensors
and explicitly retained ranges remain usable. An undeclared late external
read is rejected instead of hiding missing preparation behind disk I/O.

Retained anonymous memory is **not** a claim of OS-level `mlock`/no-swap.
Memory reservation and no checkpoint reads are separate from operating-system
pageout guarantees. Strict physical page locking and memory-pressure behavior
still require platform-specific validation and admission policy.

## Evidence

The local nine-fixture matrix covers eight families plus Qwen3.8 FP8:
OLMoE, Qwen3.6, Inkling, Kimi, GLM, GLM5.3, Qwen3.8 and DeepSeek V4.
It prepares two real adapter ranges, generates a reply, exits the requester
and its checkpoint source, and reconnects with the accepted identity for a
second conversation through the normal workspace. It also rejects a wrong
checkpoint root on the correct host. It checks no engine reboot, no allocated weight-mirror
blocks, retained allocation leases, and release on explicit owner Stop.

`--crash-requester` additionally tests retention beyond the old 15-second
control lease and the source's 30-second tracker-advert TTL. The loader unit/integration test checks byte-exact retained
range reads after sealing, denied undeclared reads, and denied file mappings.
Native CI runs these checks separately from legacy disk-cache tests.

### Real OLMoE checkpoint, September 11, 2026

The 7,417,133,727-byte OLMoE checkpoint also passed the household test with
two loopback donors, each offering 8 GB, CPU execution and context 128:

- approved ranges `[0,7)` and `[7,16)`; all 64 experts of each layer prepared;
- first chat, requester exit, source shutdown, and second chat from the TUI;
- unchanged engine boot counts; no allocated safetensors mirror blocks;
- memory reservations held until explicit donor-owner Stop;
- 4.4 MiB and 328 KiB of metadata/log/cache-directory storage on the donors,
  not two copies of the 7.4 GB checkpoint.

The two short turns displayed 8.30 and 8.56 tok/s (eight generated tokens),
with 1.2 and 1.1 seconds of prefill. These are short CPU loopback observations,
not a sustained-speed claim, a physical LAN measurement, or a Mac benchmark.
Preparation served about 25.6 GB because bounded transport blocks can be
requested repeatedly. This initial-transfer overhead is not inference disk
I/O; it remains an optimization opportunity, not a hidden zero-download claim.

Separately, `segment_split_test.sh` on the same real checkpoint produced
identical greedy token IDs across one and two nodes (eight tokens, two rounds,
plus warm-up). At four total threads, complete invocations took 1.811 versus
1.859 seconds. This is a split-equivalence check using local source files,
not a claim that that separate benchmark exercised sealed resident loading.

The full `make test` regression suite passed. The native resident-runtime
matrix at commit `2f0c556` passed on macOS Intel/Apple Silicon with and without
OpenMP; the later TUI/packaging commits require their own green run.

## Preparation lease monitor

A physical PC/Mac attempt reached resident READY on the PC but the Mac
reported `Request lease expired before preparation completed`. Preparation
previously sent heartbeats from the same loop that rendered the terminal.
Blocking that terminal for longer than the 15-second lease reproduces the
failure on the prior packaged build; the new regression is
`home_flow_test.py --resident-default --stall-preparation-ui --expect-metrics`.

The connection monitor now runs from donor approval through loading and chat,
independently of foreground rendering. It exclusively reads donor statuses;
the UI consumes synchronized snapshots, and command writes are serialized.
Preparation redraws are capped at ten per second. Control I/O bounds are
reapplied after the encryption handshake, which otherwise restores the
general timeout. Donor closure reasons are forwarded when possible; transport
failures identify the affected donor address. The lease remains finite and
incomplete allocations still roll back when the requester is truly lost.

The paused-terminal regression fails on `b0156a6` and passes with this fix:
two real local donors retain their leases during a 20-second output pause,
then complete resident inference and reopen a second conversation without
weight reloading. This proves the rendering/heartbeat bug, not that every
possible physical-network interruption is resolved. Physical retesting remains
required. No planner allocation change is included in this fix.

The subsequent physical attempt (`ba216e20515b1190`) used requester
`29f7732` but the prior donor packages. It still failed: the PC reached
READY, the Mac closed its control connection and displayed lease expiry.
This is a failed physical gate, not a successful resolution. A no-allocation
probe then authenticated to both live donors and observed each still waiting
for an OFFER after four seconds. The new `test-home-monitor` regression also
exercises a real encrypted handshake and AUTH followed by an idle connection;
with default settings the updated donor bounds that wait to one second.
That verifies the timeout correction, not the cause of the physical failure.
Both donor applications must be updated before the next comparison. The
native/Linux CI for `29f7732` passed; native macOS 12.6 and physical LAN
completion remain unverified.

## Outstanding release gates

### September 13 PC / Intel Mac preparation regression

The physical Hybrid run reached accelerator READY on the Mac (four OpenMP
threads), but the coordinator stopped after preparing layer 12. Source
progress was still increasing at 897.6 seconds: 29.14 GB served across remote
and local readers. The requester had a fixed 900-second preparation limit;
cancelling the source then surfaced as a read error and donor disconnection.
No conversation or speed measurement completed in that run.

Preparation now expires after 15 minutes **without** a transfer-byte advance
or a forward preparation-phase transition, with a separate 24-hour safety
ceiling. Heartbeats and repeated status messages do not renew this allowance.
Expiry names the reason before rolling back the approved allocations.
The interleaved input cache above addresses the repeated-block amplification;
it does not add a disk inference path.

Regression gates: fake-clock progress past 900 seconds and stalled/hard-limit
expiry; authenticated interleaved and cross-shard reads with bounded eviction;
byte identity, empty weight mirrors and post-seal refusal; complete two-donor
Hybrid TUI flow including paused rendering, requester loss, retained RAM and
calibration invalidation. The updated physical PC/Mac run is still required.

The 7.4-GB OLMoE checkpoint subsequently passed the complete two-donor
loopback flow with context 4096, resident Hybrid, second conversation with
the source offline, no reload and zero weight-mirror allocation. Logs recorded
336 completed remote expert calls and 2352 concurrent local expert calls.
This was a functional test with two threads per donor on one physical PC,
not a PC/Mac speed comparison. Use `home_flow_test.py --prepare-timeout 3600
--io-timeout-ms 300000` for this large CPU diagnostic: the harness defaults
remain short for tiny CI fixtures. A prior run with the forced 10-second test
I/O deadline reached READY but timed out during prefill and entered recovery;
the production transport default was not changed to make this test pass.

- Native CI on the final commit and the physical PC/Mac
  transfer/prepare/generate/reconnect/Stop test.
- Measured memory-pressure/pageout policy, not a claim based on malloc alone.

Concurrent donor/chat operation, independent private sessions, a web chat UI,
and upstream-supported GPU execution remain separate uncompleted product
requirements. Segment still places per-layer KV on the approved donor nodes;
local chat history alone does not keep that KV solely on the chatter.

### September 13 Hybrid performance follow-up

The updated physical PC/Mac preparation completed and three 32-token chats
reported 3.66, 3.56 and 4.09 token/s (OLMoE, context 4096, PC 12 threads,
Mac 4 threads, approved accelerator layers 14:16). Weight-source bytes did
not advance during generation. This establishes execution, not a speedup
against historical PC-only runs with potentially different builds/load.

The calibration failure message previously conflated a missing leased advert
with a changed runtime. Missing inventory is now retried for two reporting
periods; an observed hardware/endpoint/runtime/thread mismatch still rejects
the measurement immediately and names the component. The original approved
identity is never replaced with the most recent report. This fixes the
classification/retry; the exact cause of the original physical advert failure
has not been established. Isolated real-model calibration/reopen/invalidation
passed with this change; the physical catalogue still needs retesting.

Resident Hybrid now measures local and split work per layer. Four interleaved
samples per path initialize an EWMA; split must be at least 3% faster. The
other path is probed every 64 observed rounds. This is a workload-dependent
heuristic, not a monotonic latency guarantee. Approved donor memory remains
resident. `LUMABRI_HOME_HYBRID_POLICY=local|split|adaptive` is an advanced
diagnostic setting inherited when the donor starts (default adaptive), not a
new permission or public-discovery route. Logs separate send, local work and
collection wait. Prewarming/calibration probes are included in reported time.

The original `23ebce7` check compared the first four remote contributions
bit-for-bit against the local callback. This was too strict as a household
cross-platform compatibility policy: a difference alone does not establish
an incorrect model. The revised FP32 envelope (policy v1) is
`abs(remote-reference) <= 1e-6 + 1e-5 * abs(reference)` for **every** component,
with finite inputs required. These are explicit engineering tolerances, not
a universal model-quality theorem. They do not promise identical greedy
tokens. Only the already-approved OLMoE Hybrid callback currently uses this
policy; model/root, shape, numeric-class and peer authorization still apply.

Small differences retain the remote result and are logged as rounding, not
silently recomputed locally. An out-of-envelope probe is replaced locally
and disables remote use for that engine lifetime. Probe the first four remote
inputs per accelerated layer and periodically every 64 layer rounds; examine
all replies for NaN/Inf even between probes. Forced `split` cannot override an
out-of-envelope/non-finite result. These smoke tests cannot establish a bound
for every future input. Their extra reference computation is excluded from
the scheduler's service estimate but remains in actual end-to-end timings.

The physical MoE microbenchmark detected small cross-platform differences
(maximum absolute error 1.49e-8) despite the same advertised numeric class.
Raw split results therefore failed its strict bitwise gate. Timings were also
highly variable; they must not be presented as certified token/s or a speedup.
The benchmark's optional `LMB_BENCH_INSPECT_DIFF=1` only quantifies raw
differences and still exits nonzero on a mismatch. It is not a production
bypass. The normal test must either match or report local fallback explicitly.

`LMB_BENCH_GREEDY=1` adds a separate full-model check with the actual tokenizer,
chat template, three fixed prompts, fresh KV, and up to 32 greedy tokens per
prompt. Local and forced-split execution must produce identical token IDs;
fallback or zero remote calls cannot pass this test. It loads another resident
copy on the PC and must only run with sufficient spare RAM. Its cache-miss
check proves no late expert loads, not OS-level no-swap behavior. Passing this
finite corpus is evidence for the tested build and devices, not universal
bitwise reproducibility across platforms.

The September 14 physical test of the **original** `23ebce7` runtime completed
three 32-token hosted turns: 6.61, 6.49 and 7.19 tok/s (median 6.61), fresh
conversation each time. Calibration was saved and the source's transfer
counter stayed at 11,264,897,654 bytes after preparation. The first remote
probe was discarded, so these are **local-fallback** speeds, not Mac speedups.
A subsequent isolated raw-result microbenchmark on the same approved Mac
measured 5.212 ms per native-local MoE layer versus 11.434 ms split, over
48 timed samples each. Maximum absolute difference was 1.49e-8. Those are
synthetic per-layer timings, not chat throughput; variability remains material.

The first draft of the revised policy used an absolute floor of `1e-7`.
The full-model test encountered a 2.04891e-7 expert-output difference and
correctly reported **NOT VALIDATED**, because fallback had replaced the Mac.
That run is not evidence of remote equivalence. The revised `1e-6` absolute
floor plus `1e-5` relative tolerance subsequently passed the three-prompt
physical oracle: **96 identical token IDs, 513 remote calls, 18 rounded probes,
no numerical fallback and no late expert loads**. This validates the finite
corpus on that PC and Intel Mac; it does not certify all prompts or platforms.

For the three prompts, 31 decode steps took respectively 4.044 / 3.663 / 3.255
seconds locally and 6.138 / 4.498 / 4.633 seconds with forced splitting.
This uses a separate in-process diagnostic model, context capacity 512 and
12 PC threads, not the hosted TUI timing path. Mac computation was used, but
this one-remote-expert-per-covered-layer policy was slower on every prompt.
Numerical compatibility is fixed for this corpus; a PC+Mac speedup remains
unproven. The adaptive policy is allowed to prefer local execution when the
measured split service is slower; retained donor memory is not evidence of
useful concurrent execution.

The revised policy also passed the local household flow (approval, resident
Hybrid, timing/calibration persistence and invalidation, and a second private
conversation with the checkpoint source offline and no weight-mirror writes),
the parallel/order/failure unit tests including non-finite replies after the
startup probes, and the PTY UI regression. `/plan` now explicitly separates
approved resident capacity from live execution. These results do not replace
native CI for the revised commit or establish an acceleration over the PC.

`make build/bench_home_hybrid ENGINE=...` builds this opt-in OLMoE diagnostic
against prepared sources. It reuses already approved routes and requests no
new donor allocation. Its numbers are milliseconds per MoE layer, not full
generation throughput. `ENGINE_CPU_FLAGS` permits explicit local tuning and
invalidates the generated archive; portable packages leave it empty. A native
build must not be redistributed to a different CPU, and changing SIMD/math
paths requires renewed numerical and performance validation. Colibri's source
checkout remains unchanged.

Final local gates passed with the numerical fallback enabled: the two-donor
resident TUI flow, saved/reopened calibration and invalidation, second private
conversation with the source offline, and zero weight-mirror writes. Against
the still-approved physical Intel Mac, the microbenchmark returned
`NUMERIC FALLBACK PASS`: one remote probe was rejected and all delivered
results matched the native local oracle. That proves the fallback, not a
cross-machine speedup. No final full-chat A/B or native CI result is implied.

One repeated flow exposed a separate streaming failure on the random tiny
tokenizer: conservative UTF-8 replacement-character holdback moved before an
already emitted, unchanged prefix. Streaming now retains verified emitted
bytes, holds replacement characters at the end of the common prefix, and
still rejects actual prefix rewrites. Targeted prefix tests and the subsequent
complete resident flow passed.

## Perfect-draft verification experiment (2026-09-14)

The opt-in benchmark now accepts `LMB_BENCH_BLOCK=1`. This mode does not use
household credentials, remote routes, allocations or a draft model. It loads
one separate resident OLMoE copy, generates a native greedy reference, then
teacher-forces the same continuation through 1/2/4/8-row verification. Every
row receives normalization, the vocabulary projection and argmax, and every
prediction is compared with the reference. Calling `step(S)` alone would
return only the last row and undercount verification work.

Example developer invocation (not a portable binary build):

```sh
make build/bench_home_hybrid ENGINE=/path/to/colibri/c ENGINE_CPU_FLAGS=-march=native
LMB_BENCH_BLOCK=1 LUMABRI_NO_EXEC=1 COLI_NO_OMP_TUNE=1 PIN=off \
  build/bench_home_hybrid /path/to/olmoe 0 16 12
```

Measured on the i7-1355U Linux/WSL PC, OLMoE merged resident INT8 expert
checkpoint, GCC 13.3, `-O2 -march=native -fopenmp`, 12 threads, context capacity
512. Three prompts, 16 continuation positions each, three rotated repeats.
Prefill and weight preparation are excluded; all verification work is timed.
The table uses the median of the three 48-position corpus totals, not the
best run. The tiny-model test first passed the same correctness checks.

| Mode | Median seconds / 48 positions | Ideal positions/s | Relative to native |
| --- | ---: | ---: | ---: |
| Native one-token decode | 4.730674 | 10.147 | 1.000 |
| One-row verifier control | 4.639351 | 10.346 | 1.020 |
| Two-row verification | 3.896706 | 12.318 | 1.214 |
| Four-row verification | 3.363934 | 14.269 | 1.406 |
| Eight-row verification | 3.437260 | 13.965 | 1.376 |
| Eight rows, experimental batched head | 3.141728 | 15.278 | 1.506 |

All **864 greedy predictions** matched; there were zero late expert-cache
misses. This is not a universal numerical-equivalence or OS no-swap proof.
The final mode batches only the vocabulary projection using the existing
matrix kernel; it is diagnostic code, not a deployed Edge implementation.
Colibri is not modified. Binary SHA-256 for this run:
`dfc7cce471d9c706e7dd620d1f377e31e80421f9bf34d292def523addcd9a61e`.

These are optimistic verification rates for **perfect, free proposals**,
not measured speculative chat rates or a PC+Mac speedup. Real drafting,
communication, rejection and rollback must still be paid. This locally tuned
binary and short-context workload cannot be compared directly with the older
6.60 tok/s packaged hosted-chat result. Timings varied substantially across
repeats; a larger deployment benchmark is still necessary.

Decision: batching creates measurable headroom, but this verifier alone does
not establish a doubling or 20 tok/s. At the measured rates, eight-row
verification costs about 5.8 native token times (5.3 with the experimental
head), before drafting/network/rejection overhead. A real proposer must
therefore deliver enough useful consecutive outputs to repay that cost.
More speculative candidates are not automatically useful: four rows beat
eight in the unmodified-head comparison. OLMoE's MoE loop still processes
rows individually; grouping the rows that use the same expert is a separate
kernel experiment, not an acceleration claimed by this measurement.

## Causal proposer experiment (2026-09-14)

`LMB_BENCH_CAUSAL=1` replaces perfect proposals with a zero-weight n-gram
proposer. It sees only committed prompt/output tokens, requires a suffix match
of at least two tokens, and proposes at most three tokens. Verification uses
all rows and the experimental batched head. At a mismatch, only the accepted
prefix plus the correction token is appended; OLMoE's causal KV position is
restored and the discarded suffix is overwritten. This rollback is not valid
for recurrent compressed attention and is not offered to other adapters.
The reference continuation is used only to check results, never to propose.
Special-token stops are honored. No remote proposer or network is involved.

The same PC, model, compiler flags and 12-thread setting as the preceding test
were used, with up to 32 generated positions, four prompts and three repeats.
The initial prefill/first prediction is reported separately and excluded from
the decode times below; all proposal, verification and rollback costs are
included. The repetition prompt is deliberately separate from ordinary chat.

| Prompt | Native median decode seconds | Causal n-gram median seconds | Native / speculative |
| --- | ---: | ---: | ---: |
| Cappelletti description | 2.896520 | 2.917638 | 0.99 |
| Arithmetic question | 3.142904 | 3.466701 | 0.91 |
| Python function | 3.378155 | 5.869074 | 0.58 |
| Explicit sentence repetition | 3.788574 | 2.701853 | 1.40 |

All **768 generated positions** matched. Across speculative runs: 123 proposed
tokens, 75 accepted, 18 rejected blocks, zero late expert-cache misses. The
median full-corpus decode time was 15.133205 s native versus 15.509343 s with
speculation (8.194 versus 7.995 decode positions/s). Timings had substantial
outliers; the roughly 2.4% corpus difference is not evidence of a statistically
established slowdown. It is also **not evidence of a general speedup**.
Binary SHA-256: `a9cf6dc06c00e2372305fab840ccf0c411c8ea58c6d747fa404e8d1cfae82020`.

Decision: do not enable this proposer in household chat or offload it to a
donor as a claimed acceleration. It has too little useful work on ordinary
prompts, even before communication is added. Repetition-only improvement
does not satisfy the PC+Mac objective. A better compatible proposer and/or
cheaper verification still needs to be demonstrated. The tiny-model contract
test validates accepted-prefix handling and real rollback after rejection;
its repetitive/random output is not a performance proxy for a trained model.

### Expert RPC failure diagnostics

The client now logs `[expert-rpc]` when receiving an expert contribution fails.
`reason` distinguishes a poll reply timeout, a receive timeout (including a
partial frame), connection closure/reset, an invalid frame or reply, AEAD
authentication failure, and an explicit remote error. Each line includes the
peer, layer, expert, errno, poll events, elapsed time since dispatch and the
configured poll wait. Elapsed time includes overlapping local computation;
it is **not** a measurement of pure network RTT. Socket I/O timeouts remain
separate from the poll wait. Timeout, retry and admission policies are unchanged.

Only known fixed remote error strings are logged. Arbitrary remote text,
activations, credentials and frame payloads are not printed. EOF and malformed
plain/encrypted frames set explicit errno values instead of reusing stale
errors. A failed contribution still invalidates the strict Hybrid benchmark;
local fallback is not counted as donor acceleration.

Socket-pair regression tests cover withheld replies, EOF, withheld payloads,
wrong reply sizes, remote busy/errors and malformed frame headers. Encrypted
tests cover EOF, authentication failure and malformed headers. Parallel order,
fallback, failover, encryption and ASan/UBSan checks passed locally. These
diagnostics do not by themselves establish the cause of a physical-network
failure or a speedup.
