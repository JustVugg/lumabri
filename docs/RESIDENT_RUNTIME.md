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
control lease. The loader unit/integration test checks byte-exact retained
range reads after sealing, denied undeclared reads, and denied file mappings.
Native CI runs these checks separately from legacy disk-cache tests.

These are synthetic checkpoints, not the production OLMoE checkpoint or a
physical PC/Mac LAN. Passing them does not establish production readiness.

## Outstanding release gates

- Native CI, real-checkpoint numerical comparison, and the physical PC/Mac
  transfer/prepare/generate/reconnect/Stop test.
- Measured memory-pressure/pageout policy, not a claim based on malloc alone.

Concurrent donor/chat operation, independent private sessions, a web chat UI,
and upstream-supported GPU execution remain separate uncompleted product
requirements. Segment still places per-layer KV on the approved donor nodes;
local chat history alone does not keep that KV solely on the chatter.
