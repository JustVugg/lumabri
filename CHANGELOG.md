# Changelog

This file follows the commits contained by each Git tag. Changes merged after
a tag stay under Unreleased until a new tag is created.

## Unreleased

- Placement is chosen for speed: every computer that can hold a model alone is
  a candidate, and a split is selected only when no single computer fits or
  when it is estimated at least 5% faster. Reviewing one model with no
  computer ticked lets the planner choose among all sharing computers; the
  review, the API (`speed_plan`), the terminal and `--speed-plan` show the
  estimate and the reason. Estimates come from a short machine probe, link
  probes and calibration records, and were checked on fixtures only. See
  `docs/SPEED_PLANNER.md`.
- The planner reads a released Qwen3.8 checkpoint: a `config.json` larger
  than 64 KB is read whole (the released one is 72 KB) and the n-gram table
  is accepted as FP8 shards, which is how the release stores it. A truncated
  config and an integer table are still refused.
- A computer can join a household with a short one-time code instead of the
  32-character key: `lumabri pair` and `lumabri join --code`, `/pair` and
  `/join` in the workspace, and Computers > Add a computer / I have a code on
  the page. The code only selects the base point of a password-authenticated
  exchange, the tracker allows five attempts per code, and joining with the
  full key is unchanged. The exchange is checked against the published
  CPace and RFC 9380 test vectors; see docs/PAIRING.md.
- Standing consent: a computer can accept its own household's requests to
  load a model without a person clicking. One owner-only switch per computer
  (`lumabri consent on|off|status`, **/settings** in the terminal workspace,
  or **Computers & models** on that computer's own page); no other computer
  can set it. It replaces the click only: memory budget and reserve, household
  authentication, identity pinning, residency policy and model integrity are
  checked as before, and each automatic acceptance is recorded with requester,
  model and memory. `lumabri start` turns it on for a household it creates on
  a fresh installation and says so; existing installations stay off. Turning
  it off holds from the next request and unloads nothing. POSIX only for now.
- Preparing a model on the computer that also holds its files is several
  times faster. The donor's loader now reads blocks from the user's own
  source directory instead of pulling them through the encrypted transport on
  loopback, and SHA-256 uses the x86 SHA extensions when the CPU has them.
  The 7.4 GB OLMoE Q8_0 checkpoint on one PC went from 119.5 s to 22.7 s with
  an existing hash index. Integrity is unchanged: the local directory is only
  a hint taken from a private, same-user record, and every block is verified
  against the same authenticated per-MiB hashes as a block from a peer; a
  mismatch retires the local file and the peers serve it. Weights are still
  loaded fully into RAM and sealed before READY. Across computers, and on a
  Windows donor, nothing changes. See
  `docs/RESIDENT_RUNTIME.md#when-the-source-is-this-same-computer`.
- Drafts across the segments (off by default; `--drafts`/`LUMABRI_DRAFTS`):
  several candidate tokens cross the whole chain as one block of rows in one
  network round, and each segment discards the rows whose candidate was
  wrong. Replies are identical to decode without drafts, greedy and seeded
  sampling alike. Prompt lookup is the draft source; OLMoE and Qwen3.8
  segments support rollback (Qwen3.8 keeps one copy of its recurrent state
  per row). A chain with an older or unsupporting segment decodes as before.
  DeepSeek V4.1 is not wired; `docs/DRAFTS.md` says why and how. New
  `LMB_SEG_DRAFT` operation, `DRAFTS1` in `STAT`,
  `draft_metrics` in `segment_chat --json`, `make test-drafts`.
- Release archives: `.github/workflows/release.yml` builds one self-contained
  CPU archive for Windows x86-64, Linux x86-64 and ARM64, macOS Apple Silicon
  and Intel, on a `v*` tag or by hand. Each archive is verified, unpacked and
  started on its runner (page, sign-in, first-model offer, `service stop`), and
  a tag attaches the archives and `SHA256SUMS` to a draft release. Linux
  archives link OpenMP statically and need only glibc 2.35 and, for model
  download, the system libcurl. Linux packages now carry `SHA256SUMS` like
  macOS. The archives are unsigned. See `docs/RELEASE_ARCHIVES.md`.
- Qwen3.8's own draft head (MTP) proposes the drafts (`--drafts head|auto`)
  when the model's GGUF carries it (`nextn_predict_layers = 1`, block
  `layers`). The Edge loads the head resident, with all of its experts and
  none of the model's layers, and keeps the head's rows per conversation.
  Its proposals equal the unmodified engine's head token for token. The
  planner prices the head at the Edge whenever the file has one; a GGUF with
  a head was refused before and is now accepted. A head in a separate
  `mtp-*.gguf` or in safetensors is not read yet, and the saving on the real
  checkpoint is not measured yet.
