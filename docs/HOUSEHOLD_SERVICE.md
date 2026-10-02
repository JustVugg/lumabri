# Household service and resident plans

The TUI is a client. Closing it no longer stops sharing, the household tracker,
or an approved preparation. New donor requests still require explicit owner
approval in **Share resources**. Leaving that view is not accepting an offer.

## Ownership and recovery

The local manager and the resource keepers are separate processes. A donor
keeper owns its compute/cache leases, inventory reporter, Segment engine and
optional Edge host. A tracker keeper owns the household tracker. A preparation
keeper owns the checkpoint source and the approved multi-donor transaction.
Prepared allocations remain with their donors when that transaction finishes.

Each role has an exclusive file lock and a private Unix-domain control socket
under `~/.lumabri/service`, with kernel-verified same-user credentials. The
directory is mode 0700; socket, lock and journal files are private. The service
uses a private hashed directory under `/tmp` for the socket only when a long
home path exceeds the platform's Unix socket path limit. Locks and journals
stay in the home directory; kernel credential checks remain mandatory. Commands
are bounded and versioned. Mutations identify both the keeper incarnation and
the immutable allocation: an old confirmation cannot accept or unload a new
request. LAN encryption, endpoint pinning, household keys and donor consent
are unchanged.

Configuration and operation observations are atomically journaled. The
manager reconciles live sockets with recorded operations after restart. A
saved PID is diagnostic data, never permission to kill or adopt a process.
A record with no responding keeper is **interrupted**, not a live allocation.
The manager never replays an approval or starts a second reservation from it.

`lumabri service restart` replaces only the manager. Live keepers and model
processes survive. If a keeper itself crashes, its existing child supervision
cleans up its engines; this is **not** transparent model recovery. A new
allocation requires fresh approval. Rebooting the computer does not preserve
RAM or resume models automatically. No system-wide login service is installed.

## Interface

- **Share resources:** Esc detaches; `x` unloads the current allocation;
  `s` stops the local sharing service. Pending offers remain declined by default.
- **Preparation:** Esc detaches; `c` cancels the operation and releases its
  incomplete allocations. Closing the window is not cancellation.
- **Resident models:** lists remembered plans for the current household.
  `r` asks every pinned donor about the exact allocation/root. An unavailable
  donor leaves the plan unconfirmed; no automatic download or replacement runs.
  Enter reconnects to the pinned host and verifies its checkpoint again.
  `x`, followed by confirmation, releases the plan on its approved donors.
  Partial release failures stay visible rather than being reported as success.
- **/service:** shows status and can explicitly stop all local keepers.
- CLI: `lumabri service status --json`, `start`, `restart`, `stop`.

The resident library stores no conversation text, credentials or weights.
Multiple plans are separate records, not evidence of simultaneous capacity on
one computer. Each donor still admits one allocation; multiple active plans
need disjoint available donors. There is one preparation at a time per local
service. Shared-donor batching, automatic failover/replay, server-wide cost
optimization and model eviction policies are separate work.

## Next dependencies

The next milestone is **inventory and measurements usable by the planner**:
unify usable CPU/RAM and adapter-proven GPU/VRAM, per-model memory, links,
load, preparation and per-range performance, plus optional cost/energy facts.
Short probes seed observations and completed sessions update them; changed
execution conditions make them stale. See [the evidence contract](CALIBRATION_RECORDS.md#planner-resource-evidence)
for the implemented subset and remaining unknowns.

Shared-donor **multi-model management** follows this foundation: independent
allocation identities, summed reservations, per-model unload, isolated sessions
and restart reconciliation must work on the same donor before concurrent
capacity can be claimed. A separate **DeepSeek V4.1** integration must address
its cross-layer state and resident Engram tables; Colibri 1.12.1's standalone
engine is not an Edge/Segment adapter and cannot be aliased to DeepSeek V4.

## Tests

`tests/integration/household_service_test.py` covers singleton startup, private
permissions, stale commands, bounded partial frames, clean restart and crash
reconciliation. `test_chat_ui resident-plan` covers the versioned multi-plan
library, allocation identities, private files, symlink rejection and exact
household scoping.

```sh
python3 tests/integration/household_service_flow_test.py --models-dir /path/to/tiny-model-parent
```

This real-engine gate creates an isolated tracker and two donors, closes the
requester's TUI before approval, reattaches to a pending donor request, then
checks real generation after TUI exits and manager restarts. It asserts
unchanged keeper and engine identities, verifies the resident library against
both donors, rejects stale allocation/root release attempts, and tests explicit
release. Its processes, keys and homes are test-only. Loopback and tiny-model
coverage do not certify physical-LAN performance, large-model capacity or
native Windows support. Native macOS coverage is supplied by CI, not inferred
from the Linux result.
