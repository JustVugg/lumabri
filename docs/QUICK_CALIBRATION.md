# Optional short calibration

In the model catalogue, select participating computers and a model, open
`/ actions`, choose `/calibrate`, and review the confirmation. Enter requests
the plan; Esc returns without indexing, offers or execution. Every used donor
must still approve its exact allocation, including the eight-token turn limit.

The probe uses the same real Hosted/Segment path as chat, a fresh short prompt,
and at most eight generated tokens. The local client starts a monotonic
20-second deadline before submitting the prompt. On expiration it shuts down
the Hosted socket, wakes the blocked stream and releases the household plan.
No interrupted, invalid or insufficient measurement is saved. Ctrl-C cancels.

The inference deadline does **not** include discovery, indexing, transfer or
loading weights. Those use the existing preparation flow and cancellation.
An unloaded large model cannot be measured without preparing it. Reused
verified weights avoid unnecessary transfer but still require engine loading.

After the completed probe, Lumabri saves the same versioned real-timing record
as a normal chat turn and releases its engines/leases. The catalogue displays
the speed only for a matching checkpoint, binaries, donor runtime identities,
hardware, selected ranges, thread counts, context and session count. Its model
detail shows the actual prompt/generated-token sample sizes. Failed or expired
probes never overwrite an earlier valid record with a fabricated speed.

The result is a **short-run observation**. It does not predict long-context
performance, sustained thermals, other background load or concurrent capacity.
It is not a microbenchmark extrapolated from a different model. Stage-level
cost fitting and placement informed by those costs remain separate work.

## Gates

- Timer tests cover invalid requests, double start, normal stop with the socket
  still usable, expiry waking a stalled read and descriptor ownership.
- `home_flow_test.py --expect-quick-calibration` reviews and dismisses the
  confirmation, checks no loading before consent, approves two real Segment
  donors, executes exactly one short generation, checks the saved record and
  reopened catalogue, and verifies resource/cache lease release.
- The same flow runs on installed Linux and macOS candidates. Tiny fixtures
  validate execution and protocol, not useful language quality or large-model
  speed. Physical LAN acceptance remains a separate test.
