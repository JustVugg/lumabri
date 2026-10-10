# Model preparation from the service

The web **Computers & models → Prepare models** view and the management API
use the same resident Segment preparation keeper as the TUI. Closing the
page/TUI or restarting the manager/API does not cancel that keeper. This is
preparation on explicitly selected, authorized computers, not cloud provisioning
or automatic donor consent.

## Permissions and source

`lumabri api manage-grant NAME` enables preparation and allocation management.
It does **not** grant inference access or access to another user's history.
The owner configures the model folder in the existing household settings.
Clients select catalogue names; paths, executables, backend overrides, endpoint
addresses, keys and approvals are not accepted in requests.

All endpoints use the gateway's existing bearer authentication, loopback binding,
Origin checks and bounded request limits. Operator visibility alone permits
catalogue inspection, not preview/start/cancel or operation-history access.

## Workflow

1. `GET /api/v1/catalogue` lists discovered checkpoints, adapter, layer count,
   source byte count and whether validated source sizing is available. It does
   not invent a speed. `GET /api/v1/workspace` supplies the current node IDs.
2. `POST /api/v1/preparation` with `action: "preview"`, explicit `models` and
   `nodes`, `context`, `sessions`, and `max_new` returns a typed-plan projection
   and a `review` digest. Preview neither reserves memory nor sends offers.
3. Submit the same fields with `action: "start"`, that `review`, and a new
   random 64-character lowercase hexadecimal `operation` ID. A 202 response
   acknowledges the operation, **not** completed preparation. Donors must approve
   each model's signed offer. Only after preparation is the allocation usable.
4. `action: "status"` with `operation` observes the same keeper. `action:
   "cancel"` requests cleanup of incomplete preparation. Already READY models
   in a joint batch remain loaded. A cancellation acknowledgement is not proof
   that cleanup has already completed; continue observing.
5. `GET /api/v1/preparation` lists durable receipts for this household. Use
   status to check a recorded running operation against its live keeper.

Example preview body:

```json
{
  "action": "preview",
  "models": ["model-a", "model-b"],
  "nodes": ["<64-character node identity>"],
  "context": 512,
  "sessions": 2,
  "max_new": 128
}
```

The preview binds exact model names/source metadata, runtime/build and hardware
identities, current allocation sets, context/session limits and per-node layer
ranges/reservations. Changed plans require a new review. Source metadata is a
cheap change fence, **not** a signed weight hash: the existing indexer verifies
the actual checkpoint before donor approval. Do not edit checkpoints while
they are being prepared.

## Retry and failure semantics

An immutable operation intent is persisted before launching the keeper. Reusing
its ID with identical parameters only observes it, even after completion,
failure or API restart. Reusing the ID for different parameters returns 409.
No receipt replays donor approval or creates a replacement allocation.

Receipts are owner-private, bounded to 256 intents, and progress snapshots are
checksummed and atomically replaced. They do not persist raw C pointers or use
PIDs as authority. Damaged receipts fail closed. `interrupted_or_starting` means
that the last running snapshot cannot currently be confirmed through its exact
keeper incarnation; it is not permission for an automatic retry. Inspect
resident allocations before creating a different operation.

There is one preparation keeper per owner's service, shared by API and TUI.
Concurrent independent preparations are rejected. Joint preparation is sequential:
if model B is declined, already READY model A stays usable. A single-model
request uses the same joint planner with one model; Hybrid is not selected by
this interface. All inference in this path requires resident weights.

## Limits and verification

- At most eight models and 32 explicitly selected computers per request;
  1–8 sessions; context 1–131072 and output 1–16384 (not above context).
  These API bounds do not override tighter model or donor memory limits.
- Newly prepared allocations still require explicit inference grants or managed
  model registration through the existing service. Preparation is not a grant.
- The catalogue currently uses checkpoints present in the owner's folder; it
  does not download arbitrary model names from the Internet.
- Preparation does not prove GPU execution, sustained capacity, absence of OS
  swapping, or performance on a large model or a physical multi-machine LAN.

The integration gate uses two distinct reduced OLMoE checkpoint identities with
real Colibri inference, donor approval, duplicate start, changed-review rejection,
API restart, full completion, donor rejection and cancellation. The same gate
can drive the actual browser (`LUMABRI_TEST_BROWSER=1`). The original TUI joint
preparation tests remain in place as a regression gate.