- `lumabri start` no longer fails its first model on computers with a small
  locked-memory allowance: when no residency policy was requested, its sharing
  service locks a load whose reservation fits the allowance and otherwise
  holds the weights fully in RAM without locks, reported as "not locked" in
  the start output, donor log, service status, `lumabri doctor` and the page.
  An explicit `engine-locked` and all other commands still refuse as before.
- The sign-in that prepares a model from the page receives the inference grant
  for that allocation when it is ready, and the chat picker gains the model
  without a reload. A full grant list is reported instead of failing silently.
- Model download on request: `lumabri fetch` and a "Get a model" card on the
  local page fetch a repository snapshot or named GGUF files into the models
  folder, resumable, over verified HTTPS, checked by size and hash before the
  model becomes visible. Only models that the household's offered memory can
  hold are offered. Nothing is downloaded, prepared or loaded automatically.
  The transfer runs in the new `lumabri-fetch` helper, the only household
  program that links libcurl. See `docs/MODEL_DOWNLOAD.md`.
- Raspberry Pi: `segment_node` without `--max-rows` now clamps its 256-row
  default to the engine's batch cap instead of refusing to start (OLMoE caps
  at 128), and arm64 Pi kernels report the CPU via the `Model` line of
  `/proc/cpuinfo` instead of "unknown".
- Web workspace controls for authorized-machine replica automation: explicit
  computers and memory budget, review before applying, exact policy/route
  revisions, pause/resume/reconciliation, read-only operator visibility and
  protection from stale replies. The same service owns execution and donor
  consent; the browser neither places models itself nor creates cloud servers.
- Opt-in elastic resident replicas on explicitly authorized machines: bounded
  demand-driven preparation, exact operation receipts, live-contract publication,
  withdrawal/drain/release, and on-demand wake with stable model IDs and grants.
  Pending resources remain charged through uncertain outcomes; an operator can
  reconcile quiescent preparations without replaying offers. Native CI includes
  the real reduced-OLMoE lifecycle. This is not cloud provisioning or certified
  production capacity. See `docs/ELASTIC_REPLICAS.md`.
- Preparation status re-reads the exact operation after an unavailable keeper
  RPC, so a concurrently completed operation is not mistaken for failure.
  A timeout alone never authorizes another allocation.
- Add opt-in `engine-locked` CPU allocation residency: page-aligned owned
  buffers and retained raw tables are locked before use, with explicit
  refusal on lock failure, allocator-boundary auditing and native tests.
  Default observation and process-wide `locked` policies are unchanged.

- Segment drain/resume promptly republishes admission changes. A hosted turn
  waits cancellably for complete discovery within its configured deadline,
  before opening sessions; stale draining snapshots no longer reject it
  immediately. Residency and admission guards remain authoritative.
- An unreachable Segment CLOSE no longer terminates a healthy resident host.
  The affected conversation slot remains quarantined until retirement is
  confirmed; neither slot reuse nor host retirement can invent a clean reset.

- Chat-history saves return the document and revision committed by that
  request, even when another request updates or deletes it before the reply.
  Concurrent edits retain their revision-conflict protection.
- Browser chat keeps generation saves bound to their conversation when a
  history load completes late; deleting a conversation preserves another
  selected chat or a new draft and blocks sends to the pending deletion.
- Multi-host benchmark and production gates reject malformed token oracles,
  boolean or fractional counters, non-finite timing or throughput values, and
  mismatched model, family or mode across release reports.
- Segment routing uses lease-bound, executor-measured RUN compute costs to
  choose among sampled resident replicas during initial placement and
  recovery; optional sideband telemetry preserves discovery v1 compatibility.
- Segment pressure recovery now drains in-flight sessions through ordinary RAM
  pressure, recovery hysteresis and manual pause; only the critical RAM/swap
  floor aborts a kernel. A failed RUN can reopen the same exact-range executor
  (direct or relay) with clean-state replay, and client errors preserve the
  actual executor/transport status instead of claiming that every failure is a
  missing replica.
- Donor runtime coordination: one automatic RAM-sized compute donation per
  machine/user now covers both chat roles and `serve --join`; duplicate starts
  remain useful as storage-only peers and identify the current lease owner.
  Linux children also terminate with their parent instead of leaving resident
  Expert/Segment executors behind after a killed TUI.
