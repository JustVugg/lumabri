# Resident inference API

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
overwriting an intervening change. The ordered set is an operator preference,
**not** a measured speed, cost or load-balancing optimizer.

For each new chat request, the gateway rechecks the live allocation and host,
then tries the next approved member if the previous one cannot be opened.
This happens **before submitting any prompt**. Once submitted, an error or
disconnect ends that response: there is no hidden automatic generation retry
and no KV migration. A new user-initiated request can replay its own text
history on another member. If no approved replica is available, HTTP 503 is
returned; the gateway does not reload a model or replay old donor approvals.

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
data: {"stats":"STAT ...","observation_saved":true}
```

`bytes` is base64, preserving exact engine output even when a DATA frame splits
a UTF-8 character. Decode each delta to bytes and feed **one streaming UTF-8
decoder** per response. `done` is emitted only after a matching engine DONE.
An engine ERROR produces `event: error` with a message, never `done`. A closed
connection without `done` is an incomplete response, not a completed answer.
Disconnecting/aborting the HTTP request closes only its hosted conversation.
There is no automatic generation retry that could duplicate visible tokens.

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
| 429 | User already has two in-flight requests, or approved replicas are occupied |
| 503 | Approved allocation unavailable, host busy, or submit failure |

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
