# Resident inference API

## Cluster workspace

The bundled browser has an operator-only **Computers & models** view. Explicitly
grant this read-only capability to an existing API credential:

```sh
lumabri api operator-grant operator-name
lumabri api operator-revoke operator-name
lumabri api workspace --tracker HOST:PORT
```

`GET /api/v1/session` reports the credential's visibility capability;
`GET /api/v1/workspace` requires it. This grants neither inference nor access to
another user's conversation history. Inference still needs a separate model
grant. Revoking/recreating a username does not inherit its old operator grant.
The local CLI can read the same typed snapshot without issuing an API token.

The snapshot includes leased machine reports, memory reservations and observed
queues, saved allocations and range assignments, managed replica references,
and configuration-bound measurements from real turns. A missing inventory or
registry is explicitly unavailable. A saved plan does not prove readiness;
actual hosts and donor approvals are checked when a chat is requested. Stale
calibrations have no current speed value. These are last matching observations,
not a concurrency or latency guarantee. The panel refreshes every ten seconds
only while visible and removes old values if a refresh fails.

Whole-machine prices and power estimates remain operator declarations. Missing
prices are not zero cost, currencies are not added together, energy is not
invented from those estimates, and GPU detection is not verified execution.
The view cannot start, stop, prepare or move models. No cloud resources or
paid provisioning are introduced.

## Inference gateway

The local gateway exposes **already approved resident allocations**. It does
not download a model, approve a donor, change placement or evict weights.
The TUI and gateway use the same authenticated allocation checks, hosted
connection, chat templates and incremental reply decoder.

This first endpoint is loopback-only (`127.0.0.1`). It is **not an OpenAI API**
and must not be exposed through a public port forward. Remote TLS deployment
is separate work. The bundled browser chat opens at `http://127.0.0.1:47380/`.
Stopping this gateway
cancels its requests, but does not unload models
or stop conversations belonging to other clients.

## Operator setup

Prepare models through the household TUI and obtain every donor's approval.
The same OS account that prepared them can then run:

```sh
./lumabri api list
./lumabri api user-add alice
./lumabri api grant alice ALLOCATION_ID_FROM_LIST
./lumabri api start
```

The configured household is reused. `list`, `grant`, `start` and `serve` accept
`--tracker HOST:PORT`; with a different explicit tracker, provide its household
credential using the existing `LUMABRI_TOKEN` mechanism. `start --port N`
changes the default port 47380, not the loopback binding.

`start` detaches an authenticated same-user keeper. Closing the terminal/TUI or
running `lumabri service restart` does not stop it or duplicate it. Repeating
`start` with the same household and port reuses the keeper; a different
configuration is rejected until explicitly stopped. `lumabri service status
--json` lists the `api` role (phase 1: starting, phase 2: listening).

`lumabri api stop` stops only the gateway and cancels its HTTP requests. Loaded
models and other clients remain active. `lumabri service stop` also stops the
gateway as part of stopping the household. For foreground diagnostics,
`lumabri api serve` remains available without registering a keeper.

A crashed keeper is reported as interrupted, not silently healthy. Run `api
start` to replace it; a journal PID is never used to kill or adopt a process,
and old donor approvals are never replayed. A port conflict is a startup
failure, not a second listening instance. Logs are in the private
`~/.lumabri/service/api.log` file. Boot-time autostart is not installed.

`user-add` prints a bearer token **once**. Keep it private. Only its SHA-512
digest is saved, in owner-only files under `~/.lumabri/api-access`. Grants name
exact allocation or managed-model identities, not names or a wildcard.
Re-preparing a raw allocation requires a new grant. A managed identity can
retain its grant across explicitly approved replica changes, as below.

`./lumabri api revoke alice` revokes future requests. It does not retroactively
cancel an already authenticated request; those are bounded to five minutes.
Automatic recovery rechecks the credential and grant before resubmitting text;
a revocation prevents that new disclosure even within an existing request.
Deleting a user does not delete or unload the resident model.

## Stable model identities and approved replicas

An operator can expose several independently approved allocations of the
**same checkpoint** under one stable identity:

```sh
./lumabri api model-add olmoe APPROVED_ALLOCATION_A APPROVED_ALLOCATION_B
./lumabri api grant alice MODEL_ID_FROM_MODEL_ADD
./lumabri api model-set MODEL_ID_FROM_MODEL_ADD APPROVED_ALLOCATION_B
./lumabri api model-remove MODEL_ID_FROM_MODEL_ADD
```

These commands accept `--tracker HOST:PORT`. They never load or unload weights,
create a server, or replace a donor's approval. `model-add` and `model-set`
authenticate all members and query the running hosts before atomically saving
the set. Each member must have the same source content identity, adapter,
numeric ABI/class and greedy policy. Routing roots may differ: the exact
allocation, signed root and host identity are stored separately for each
member. Changing checkpoint or numeric semantics requires a new model ID and
new user grants. Registered plans must have current provenance (version 6).

Up to 32 managed models and eight replicas per model are supported. Names are
1–32 lowercase letters, digits, `_` or `-`. Private revisioned records reside
under `~/.lumabri/api-access/models`; concurrent updates fail rather than
overwriting an intervening change. The default preference is the operator's
order. It can be changed without loading, moving or unloading any weights:

```sh
./lumabri api model-policy MODEL_ID ordered
./lumabri api model-policy MODEL_ID observed-decode
./lumabri api model-policy MODEL_ID declared-cost
```

`observed-decode` prefers the faster **last observed decode**, only when all
available approved plans have comparable observations: exact build, hardware,
threads, resident allocation set, ranges, context and configured sessions;
matching prompt/output lengths; at least eight generated tokens; no older than
five minutes. A five-percent preference band retains the requested order for
near-equal rates. It does not predict TTFT for a different prompt or certify
concurrent capacity. Missing, changed or incomparable observations restore
the operator's order, rather than manufacturing an estimate.

`declared-cost` prefers the lower sum of declared whole-machine hourly prices
for the replica's participating computers (each counted once). Every candidate
must have known prices in the same currency. This is a resource-footprint
preference, **not incremental cost or a saving**: the other replicas are still
resident and their computers still incur costs. It has no performance SLO.
Missing/mixed-currency prices restore the operator's order. Gateway logs state
which preference and evidence were used; normal users cannot change policy.

Completed turns retain up to sixteen exact-configuration observations per
checkpoint in private fixed-size slots, at most 512 KiB plus a 32 KiB temporary
record. New configurations replace the oldest slot; an older delayed writer
cannot replace a newer same-key observation. Only timing, counts and machine
provenance are stored, never prompt or answer text. The existing latest-record
file remains readable. Old version-one routes load with `ordered`; saving a
route now uses version two, which older binaries do not understand.

For each new chat request, the gateway rechecks the live allocation and host,
then tries the next approved member if the previous one cannot be opened.
This initial selection happens **before submitting any prompt**. If no approved
replica is available, HTTP 503 is returned, or HTTP 429 `replicas_busy` when
at least one usable candidate refuses admission as BUSY. The gateway does not
reload a model or replay old donor approvals.

### Drain an approved host without interrupting admitted turns

The local operator can control an exact saved allocation:

```sh
./lumabri api replica ALLOCATION_ID status
./lumabri api replica ALLOCATION_ID drain
./lumabri api replica ALLOCATION_ID resume
```

Use `--tracker HOST:PORT` when not using the configured household. Each command
returns a versioned JSON observation: host instance/revision, `accepting`,
`draining` or `drained`, connected conversations and admitted requests. A drain
acknowledgement is not completion: poll status until `drained` if completion is
needed. Queries do not open an inference session or touch the model weights.

Only the original approved requester identity can control the host, over the
encrypted connection and with the exact signed allocation root. A shared
household token alone is insufficient. Updates compare the observed process
instance and revision; a concurrent change fails instead of overwriting it.
The wire protocol permits retry of a lost mutation reply at the same fence.
Unreachable, old or incompatible hosts are **unknown**, never assumed idle.

