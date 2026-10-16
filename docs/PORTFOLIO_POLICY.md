# Bounded resident-portfolio policy

The permanent API service can choose between **whole measured portfolios** of
already approved resident allocations. It is off until the administrator
explicitly configures it. It does not prepare or release weights, replay donor
approvals, buy servers or change a running conversation's route.

This is one component of the global planner, not its completion. New placements
use the separate [joint resident search](HOUSEHOLD_PLACEMENT.md), preparation
uses the [shared service operation](MODEL_PREPARATION_API.md), and approved
capacity can follow the [elastic lifecycle](ELASTIC_REPLICAS.md). These do not
turn this routing policy into a requirements-aware placement optimizer or
demonstrate billed savings. A measured static-versus-managed cost comparison
remains a release requirement. The reduced-model comparison below verifies the
connected workflow with declared prices; it is not a datacenter savings study.

## Prepare comparable evidence

Prepare and approve every candidate first. Measure the complete model mix with
`api capacity-measure`, using the same order of model contents, numeric
contracts, context/session limits, client count and prompt/output workload.
Measure after all co-resident allocations are in place; adding another model
invalidates earlier contention evidence. Use at least three rounds.

Give the logical model names one replica each, matching the declared current
portfolio. The policy changes those destinations atomically, keeping model
IDs, user grants, private histories and public context/output limits. It does
not synthesize candidate portfolios from independently measured model speeds.

## Configure through the shared service operation

`GET /api/v1/portfolio-policy` requires operator or management permission.
`POST` to that endpoint requires **management** permission and a JSON body.
Inference grants and read-only operators cannot enable automation. The local
owner can use the same operation:

```sh
lumabri api portfolio-policy policy.json --tracker HOST:PORT
lumabri api portfolio-policy --tracker HOST:PORT
```

Run `lumabri api start --tracker HOST:PORT` to keep the controller and API
active independently of the TUI. Stopping the API stops policy evaluation;
it does not unload resident models. `enabled` is saved authority, not proof
that the service is currently running: inspect `last_check` and service status.

Example input (replace the IDs, revisions and observation names):

```json
{
  "action": "configure",
  "revision": 0,
  "enabled": true,
  "records": ["normal", "economy"],
  "current": 0,
  "models": [
    {"id": "<64 hexadecimal characters>", "revision": 1},
    {"id": "<64 hexadecimal characters>", "revision": 1}
  ],
  "ttft_ms": 1000,
  "gap_ms": 200,
  "max_age_seconds": 300,
  "cooldown_seconds": 60,
  "horizon_seconds": 3600,
  "min_saving_bps": 1000,
  "currency": "EUR",
  "ceiling_micro_per_hour": "2000000",
  "switch_cost_micro": "1000"
}
```

`revision` is the expected policy configuration revision: zero creates, later
writes require the current value. Each model separately carries its expected
route revision. `current` is a zero-based index into the ordered candidates.
There are 1–8 models and 2–8 candidate observations. Input is limited to 4096
bytes; unknown or duplicate fields are rejected. Money is an integer decimal
string in millionths of the declared currency. `min_saving_bps: 1000` means a
10% minimum footprint reduction; it is not a fee or a performance estimate.

To disable, use the same endpoint or CLI file with only:

```json
{"action":"disable","revision":1}
```

Configuration authority persists until disabled or explicitly replaced; it is
not tied to an open browser/TUI. Revoking the credential that created a policy
does not implicitly remove this saved service configuration. Inspect/disable
it as a separate administrative action.

## Decision and failure behavior

The API keeper checks at most every 30 seconds, in a separate bounded child
so inventory queries do not block the HTTP accept loop. Only one policy
decision/configuration operation can hold the private lock at once. The
worker has a 90-second I/O deadline and a 120-second process lifetime.

The controller requires fresh matching capacity evidence and known machine
prices in the configured currency. Missing/mixed prices are not zero. Among
passing candidates inside the ceiling it prefers the lowest declared machine
footprint, retaining the current candidate on a tie. A cost-driven change must
also satisfy the saving threshold and recover the declared switch cost over
the horizon. The cooldown applies to restoration as well as cost changes.
If the current candidate no longer passes and another does, it can select that
approved candidate after cooldown. No passing candidate means **no change**,
not an invented capacity estimate or unauthorized shutdown of existing work.

