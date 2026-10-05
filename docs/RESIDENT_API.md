# Resident inference API

The local gateway exposes **already approved resident allocations**. It does
not download a model, approve a donor, change placement or evict weights.
The TUI and gateway use the same authenticated allocation checks, hosted
connection, chat templates and incremental reply decoder.

This first endpoint is loopback-only (`127.0.0.1`). It is **not an OpenAI API**
and must not be exposed through a public port forward. Remote TLS deployment,
a managed gateway lifetime and the web/private-history interface are separate
work. Stopping this gateway cancels its requests, but does not unload models
or stop conversations belonging to other clients.

## Operator setup

Prepare models through the household TUI and obtain every donor's approval.
The same OS account that prepared them can then run:

```sh
./lumabri api list
./lumabri api user-add alice
./lumabri api grant alice ALLOCATION_ID_FROM_LIST
./lumabri api serve
```

The configured household is reused. `list`, `grant` and `serve` accept
`--tracker HOST:PORT`; with a different explicit tracker, provide its household
credential using the existing `LUMABRI_TOKEN` mechanism. `serve --port N`
changes the default port 47380, not the loopback binding.

`user-add` prints a bearer token **once**. Keep it private. Only its SHA-512
digest is saved, in owner-only files under `~/.lumabri/api-access`. Grants name
exact allocation identities, not model names or a wildcard. Re-preparing a
model creates a new allocation and requires a new grant.

`./lumabri api revoke alice` revokes future requests. It does not retroactively
cancel an already authenticated request; those are bounded to five minutes.
Deleting a user does not delete or unload the resident model.

## HTTP contract, version 1

All endpoints require `Authorization: Bearer TOKEN`. Model identities are
64-character allocation hex strings from `api list`.

`GET /api/v1/models` returns only the caller's granted plans:

```json
{"schema":1,"models":[{"id":"ALLOCATION_ID","name":"MODEL_NAME","context":4096,"max_tokens":256,"sessions":4,"state":"saved_plan"}]}
```

`saved_plan` deliberately does not claim that a worker is still online. Before
each inference, the gateway checks every donor's live allocation/root and
authenticates the expected Edge identity and checkpoint. Client text is never
sent to an arbitrary URL or replacement host named in an HTTP request.

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
supplies its own history; the gateway stores no transcript. Each HTTP request
gets an isolated hosted conversation, even for the same API user. This avoids
cross-client KV sharing but replays the supplied history on each request.

Successful admission returns `text/event-stream`. Events are:

```text
event: delta
data: {"bytes":"SGVsbG8="}

event: done
data: {"stats":"STAT ..."}
```

`bytes` is base64, preserving exact engine output even when a DATA frame splits
a UTF-8 character. Decode each delta to bytes and feed **one streaming UTF-8
decoder** per response. `done` is emitted only after a matching engine DONE.
An engine ERROR produces `event: error` with a message, never `done`. A closed
connection without `done` is an incomplete response, not a completed answer.
Disconnecting/aborting the HTTP request closes only its hosted conversation.
There is no automatic generation retry that could duplicate visible tokens.

Before streaming, errors use JSON `{"error":"CODE"}` and HTTP status:

| Status | Meaning |
| --- | --- |
| 400 | Unsupported/malformed request, roles, framing or limits |
| 401 | Missing, invalid or revoked bearer token |
| 403 | Unapproved allocation or rejected browser Origin/Host |
| 404 | Granted allocation has no saved resident plan |
| 408 | Incomplete request body exceeded its receive deadline |
| 429 | User already has two in-flight requests |
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
exercises the actual listener, credentials, quotas and disconnect cleanup.
The real two-model gate adds API users while a second model stays in its TUI:

```sh
python3 tests/integration/household_service_flow_test.py \
  --models-dir build/home-flow-models --keep-requester \
  --multi-model --sessions 4 --api
```

This uses real inference on tiny OLMoE checkpoints over loopback. It does not
certify a large-model LAN, datacenter workload or remote browser deployment.