Drain is serialized with actual host admission, so it applies to both direct
TUI clients and API aliases. It refuses new connections with BUSY, closes idle
conversations and rejects turns not yet fully received/admitted. Already
admitted active or queued turns retain their normal cancellation/deadline
rules and can finish; draining does not cancel them. After those clients leave,
status is `drained`. Resume admits new conversations on the same resident
engine; prior conversation state is reset before reuse. Other model hosts on
the same donor are unaffected. Both one-slot and multi-slot Segment hosts
use this pool; legacy non-Segment hosts do not support this control.
Single-slot hosts reset conversation state when the client disconnects, as
before; multi-slot hosts reset the isolated slot under the turn gate on reuse.
Hosted one-slot codecs advertise their slot and handle cancellation like the
multi-slot codec. Standalone single-slot CLI pipes keep their existing EOF
semantics. Older single-slot engines without that control capability are
drained/reset on disconnect instead of receiving an unsupported CANCEL.
An authenticated BUSY response (including drain or cancellation cleanup) stays
HTTP 429 `replicas_busy` at the API boundary; it is not reported as an unavailable
allocation. A pinned-identity or checkpoint mismatch remains a failure, not BUSY.

This is a host admission primitive, **not automatic scale-down**. It does not
send donor RELEASE, reclaim RAM, stop a server or certify that unrelated direct
Segment clients are idle. No HTTP mutation permission is implied by the
read-only workspace operator grant. The node controls below, a coordinated
retirement operation, policy/cooldown/minimum capacity decisions and provider
lifecycle are separate requirements before automatic resource release. State
is scoped to the running host instance, not a persistent policy replayed after
a replacement process.

### Inspect and drain the approved Segment allocations

```sh
lumabri api segments ALLOCATION_ID status --tracker HOUSEHOLD:PORT
lumabri api segments ALLOCATION_ID drain --tracker HOUSEHOLD:PORT
lumabri api segments ALLOCATION_ID resume --tracker HOUSEHOLD:PORT
```

These local operator commands contact each allocation keeper, pinned to the
saved peer identity and checkpoint root. The keeper additionally checks the
original requester's identity and allocation ID. The household token alone
cannot inspect or control somebody else's allocation. The keeper forwards the
bounded operation over a private inherited socket to its own Segment child;
the Segment network listener still permits only the approved Edge identity.

Each node reports its live process instance, revision, resident session count
and admitted Hybrid expert requests, including queued ones. `drained` requires
explicit drain plus zero sessions and zero admitted expert requests. A retained
KV between two RUNs is **not** idle capacity. Slow session creation returns an
unknown/busy result instead of assuming zero use. Unsupported runtimes, stale
instances, failed reads and lost channels are also unknown, never drained.

Drain rejects new Segment OPENs and new Hybrid expert calls. Existing session
retries, RUN/CLOSE and previously admitted expert work keep their usual limits;
drain itself does not cancel a kernel. Resume uses the same resident engine.
`accepting` describes this manual admission policy, not a capacity or health
certificate: memory pressure, residency loss and the existing compute/session
limits can still refuse work. Resume never clears those independent guards.
For a whole replica, first drain its host and wait for admitted turns to finish,
then drain its segments. Do not drain segments first: a newly admitted turn may
need to OPEN or rebuild its state. Multi-slot hosts may retain idle KV until
slot reuse or session expiry, so host `drained` alone is not segment `drained`.

The command reports each node independently. A nonzero exit means at least one
state is unknown or changed concurrently; mutations already accepted elsewhere
remain in effect and are not silently rolled back. Revision fences prevent an
old mutation from crossing a newer drain/resume or engine replacement.

This control **does not release weights or stop a server**. Explicit owner
unload still exists separately and can interrupt work. Safe automatic release
needs a coordinated, fenced retirement operation and the model's authorized
minimum/maximum/cooldown policy; those are not implemented by these commands.

### Bounded, visible response recovery

For managed models with multiple approved replicas, a lost codec connection or
an explicitly typed `LMB_REPLICA_UNAVAILABLE` engine error can replay the request
on a different member. At most three allocations are attempted within the
existing five-minute request deadline. A `recovering` SSE event makes the
interruption visible. There is **no KV migration**: the prompt/history are
reprocessed, so recovery adds latency and computation.

All attempts use one request-private sampling seed on runtimes advertising
`LUMABRI_REQUEST_SEED 1`. Independently of that seed, every byte of the already
delivered prefix must match before any continuation is published. Matched bytes
are suppressed. A divergent or shorter replay ends with `error`, never `done`;
the browser retains an interrupted conversation for explicit user action.
Frame boundaries and partial UTF-8 characters do not reset this comparison.
The temporary prefix is bounded to 8 MiB and is not telemetry or calibration.

