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

- **Share resources:** Esc detaches; Tab cycles the loaded/requested models;
  `x` unloads only the currently displayed allocation;
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
The background donor keeper can retain up to four independently approved models
on the same machine, provided their summed reservations fit its sharing budget
and current memory permits preparation. Pending loads remain reserved before
their full RSS appears. The reporter subtracts all live reservations from the
offered budget; admission rechecks it, so stale inventory never grants memory.
Overflow and capacity exhaustion fail closed. Allocations share one machine-wide
ownership lease, but have separate engine processes, KV/session state, allocation
IDs and mutable preparation mirrors; content-addressed immutable blocks may be shared.
Releasing one model or losing its requester cannot release another allocation.
Only stopping the sharing service unloads all its models. Manager restart does
not replay approvals or create duplicate reservations.

This is memory-safe coexistence, not guaranteed per-model performance. Models
share CPU time through the OS; their independent engines may compete when chats
overlap. Previous single-model tok/s are historical observations, not a concurrent
SLA. The current maximum is four models and one hosted chat per model. There is
one preparation at a time per requesting local service. The legacy foreground
diagnostic donor remains single-allocation. Shared-donor batching, fair global
compute scheduling, automatic failover/replay, server-wide cost
optimization and model eviction policies are separate work.

## Next dependencies

The next milestone is **inventory and measurements usable by the planner**:
unify usable CPU/RAM and adapter-proven GPU/VRAM, per-model memory, links,
load, preparation and per-range performance, plus optional cost/energy facts.
Short probes seed observations and completed sessions update them; changed
execution conditions make them stale. See [the evidence contract](CALIBRATION_RECORDS.md#planner-resource-evidence)
for the implemented subset and remaining unknowns.

The [DeepSeek V4.1 integration](DEEPSEEK_V41.md) adds a Lumabri-owned text CPU
adapter with resident Engram tables and per-conversation cross-layer feedback.
It uses the same approval and service lifecycle, without aliasing V4.1 to V4.

## Tests

`tests/integration/household_service_test.py` covers singleton startup, private
permissions, stale commands, bounded partial frames, clean restart and crash
reconciliation. `test_chat_ui resident-plan` covers the versioned multi-plan
library, allocation identities, private files, symlink rejection and exact
household scoping.

```sh
python3 tests/integration/household_service_flow_test.py --models-dir /path/to/tiny-model-parent
python3 tests/integration/household_service_flow_test.py --models-dir /path/to/tiny-model-parent --keep-requester --multi-model
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
