# Elastic replicas on authorized machines

The API keeper can prepare extra resident Segment replicas for a logical
model and retire idle replicas. This is **opt-in resource management on
selected existing machines**, not cloud provisioning, automatic donor consent
or a guarantee of a particular token rate. The original model must already
have a registered logical ID and a known checkpoint content identity.

The same preparation keeper, memory admission, resident loading and donor
approval path used by the TUI executes each scale-up. A donor's one-time
approval is never replayed. An explicit, unexpired donor repeat policy may
authorize a new allocation within its own scope and allocation quota;
otherwise the donor receives a new request and must approve it.

## Configure

`GET /api/v1/elastic-policy` requires operator access. `POST` requires the
separate management permission. The local owner can use
`lumabri api elastic-policy [CONFIG_JSON_FILE] --tracker HOST:PORT`.
No configuration means no automatic preparation or unloading.

Example (replace the IDs with real IDs from the workspace/model registry):

```json
{
  "action": "configure",
  "id": "<logical model ID: 64 lowercase hex characters>",
  "revision": 0,
  "route_revision": 1,
  "enabled": true,
  "min_replicas": 1,
  "max_replicas": 2,
  "cooldown_seconds": 60,
  "idle_seconds": 300,
  "up_rejections": 3,
  "ttft_ms": 0,
  "max_reserved_bytes": "24000000000",
  "source_model": "olmoe",
  "nodes": ["<explicitly selected machine ID>"],
  "context": 512,
  "sessions": 2,
  "max_new": 128,
  "adopt_allocations": ["<existing allocation ID>"]
}
```

`revision` fences the policy; zero creates it. `route_revision` separately
fences the exact existing model route. `adopt_allocations` must list **all
current replicas in route order**: it explicitly authorizes managing their
lifecycle, including eventual retirement. Their nodes must be in `nodes`.
An existing replica shared through another logical model cannot be retired
by this policy until the other reference is removed.

`source_model` is an entry in the owner's configured model folder, not an
arbitrary path or download URL. Its indexed content must match the logical
model. A source metadata change blocks further preparation until reviewed.
The context and output limits cannot weaken the model's existing contract.
The session count must match adopted replicas. This controller prepares
resident Segment plans; it does not switch to disk or Hybrid implicitly.

Limits: at most 32 policies, 8 replicas per model, 32 selected nodes, a
30-second minimum cooldown, a 60-second minimum idle grace and one pending
lifecycle operation per policy. The common preparation keeper additionally
serializes preparations. Preparation and retirement receipts remain bounded
at 256 entries each; reaching a receipt limit holds new work for operator
inspection rather than forgetting an idempotency key.

## Demand and readiness

The keeper checks approximately every five seconds without blocking HTTP
requests. A resident minimum can trigger preparation. Recent gateway
capacity rejections trigger growth after `up_rejections`; optionally,
`ttft_ms > 0` enables a trigger when the p95 of at least four recent completed
turns exceeds that threshold. These are demand signals, **not certification
that another replica will meet the target**, particularly on shared CPUs.
The planner still has to find a feasible plan within the selected nodes.

Only this gateway's authorized requests are observed. Unknown telemetry or
a new gateway instance is not evidence of inactivity. Scale-down requires
a complete 60-second observation window followed by the configured idle
grace, no pending/admitted request and no new relevant request. Requests to
the model and its direct allocation IDs prevent an idle decision. Other
gateways and TUI connections are protected by the final live drain fences,
not inferred from this gateway's counters.

- **Always ready:** `min_replicas >= 1` maintains that resident target when
  preparation is possible and approved. Hardware failure or refusal can
  still prevent reaching it; inspect the status reason.
- **On demand:** `min_replicas = 0` permits withdrawal and retirement of the
  last idle replica. The logical ID, checkpoint/numeric contract and user
  grants remain. The catalog reports `dormant`.