Every retry revalidates donor approval, signed root, content identity, numeric
ABI/class, host identity and the current API credential/grant. The replica must
belong to both the original request's set and the current operator-approved set.
Newly added replicas are not adopted mid-response. Removed/revoked members,
busy allocations and previously attempted allocations are excluded. A browser
disconnect, cancellation, malformed stream or ordinary engine error does not
trigger another attempt. Exhausted capacity preserves the visible partial
answer and reports an error; it does not prepare new resources.

An older runtime without the seed capability can participate in automatic
recovery only for an explicitly greedy model. Raw allocation requests have no
alternative set and do not recover automatically. This recovery loop is in the
API/browser path; direct hosted TUI conversations retain their existing behavior.
Update clients and household hosts together: older clients reject the new
sampling-capability bit rather than silently misinterpret it. Updated clients
can still use older hosts without the new seeded-SUBMIT field. Colibri is not
modified; seed negotiation lives in Lumabri's codec and Segment gateway.

### Bounded turn admission

The gateway admits one API turn per exact resident allocation at a time,
across users, aliases, raw-allocation grants and gateway processes using the
same access directory. A busy replica is skipped in favour of another approved
member, instead of filling the first host's internal conversation queue while
another replica is idle. If none can be admitted because capacity is held,
the request receives HTTP 429 `replicas_busy` before submission. There is no
additional implicit API queue or automatic resend.

Admission is owned by kernel file locks and is released after local stream
cleanup or worker-process death. No journal PID or stale counter recreates a
reservation. The private `api-access/dispatch` directory has at most 32 fixed
lease cells plus its lock; allocation churn does not grow it without bound.
Partial, unlocked cell records after a crash are only replaceable hints.
Unsafe files fail closed. Each admission attempt waits at most 100 ms for the
dispatch bookkeeping lock before reporting contention.

This is a gateway limit, not a claim that a replica is idle in the whole
cluster: direct TUI chats, another operator and the host's cancellation cleanup
still share its compute. Host admission, turn deadlines and the node-wide FIFO
remain authoritative. Per-user and global gateway limits are unchanged; this
does not certify a tok/s target or add new resident capacity.

The browser shows the managed name and uses its stable ID for private history.
A managed grant does not authorize direct access to its raw allocation IDs.
Deleting the route leaves weights and existing private conversation records
untouched; it prevents further inference under that ID.

## HTTP contract, version 1

All `/api/v1/` endpoints require `Authorization: Bearer TOKEN`. The four
compiled static web assets are public, but expose no account or model data.
Model identities are
64-character allocation or managed-model hex strings from `api list`.

`GET /api/v1/models` returns only the caller's granted plans:

```json
{"schema":1,"models":[{"id":"ALLOCATION_ID","name":"MODEL_NAME","context":4096,"max_tokens":256,"sessions":4,"state":"saved_plan"}]}
```

`saved_plan` deliberately does not claim that a worker is still online. Before
each inference, the gateway checks every donor's live allocation/root and
authenticates the expected Edge identity and checkpoint. Client text is never
sent to an arbitrary URL or replacement host named in an HTTP request.
Managed records use `state: "saved_route"`, `replicas` and `revision` instead;
`sessions: null` deliberately avoids claiming live or aggregate capacity.
Their advertised context and output limit are the minimum of their members.
Operator `api list` also includes content/numeric metadata and allocation IDs;
the user-facing catalogue omits that operator-only detail.

`POST /api/v1/chat`, with `Content-Type: application/json` and a bounded
`Content-Length`, accepts:

```json
{
  "model": "ALLOCATION_ID",
  "max_tokens": 128,
  "messages": [
    {"role":"user","content":"Hello"},
    {"role":"assistant","content":"Hi"},
    {"role":"user","content":"Continue"}
  ]
}
```

Messages must alternate user/assistant, starting and ending with user. System,
tool and multimodal messages are not supported in this version. The caller
supplies its own history; this inference endpoint does not itself store a
transcript. The separate private-history endpoints below provide persistence.
Each HTTP request
gets an isolated hosted conversation, even for the same API user. This avoids
cross-client KV sharing but replays the supplied history on each request.

