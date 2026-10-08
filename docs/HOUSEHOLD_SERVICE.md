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
- **Explore models:** Space selects several models; `/prepare` reviews their
  joint placement before requesting separate donor approvals. Models are
  prepared sequentially. A rejection or cancellation keeps already-ready
  models loaded and reports the completed count. See
  [joint preparation](HOUSEHOLD_PLACEMENT.md#prepare-selected-models-from-the-tui).
- **Resident models:** lists remembered plans for the current household.
  `r` asks every pinned donor about the exact allocation/root. An unavailable
  donor leaves the plan unconfirmed; no automatic download or replacement runs.
  Enter reconnects to the pinned host and verifies its checkpoint again.
  `x`, followed by confirmation, releases the plan on its approved donors.
  Partial release failures stay visible rather than being reported as success.
- **/service:** shows status and can explicitly stop all local keepers.
- CLI: `lumabri service status --json`, `start`, `restart`, `stop`.

JSON `state`/`live` describe keeper reachability; `operation_state` reports the
validated live or journalled operation state (`running`, `done`, `failed`,
`stopped`, or `unknown` without a valid record). A nonresponding keeper is not
proof that preparation completed. Invalid records are never partially displayed.
The resident-model view refreshes saved plans while background work finishes;
saved status still requires an authenticated readiness check before use.

The resident library stores no conversation text, credentials or weights.
The [resident inference API](RESIDENT_API.md) can grant named local API users
access to exact approved allocations, without repeating donor approvals or
loading a new model. It currently binds only to loopback and stores no chat
history; it is not an OpenAI-compatible or public-network endpoint.
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

The current limits are four resident Segment models and up to eight approved
conversation slots per model. Weights are shared within a model; conversation
state is not. There is one active turn per model, with bounded FIFO admission
for waiting turns. Previous single-model tok/s are historical observations, not
a concurrent SLA. There is one preparation at a time per requesting local
service. The legacy foreground diagnostic donor remains single-allocation.

Reusing a conversation slot requires confirmed retirement of its remote
Segment sessions, even after the previous TCP connection has expired. CLOSE
reconnects to the same fenced allocation and retries BUSY or lost replies
within a short bound. An unconfirmed retirement cannot produce RESET_DONE
or silently open replacement sessions. It is reported as a failure; no
memory/session quota is raised to hide a leaked reservation.

## Compute admission across models

The background donor owns a private same-user compute broker. All its managed
Segment and Edge processes take a permit for each local kernel and release it
before waiting on another node. One kernel team executes at a time on that
donor; waiting kernels are FIFO, including when different resident models are
used together. This conservative policy prevents their independently sized
thread teams from running together. It is not CPU-time-weighted scheduling:
a long prefill kernel can still delay another model. No global latency or
throughput guarantee follows from a free conversation slot.

At most 32 connected permit requests are retained, including the active owner
and incomplete headers. An incomplete header expires after two seconds. Segment
admission uses the same configured wait budget across its per-engine and donor
queues (30 seconds by default); Edge admission waits at most 30 seconds and
observes hosted cancellation. An active kernel is never evicted merely because
its queue timeout has passed. Disconnecting or terminating its process releases
the permit. Broker failure fails closed; the keeper stops its allocations rather
than letting models bypass admission. Manager restart leaves the keeper and
broker alive.

`service status --json` exposes `compute.enabled`, `active`, `queued` and
`grants` for each donor. They are live observations, not restart authority.
Per-kernel counters do not trigger journal writes. Older service records remain
readable, but have no compute-admission observation. This is local same-user IPC,
not protection against an arbitrary program already running as that OS user.

Hybrid retains an explicit restriction: it cannot coexist with another resident
allocation on the same donor. Its coordinator may wait for remote experts
inside a local kernel; allowing overlapping Hybrid models to hold opposite
node permits would risk deadlock. Use Segment for concurrent resident models
until that path can yield admission around remote waits.

The API can visibly replay an interrupted turn on another already approved
replica, with a stable request seed and byte-for-byte prefix validation; see
[the recovery contract](RESIDENT_API.md#bounded-visible-response-recovery).
This does not restart failed keepers, prepare alternate allocations or migrate
KV state. Shared-donor batching, weighted/global compute scheduling, broader
fault recovery, server-wide cost optimization and eviction policies remain
separate work.

The terminal now consumes a renderer-independent incremental reply reader.
Each reply is bound to its submitted request ID; DATA payloads remain
byte-counted even when text contains apparent protocol headers. Only a complete
DONE frame commits conversation history or a speed observation. Engine ERROR,
truncation, invalid frames and consumer cancellation cannot publish success.
The terminal limits a reply to 64 MiB and telemetry lines to 4 MiB; unsupported
DATA sideband extensions are rejected. The [resident HTTP gateway and browser
chat](RESIDENT_API.md) use this same reader, with their own smaller response
limits. The gateway does not claim OpenAI compatibility.

## Measurements and management

Leased inventory reports usable CPU/RAM, runtime identity, load and optional
cost/power declarations. Completed TUI and API turns update provenance-bound
preparation, link and per-range observations; changed execution conditions
make them stale. The operator-only browser workspace reads those same reports,
saved allocations and managed replica references without opening engines.
See [the evidence contract](CALIBRATION_RECORDS.md#planner-resource-evidence)
and [the workspace API](RESIDENT_API.md#cluster-workspace) for boundaries.
Verified household GPU execution, requirement-aware global placement,
automatic capacity changes and provider provisioning remain separate work.
Approved host drain/resume now gates both TUI and API admission without
cancelling admitted turns; it retains weights and is not donor/server release.
See [host drain](RESIDENT_API.md#drain-an-approved-host-without-interrupting-admitted-turns).
Keepers also expose [Segment drain and live session counts](RESIDENT_API.md#inspect-and-drain-the-approved-segment-allocations)
through a private channel to each owned engine. KV between turns and queued
Hybrid calls remain counted. Coordinated retirement first seals host and nodes
and then releases that exact allocation. The resident library, local CLI and
explicitly authorized browser management use the same durable coordinator;
read-only operators and inference users cannot release resources. This is not
an automatic scaling policy; no idle observation alone authorizes releasing RAM.

### Planned extension: vendor-independent agentic orchestration (not implemented)

After the managed-service release baseline, an external, interchangeable agent
may make capacity decisions from queue lengths, observed latency, resource
pressure and costs. Codex, Claude Code or another decision system are possible
clients, not built-in dependencies or integrations claimed to exist today.
Colibri remains the inference engine; the controller agent need not run on it.
Agents will use the same authenticated, typed management API/CLI; an MCP
adapter may expose that contract without introducing separate permissions.
Actual client/tool support must be verified before integration. This extension
is not a new prerequisite for the current release.

The intended final mode is autonomous **within operator-authorized policy**,
not unrestricted shell or cloud access. Typed tools will request an evaluated
plan, prepare capacity on authorized machines, scale approved replicas, drain
them and release idle resources. The deterministic service must validate every
action against budgets, minimum/maximum capacity, permissions, live operation
revisions, cooldowns and active sessions. Provider credentials stay in the
executor, not in model prompts. Conversation text and untrusted worker labels
cannot grant permissions or become executable instructions.

Start with recorded-load evaluation and shadow decisions, then explicit
operator approval, then bounded automatic execution with an audit trail,
idempotent operations and an immediate off switch. Agent timeout, malformed
output, overload or unavailability must leave the normal controller running.
Reserve resources for this control path; inference saturation must not starve
the controller which would add capacity. It is never consulted per token.

Fast decisions do not make server startup or model preparation instantaneous.
Warm reserve, admission limits, measured preparation times, fault recovery and
draining are still necessary. Targets are measurable latency/availability and
cost objectives, not an unconditional promise of zero slowdown or outages.

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
python3 tests/integration/household_service_flow_test.py --models-dir /path/to/tiny-model-parent --keep-requester --multi-model --sessions 4
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

`test_compute_broker` covers FIFO admission, full queues, cancellation,
deadlines, malformed/partial requests, process death, private sockets and
fail-closed behavior. `test_chat_ui service-codec` checks record compatibility
and ensures kernel counters cannot cause durable-state rewrites. The mixed-model
flow additionally checks that real kernels obtained compute permits on both
donors and that releasing one model leaves the other chat usable.