A request to a dormant model returns HTTP 503 with
`model_dormant_preparation_required` and supplies the wake signal. The
controller prepares and publishes a replica; a later request can then run.
This cold wake does not wait for the optimization cooldown, which could
otherwise outlive the demand observation. The idle grace still prevents
repeated unloading immediately after activity.
There is no hidden unbounded cold-start queue or completed reply promised to
the initial request.

## Crash and concurrency behavior

Each model has a private, checksummed, atomically replaced lifecycle record.
Before the first offer it records a unique operation ID, exact review fence
and pending byte reservation. The shared preparation journal records the
allocation ID before any offer. A ready receipt is only historical evidence:
publication checks the actual approved plan and live numeric contract.

A route publication intent precedes the atomic registry write. After a
crash, reconciliation recognizes only the exact old or exact intended route.
External changes stop the old controller; they are not silently overwritten.
An uncertain preparation is not restarted with another operation ID.
Pending allocations and uncertain retirements remain charged against the
policy budget. Machine-wide admission also accounts for other models and
preserves the system reserve.

Scale-down first withdraws the exact replica from the model's route, then
drains and seals the authenticated host and Segment nodes, and only then
releases their allocation. Existing connections/admitted work delay release.
Unreachable does not mean released. Closing a TUI is not required for other
replicas or other models to continue running.

Disable with:

```json
{"action":"disable","id":"<logical model ID>","revision":1}
```

Disabling stops new decisions and unpublished changes; it does not erase an
allocation or operation already owned by the policy. An already published
withdrawal continues its safe retirement. A prepared but unpublished
allocation is retained for the operator. Reconfiguration cannot discard a
pending operation or silently abandon previously owned allocations. Inspect
`phase`, `pending_operation`, `pending_allocation`, `reserved_bytes` and
`reason` when an operation is uncertain. A policy must not compete with the
resident-portfolio controller for the same model; configuration rejects an
already active overlapping portfolio policy.
The reverse configuration order is rejected too; both writers share the
same configuration guard.

To resume a disabled, non-faulted policy without replacing its pending
operation, send `{"action":"resume","id":"<model ID>","revision":2}`.
Resume never clears a fault or recreates an uncertain preparation.

For a pending preparation, `{"action":"reconcile","id":"<model ID>","revision":2}`
checks the exact receipt while excluding both another submission and a live
preparation keeper. A completed result resumes observation with the same
operation ID. A known operation with no assigned allocation can be cleared;
an assigned result can be cleared only after its exact retirement is confirmed.
Unreachable workers or an RPC timeout are not proof of release. If those
proofs are absent, reconciliation refuses and keeps the reservation visible.

## Verification

Pure C tests cover the strict policy schema, budgets, cooldown, stale/lost
telemetry, observer restart, scale-to-zero decisions, exact publication
reconciliation and private intent files:

```sh
make test-api
make test_chat_ui
./test_chat_ui elastic-json
```

The full real-engine fixture additionally tests demand-driven preparation,
donor approval, publication, inference on the new replica, another independent
model, API restart, drain with an admitted TUI generation waiting for the real
donor compute permit, release after that generation completes, a dormant model
waking with the same ID/grants, and a manual-route-change fence:

```sh
LUMABRI_RESIDENCY_POLICY=engine-locked \
python3 tests/integration/household_service_flow_test.py \
  --models-dir /path/to/tiny-olmoe-models \
  --runtime-dir /path/to/complete-runtime/bin \
  --joint complete --api-prepare --donor-policy --donor-ram-gb 1.5 \
  --sessions 2 --elastic
```

The lifecycle gate explicitly uses owned CPU allocation locking, including
the reload and cold-wake engines. It checks their effective lock evidence,
not only the parent environment. An unavailable OS locking allowance fails
preparation; it does not disable the residency guard or fall back to observation.
The ordinary `observe` policy remains separately tested and unchanged. See
[resident contracts and limits](RESIDENT_RUNTIME.md).

This is a loopback correctness test using real OLMoE inference, not evidence
of physical LAN performance, datacenter capacity, billing savings or native
platform release readiness.
