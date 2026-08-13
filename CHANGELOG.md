# Changelog

This file follows the commits contained by each Git tag. Changes merged after
a tag stay under Unreleased until a new tag is created.

## Unreleased

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