Successful admission returns `text/event-stream`. Events are:

```text
event: delta
data: {"bytes":"SGVsbG8="}

event: done
data: {"stats":"STAT ...","observation_saved":true,"recovery_attempts":0,"stats_scope":"single_attempt"}
```

`bytes` is base64, preserving exact engine output even when a DATA frame splits
a UTF-8 character. Decode each delta to bytes and feed **one streaming UTF-8
decoder** per response. `done` is emitted only after a matching engine DONE.
An engine ERROR produces `event: error` with a message, never `done`. A closed
connection without `done` is an incomplete response, not a completed answer.
Disconnecting/aborting the HTTP request closes only its hosted conversation.
During recovery, `event: recovering` carries a human-readable `message` before
the next attempt. Keep the same streaming UTF-8 decoder: already emitted bytes
will not be sent twice. Successful recovery reports `recovery_attempts > 0`,
`stats_scope: "final_attempt_only"` and `observation_saved: false`. Those engine
statistics exclude earlier attempts and must not be presented as end-to-end
recovery speed. Recovered turns never overwrite a no-replay calibration.

Completed API/browser turns use the same observation builder as TUI turns.
The planner can reuse their decode timings, actual prompt/output counts,
per-range timings, links and preparation evidence only under the exact
checkpoint/build/runtime/context/session/workload key. API time to first text
includes gateway admission and host setup; decode speed comes from the engine's
versioned timing, never from network chunk size or byte counts. This is an
observation of that workload, not a concurrency or SLO guarantee.

`observation_saved` is false when provenance or timings are missing, the
runtime/workload changes during the turn, the output cannot measure decode,
or the private record cannot be saved. This does not turn a completed response
into an error. ERROR, cancellation and truncated streams never update speed.
API observation checks use the currently leased inventory without waiting for
a future heartbeat; stale/missing information remains unmeasured. No extra
calibration prompt is generated. The store retains the latest observation per
checkpoint, not an aggregate across different replica plans.

Before streaming, errors use JSON `{"error":"CODE"}` and HTTP status:

| Status | Meaning |
| --- | --- |
| 400 | Unsupported/malformed request, roles, framing or limits |
| 401 | Missing, invalid or revoked bearer token |
| 403 | Unapproved allocation or rejected browser Origin/Host |
| 404 | Granted allocation has no saved resident plan |
| 408 | Incomplete request body exceeded its receive deadline |
| 409 | History revision conflict, occupied history lock or storage quota |
| 429 | User already has two in-flight requests, or approved replicas are occupied/draining |
| 503 | Approved allocation unavailable or submit failure |

## Bounds and isolation

- Sixteen simultaneous gateway request processes; at saturation new sockets
  are closed without claiming admission. Two in-flight requests per user,
  enforced across gateway processes using the same access directory.
- 8 KiB headers, 1 MiB request body, 129 messages, 64 KiB per message,
  8 MiB generated output; `max_tokens` cannot exceed the approved plan.
- Five-second receive/write timeouts and a hard five-minute request lifetime.
  The hard deadline may close the stream without an error event.
- One request per HTTP connection. No chunked request bodies, pipelining,
  half-closed request connections, ambiguous duplicate framing headers,
  upgrades or `Expect: 100-continue`.
- Browser requests must use the same loopback Origin and Host. No permissive
  CORS, bearer tokens in query strings, or cookie-based implicit authority.
- User grants authorize inference only. They do not expose the operator's
  household key, donor control API, checkpoint paths or other users' histories.
- Host/Segment and machine-wide compute admission still apply. This gateway
  does not invent extra inference capacity or a guaranteed per-user speed.

## Verification

`make test-api` covers bounded JSON/Unicode, HTTP framing and origin rules,
private credential files, exact grants and revocation. `make test-api-gateway`
exercises the actual listener, credentials, quotas, disconnect cleanup and
private history, including revoked/recreated account isolation.
The real two-model gate adds API users while a second model stays in its TUI:

```sh
python3 tests/integration/household_service_flow_test.py \
  --models-dir build/home-flow-models --keep-requester \
  --multi-model --sessions 4 --api --replicas
```

This uses real inference on tiny OLMoE checkpoints over loopback. It does not
certify a large-model LAN, datacenter workload or remote browser deployment.

