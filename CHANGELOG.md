# Changelog

This file follows the commits contained by each Git tag. Changes merged after
a tag stay under Unreleased until a new tag is created.

## Unreleased

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