- Colibri's mutable local `.coli_*` runtime state (including `.coli_usage`) is
  excluded from storage manifests, so it can no longer generate repeated
  unsigned-content rejections. Governor transitions now report the exact
  pressure reason, available/reserved RAM and resident RSS/weights.
- WSL1 memory compatibility: when `/proc/meminfo` omits `MemAvailable`,
  machine profiling, doctor, governor, Segment and resident Expert sizing now
  share a conservative reclaimable-cache fallback; native Linux values remain
  authoritative when present.
- Streaming Segment replies with live routing/prefill/decode/checkpoint/failover
  status, a fixed non-blocking input dock, slash-command autocomplete and menus
  usable during inference.
- Immediate live-turn interruption: `Ctrl-C`/`Ctrl-\\` stop the engine instead
  of queueing an exit behind inference, `Ctrl-Z` restores and reconstructs the
  dock across suspend/resume, and an exit guard restores termios after normal
  shutdown signals. Terminal recovery happens before waiting on the engine; a
  second shutdown signal provides an async-safe forced exit.
- Clean Segment gateway EOF on ordinary `/quit`, so remote sessions are closed
  instead of occupying executor slots until their lease expires; per-slice
  session capacity also has a bounded operator override.
- Stable machine names, named `/swarm`/`/hosts` topology, `/experts` call
  counters and periodic server-side host/executor/session telemetry.
- Per-source Segment-relay token bucket, concurrency cap and bounded serialized
  executor queue while caller-signed `LMB_RSEG` identities remain future work.
- Restore isolation: multi-GB restores no longer hold the node-wide session
  lock, and same-session `CLOSE` returns busy without blocking other chats.
- Automatic Segment resource governor: CPU teams are bounded per slice,
  fallback work is low priority, relay-only origins have lower session caps,
  new sessions preserve a RAM reserve and automatic startup requires spare RAM.
  **Release note:** NAT servers now start Segment slices too; they therefore use
  additional CPU/RAM unless `--no-exec` is passed.
- Segment temperature/top-p sampling through Colibri Edge logits, while
  preserving the exact greedy path at temperature zero.
- Stateful Segment relay over signed outbound tracker tunnels, including
  relay-only NAT executors and automatic direct-to-relay retry.
- Chunked opaque snapshots, transactional restore and exact-range replica
  replay after peer failure; checkpoint copies are skipped without a replica.
- End-to-end gates for all six Colibri families, relay-only serving, failover
  replay and the ordinary `lumabri serve` + `lumabri chat` path.
- Persistent endpoint TOFU and strict `LUMABRI_PEER_PINS` verification.
- Fail-closed encrypted startup, X25519 low-order rejection, bounded aggregate
  receive memory, one-buffer AEAD frames and encrypted-fd lifecycle cleanup.
- Durable tracker name-to-key bindings, channel/registration identity binding,
  per-key and per-source admission quotas.
- Constant-time invite-token comparison and hardened peer-key/sidecar files.
- Expanded RFC vectors, tamper, replay, pinning, restart, quota, memory and
  fail-closed tests.
- PR #21 token-length validation, PR #22 per-process sidecar staging and PR #24
  encrypted wire-capture test fixes were merged after v0.8.0 and therefore
  belong here, not in the v0.8.0 tag.

## v0.8.0

- Opt-in authenticated encrypted transport using ephemeral X25519, Ed25519
  handshake identities, HKDF-SHA512 and ChaCha20-Poly1305.
- Self-contained transport primitives and RFC 8439/RFC 7748 tests.

## v0.7.0

- Signed REGISTER/EREG challenge flow and per-machine Ed25519 peer identity.
- First-use name ownership, Ed25519 undefined-behaviour fixes, per-key live-name
  cap, signal-handler hardening, warm-cache integrity fixes and telemetry fix.

## v0.6.0

- Protocol v2 build/checkpoint identity, crash-safe multi-process mirror,
  accept-path bounds, continuous discovery and dense/expert prefetch boundary.
- Multi-row speculative EXEC, fixed-delay hedging, local content-addressed
  storage, manual signing-key rotation and READ/EXEC NAT relay.
- Path, manifest, SIGPIPE, build and test hardening merged in this tag range.

## v0.5.0

- P2P byte serving and remote expert execution for OLMoE, GLM, Inkling, Kimi K3
  and DeepSeek V4, with per-engine byte-identity tests.
- TUI, live swarm view, model switching, rarest-first assignments, invite
  tokens and the generated-patch engine integration model.
