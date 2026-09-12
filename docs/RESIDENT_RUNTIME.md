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
directly into engine memory, with one bounded transport block. Sparse virtual
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

- Native CI on the final commit and the physical PC/Mac
  transfer/prepare/generate/reconnect/Stop test.
- Measured memory-pressure/pageout policy, not a claim based on malloc alone.

Concurrent donor/chat operation, independent private sessions, a web chat UI,
and upstream-supported GPU execution remain separate uncompleted product
requirements. Segment still places per-layer KV on the approved donor nodes;
local chat history alone does not keep that KV solely on the chatter.