## Browser chat and private history

After `api start`, open the printed loopback URL and enter the token from
`user-add`. No separate web server, JavaScript build chain, external font or
CDN is involved: HTML, CSS, JavaScript and the repository logo are compiled
into the controller with a small native C build tool. The same binary remains
relocatable. The browser never saves the token or transcripts in localStorage,
sessionStorage or cookies; refreshing the page requires the token again.

Choose an approved resident model, write a question and send. The page shows
the user's message alongside the streaming reply, with Stop, New conversation,
collapsible history, JSON export and confirmed deletion. Model selection is
fixed within a conversation. The page sends at most 256 generated tokens per
turn, reduced to the approved model limit. Missing/unloaded allocations are
reported; the web page cannot authorize preparation or silently substitute a
different model. Replaying a longer conversation can exhaust the approved
context, in which case the request fails visibly; history is not trimmed.

History is saved on the gateway's computer, **not end-to-end encrypted from
the operator**. Its OS account and the inference host are trusted. Owner-only
directories/files isolate API users; each credential digest has a separate
namespace, so revoking and recreating a username never reveals old chats to
the new credential. Revocation does not erase old data: export/delete before
revocation if desired. Backups and OS-level disk encryption remain the
operator's responsibility; deletion is not a secure erase of SSDs or backups.

There are at most **32 conversations and 256 KiB per conversation** per
credential namespace (about 8 MiB, plus bounded metadata/one transaction
file). No automatic eviction occurs at quota. Each message is at most 64 KiB;
the browser stops a response exceeding this private-history limit even though
the inference endpoint allows a larger standalone output. Up to 65 questions
are retained. History is not a server-signed proof of what a model generated.

The browser saves the question before submitting inference. An answer becomes
`complete` only on a matching DONE and a successful save. Stop, errors,
disconnection or reload leave `pending`/`interrupted` visibly incomplete.
Retry is an explicit new generation from the last question, excluding any
partial assistant text. No hidden automatic replay occurs. If saving fails,
visible text can still be exported; the page does not claim it was persisted.
Concurrent edits use revision checks; an old tab cannot overwrite a newer
revision silently.

History endpoints:

- `GET /api/v1/conversations`: metadata only, never all transcript text.
- `POST /api/v1/conversations`: create with revision zero.
- `GET /api/v1/conversations/ID`: read one private conversation.
- `POST /api/v1/conversations/ID`: replace at the exact expected revision.
- `DELETE /api/v1/conversations/ID`: body `{"revision":N}`; stale revision
  returns 409. Unknown and another user's IDs both return 404.

Create/update bodies have this shape; unknown/duplicate fields are rejected:

```json
{"revision":0,"conversation":{"model":"ALLOCATION_ID","title":"A question","state":"pending","messages":[{"role":"user","content":"Hello"}]}}
```

The reply contains `id` (32 lower-case hex characters), new `revision`,
`updated` (server Unix seconds), and the saved `conversation`. States are
`idle` (no messages), `pending` (ending in user), `complete` (ending in
assistant) and `interrupted` (question or partial answer). Alternating roles,
valid UTF-8 and all size bounds are checked independently of the browser.
Escaped NUL bytes are preserved in archived assistant replies (a byte
tokenizer can emit them). They are never accepted in keys, metadata, user
prompts or the C-string inference interface. Such replies remain exportable;
the browser asks for a new conversation rather than silently removing the
byte when constructing the next prompt.
Creating/updating requires an exact model grant; reading/deleting existing
history does not require that the model still be available.

`tests/integration/web_chat_test.py` exercises actual Chromium rendering,
stream decoding, retry/Stop, private history, credential lifetime and mobile
layout using **explicit generation fixtures**. It saves screenshots under
`build/web-chat-artifacts`. The separate two-model OLMoE gate verifies real
engine bytes round-trip through the same history API without crossing users.
For the browser test only, install Playwright 1.62.0 and its Chromium build;
these are developer/CI dependencies, never runtime package requirements.
Set `LUMABRI_TEST_BROWSER=1` on the two-model service gate to also drive the
browser against **real Colibri generation**, save/reload the answer, and check
that the other API user cannot see it. The Linux CI enables this extra check;
native macOS runs the real API/history gate without a browser dependency.
