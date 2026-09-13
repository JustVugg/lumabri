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

The first four routed inputs of each accelerated layer also compare the
remote contribution against the local callback. A mismatch is replaced with
the local result and disables remote use for that resident engine lifetime.
This smoke test does not prove equality for every possible later input.
`split` cannot override a detected incompatibility.

The physical MoE microbenchmark detected small cross-platform differences
(maximum absolute error 1.49e-8) despite the same advertised numeric class.
Raw split results therefore failed its strict bitwise gate. Timings were also
highly variable; they must not be presented as certified token/s or a speedup.
The benchmark's optional `LMB_BENCH_INSPECT_DIFF=1` only quantifies raw
differences and still exits nonzero on a mismatch. It is not a production
bypass. The normal test must either match or report local fallback explicitly.

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
