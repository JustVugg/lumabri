# Household admission and independent chats

The default background donor supports up to four independently approved
allocations on a computer, within the summed RAM and preparation budget.
Engines and mutable weight mirrors are separate; releasing one allocation does
not release another. See [Household service](HOUSEHOLD_SERVICE.md). No computer
is added without selection and owner approval. A hosted model defaults to one
conversation slot; up to eight separately budgeted slots may be approved.
Memory capacity is not a concurrent throughput SLA.

## Shared weights, separate conversations

Choose **Conversation slots per new model** in workspace settings (1–8), or
use `lumabri models --sessions N`. This changes a new plan, never an already
approved allocation. Each donor sees the requested slots and summed memory
before accepting. Segment session state, Edge conversation history and sampler
state are isolated. Engine weights are shared within that model's allocation.
Hybrid remains a single-slot option until its multi-slot contract is verified.

One turn runs at a time per hosted model. Other admitted conversations enter a
visible FIFO queue: at most `slots - 1` waiting turns, one outstanding turn per
connection, a default 30-second queue deadline. `host --queue-ms N` configures
that bound (1–300000 ms). Each generation has the existing absolute request
deadline and output/context limits. There is no speculative batching or hidden
promise of parallel token generation. A connection beyond the approved slot
count receives **BUSY**; a turn beyond its queue deadline receives an explicit
error and closes only that conversation.

Clients cannot select internal codec slots. A disconnected/cancelled turn is
drained and its slot is reset before reuse, without unloading weights or
resetting other conversations. A broken shared engine terminates the model's
host rather than passing stale output to another client. Cancel cannot preempt
a kernel already executing; the drain/reset grace is bounded to 30 seconds.
Queued requests have bounded buffers; oversized frames are rejected before
allocation and partial frames have an absolute two-second deadline.

The host remains bound to the requester identity that the donor approved.
Multiple windows of that owner can use the slots. This is **not** delegation to
other users: the later authenticated user/API layer must supply that authority.

`test_host_session_pool` injects deterministic queue timeout, cancellation,
disconnect and oversized-frame faults using a deliberately mocked codec.
The real-model gate is separate:

```sh
make test_hosted_sessions
python3 tests/integration/household_service_flow_test.py \
  --models-dir /path/to/tiny-model-parent --keep-requester --sessions 8
```

The gate opens all approved slots against real OLMoE, checks the extra client's
BUSY response, compares independent/interleaved greedy output with an isolated
turn, then disconnects and reuses slots. Its JSONL records report each client's
time to first **token notification** (including queueing), p50/p95 notification
gaps and engine decode rate separately. Notification timing is not guaranteed
to equal displayed text timing: detokenization can hold back incomplete UTF-8.

The older foreground diagnostic (`LUMABRI_HOME_FOREGROUND=1`) is exclusive per
participating computer and weight-cache directory. The following BUSY and
disjoint-group test exercises that diagnostic lifecycle, not the capacity of
the default background donor.

An authenticated offer for an occupied foreground donor receives a rejected status with
`BUSY` and its own request ID. The admitted transaction, connection, resource
leases and engines are unchanged. This applies while the owner is deciding,
while weights are being prepared, and during an active chat. An unrelated
preflight or offer must not reset the owner's current Accept/Decline choice.

The requester reads the initial allocation acknowledgement before sending
heartbeats. A donor refusing an offer may close after sending its status;
writing first could hide that queued reason behind a connection error.
Malformed or missing acknowledgements stop preparation and release any
allocations already obtained. An unapproved allocation never starts a model.

## Executable gate

```sh
python3 tests/integration/home_flow_test.py \
  --models-dir /path/to/tiny-model-parent --expect-disjoint-plans --context 256
```

The test uses an encrypted tracker, two actual donor TUIs and two separate
Hosted/Segment sessions. It checks:

- explicit rejection before approval and during an active allocation;
- an owner's selected Accept action surviving another client's request;
- both approved sessions and both sets of engine/cache leases coexisting;
- both chats producing new generation results, not stale catalogue timings;
- closing the first chat without releasing the second chat's resources;
- another generation on the surviving chat, then complete lease cleanup;
- real Segment execution on both donors and no checkpoint copy on the clients.

The same gate runs against installed Linux and macOS candidates. Loopback
tests do not certify physical LAN performance. A tiny checkpoint is a protocol
fixture, not evidence of useful language quality or large-model throughput.

## Not claimed

There is no global cross-model compute scheduler, continuous batching,
per-session speed guarantee, capacity certification or automatic replay after
a participating donor fails. Those remain separate release gates. A donor's
advertised RAM alone is not proof that it will admit another request; admission
is checked again when the request reaches it.