Observation records are pinned by full digest, not only their names. Deleting
or replacing one holds automation until explicit reconfiguration. Expired
measurements or missing live reports cannot authorize a change. Measure fresh
comparable portfolios and replace the configuration to renew evidence. This
version does not turn ongoing single-model observations into joint guarantees.

Before publication it rechecks live evidence, the selected declared price and
every route revision. A durable intent records hashes of the exact old and
intended route records. After restart, an entirely old snapshot means nothing
was published; an entirely intended snapshot means acknowledge the completed
change. Mixed or externally modified records hold automation for the owner;
the controller never overwrites a manual revision. A lost response/fsync is
not interpreted as rollback. Re-read status before retrying configuration.

The status exposes configured/enabled/faulted state, pending target, current
candidate, model revisions, evidence digests, last check/change and reason.
The private API log records route changes without conversation text. Status
does not assert that all hosts are healthy between live checks.

## Cost and performance boundary

The price ceiling applies to the **selected declared machine footprint**, not
an account spending limit. This controller leaves all approved allocations in
RAM. Merely directing requests elsewhere does not turn off an old machine or
produce billed savings. Its output explicitly says
`declared_machine_footprint_not_realized_savings`.

Capacity evidence is a bounded observed workload, not a production latency
guarantee. TTFT and token-gap limits are checked against those observations;
ordinary host admission still applies to every future request. A policy cannot
prevent unrelated machine contention, network faults or OS memory pressure.

## Verification

`make test-api` covers policy parsing, limits, unknown prices, currency, integer
overflow, hysteresis/cooldown, durable state and exact interrupted-publication
reconciliation. The policy test is included in the sanitizer target.

The real-engine integration runs two independent reduced OLMoE checkpoints and
an independently approved replica on the same donors. With explicit test
prices it drains the current host, observes policy rerouting, generates from
both models, restarts the service without duplicate changes, then verifies a
manual revision stops automation. Test declarations are not measured invoices.

```sh
python3 tests/integration/household_service_flow_test.py \
  --models-dir MODELS --keep-requester --multi-model --sessions 4 \
  --api --replicas --portfolio-policy
```

### Static versus managed placement gate

This separate test prepares two checkpoint identities with real reduced OLMoE
inference. The joint planner first places both on donor A. The test then
explicitly approves a static alternative: one model on B, the other on A.
Both portfolios coexist before measuring either, so their observations bind
the same current contention and allocation inventory. Donor prices are fixture
declarations (A: EUR 0.10/hour, B: EUR 0.30/hour), not provider measurements.

```sh
LUMABRI_RESIDENCY_POLICY=engine-locked \
python3 tests/integration/household_service_flow_test.py \
  --models-dir MODELS --joint complete --api-prepare \
  --donor-ram-gb 2 --sessions 2 --placement-comparison
```

The gate records four-client overload outcomes separately from the two-client
comparison. The gateway currently admits one active turn per allocation;
two approved engine conversation slots do not mean two simultaneous gateway
turns. BUSY responses remain in the denominator and cannot become passing
capacity evidence. The comparison uses three rounds, eight output tokens and
explicit 30-second fixture TTFT/gap limits, not production SLOs.

It checks lower declared footprint selection, automatic whole-portfolio route
publication, actual authenticated chat on both models, and unchanged reservations
after routing. After explicitly disabling this experiment's policy, it retires
only the obsolete static allocations. B must have zero allocations/reservations;
both current models must remain on A. Retirement changes the workload identity,
so old evidence must become obsolete and a new real measurement must pass.

Each run emits `placement-comparison.json` in the printed artifact directory.
It includes per-model TTFT/token-gap/completion tails, completed/rejected/failed
counts, declared prices, selection, policy publication and post-retirement
remeasurement. CI retains this JSON for Linux and native Intel/ARM macOS with
and without OpenMP, without uploading access tokens or donor keys.

The October 16 local run passed all six requests in each two-client comparison.
The static portfolio's TTFT p95 was 87/90 ms for its two models; the co-located
portfolio's was 88/109 ms. Their declared selected-machine footprints were
EUR 0.40/hour and EUR 0.10/hour respectively. Both four-client probes reported
six completions and six BUSY responses across three rounds. All eight prepared
engines proved effective owned CPU allocation locks. This is a workflow result
on tiny checkpoints and loopback, **not** a performance claim for full-size
models or physical machines. The unused machine remains running; no invoice
saving or server shutdown is claimed.
