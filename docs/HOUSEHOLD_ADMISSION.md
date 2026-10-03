# Household admission and independent chats

The default background donor supports up to four independently approved
allocations on a computer, within the summed RAM and preparation budget.
Engines and mutable weight mirrors are separate; releasing one allocation does
not release another. See [Household service](HOUSEHOLD_SERVICE.md). No computer
is added without selection and owner approval. Each hosted model currently
admits one chat at a time; memory capacity is not a concurrent throughput SLA.

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

There is no shared-donor multi-session engine scheduling, continuous batching,
per-session speed guarantee, capacity certification or automatic replay after
a participating donor fails. Those remain separate release gates. A donor's
advertised RAM alone is not proof that it will admit another request; admission
is checked again when the request reaches it.
