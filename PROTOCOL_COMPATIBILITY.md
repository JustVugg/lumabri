# Protocol and rolling-upgrade contract

Lumabri has one framed transport and several independently versioned payloads.
Changing one payload never permits silently reinterpreting another.

| surface | current marker/version | incompatible behavior |
|---|---:|---|
| frame envelope | `LMB1` | close before allocating an invalid frame |
| Expert manifest | `LEM2` | skip the executor; local/replica fallback |
| signed model identity | `MID1` | refuse to mix checkpoint roots |
| Segment request/state | `SEG2` / 2 | explicit unsupported/bad-request reply |
| Segment discovery | `SGD1` / 1 | publish no compatible route |
| Segment RUN compute (`LMB_SEG_COMPUTE_GET/PUT` 100-103) | optional `GSC1` / 1 sideband | old trackers return `ERR`; old nodes send no report; old chatters continue to use `SGD1` |
| Segment assignment | `GSA1` / 2 | release/retry assignment, never guess a range |
| Expert stats capability | `ECAP` / 1, `EST1` / 2 | fall back to legacy registration without stats |
| swarm execution/detail | `SWX1` / 1, detail 2 | omit unavailable telemetry, not compute identity |
| executor residency (`LMB_ERES` 70/71) | additive op | older node answers `ERR`; chatter treats residency as unknown, no penalty |
| machine inventory (`LMB_MACHINE_REPORT/LIST/LIST_R` 77/78/79) | body version 2, including signed donor control address | incompatible versions refuse; viewer clears remote inventory and invalidates proposed plans |
| household consent (`LMB_HOME_*` 80–85) | body version 1 | authenticated offer, immutable allocation, explicit donor approval and commit; no negotiation or execution fallback |
| household allocation-exit proof (`LMB_HOME_EXIT_PROOF/R` 104/105) | immutable `LMBLIF1` record | missing operation, missing record or unreachable peer means unknown, never released memory |
| hosted greeting | optional codec field `2` after limits | new hosts explicitly declare SUBMIT/DATA/DONE independently of model family; unsupported codecs fail closed |
| encoded expert call (`LMB_EXEC2` 72/73, caps word in `ERES_R`) | additive op | a chatter sends `EXEC2` only to a node whose `ERES_R` carries `LMB_CAP_EXEC2`; the tunnel keeps `EXEC`; bf16 only for values that are exactly bf16 |

The model root, engine ID, source/build profile, numeric class, state schema,
dtype and width are compatibility data rather than decoration. A peer with a
different value is another execution class and cannot enter an existing
session. Transport encryption authenticates bytes in flight but does not make
two numeric classes equivalent.

## Rolling upgrade

1. Add decoders before emitters and keep old payloads accepted where the
   operation remains unambiguous.
2. Deploy trackers first, then origins/replicas, then chatters, then donors.
3. A Segment placement remains fenced by lease and route generation for the
   life of the session. Move it only through snapshot/restore/replay.
4. Mark a node draining before restart; wait for inflight work or migrate it.
5. Never change a marker's meaning. Allocate a new version and keep the failure
   explicit when backward decoding is impossible.
6. Run the mixed-version fixture before removing a legacy decoder.

These are compatibility rules, not a claim that arbitrary installed releases
can roll live. The currently tested whole-household operation is a planned,
quiescent [package update and rollback](docs/INSTALLED_UPDATES.md), with
completed retirement, all services stopped and new approvals after restart.
The current version pair must read every retained local state format too.

The C structs may grow additively through `struct_size`; wire structs do not.
Runtime-only scheduler fields in route snapshots are intentionally excluded
from the discovery encoding, so a v1 tracker and newer chatter still agree on
the same signed placement fields.

Allocation-exit proof is an additive, read-only query to the authenticated
donor keeper, not a tracker operation or a shutdown request. Only the original
requester may query an exact allocation/checkpoint pair. A birth record must
have been persisted before the allocation started, with a lifetime descriptor
inherited by its owned processes. Updating the keeper cannot retrofit that
evidence onto an already running legacy allocation. Old peers and invalid
responses therefore retain the reservation; normal live drain/retirement
remains separate. See [release proof](docs/RESIDENT_API.md#coordinated-retirement-of-an-exact-approved-allocation).

The local retirement journal now writes `LMBRET2`, distinguishing a verified
process exit from an idle live-process seal. It reads `LMBRET1` without adding
exit evidence. A binary supporting only v1 cannot reconcile a v2 journal;
do not downgrade a live controller in place or remove its records to bypass
that check. Finish or explicitly account for outstanding operations using a
compatible controller before a rollback. No version change replays approval.

Machine inventory uses a separate bounded table (32 identities). Report bodies
contain version, peer public key, hostname/OS/architecture/CPU/ISA strings,
core/NUMA/GPU counters, RAM/VRAM/disk quantities and the proposed RAM budget.
The report appends an Ed25519 signature over a length-prefixed
`lumabri.machine.v1` domain, the connection's 32-byte challenge and the entire
report body. With encryption the signing key must match the transport peer.
One live connection owns each identity. Reports expire after 15 seconds and
are removed when their connection closes. LIST returns version, count and,
for each row, age in milliseconds followed by its report without signature.
The authenticated tracker is trusted to relay inventory; this is not a public
hardware attestation or permission to run a model.
