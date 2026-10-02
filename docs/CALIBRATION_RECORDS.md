# Speeds from real household chats

There is no extra model download, synthetic prompt or long benchmark. A
completed household Segment turn supplies prefill and decode timing. Once it
has at least two generated tokens and complete execution identities, Lumabri
saves the latest observation in `~/.lumabri/calibrations/` on the requesting
computer. Reopening the catalogue and selecting the same donors can show
`12.50 tok/s (last)`. This example is illustrative, not a performance claim.

The detail view includes the observed prompt and output token counts. This is
historical evidence for that plan, not a guarantee for a different prompt,
longer conversation, changing network conditions or other machine load. The
configured context limit is part of the key; the actual prompt length is
recorded separately. One-token replies, legacy timing frames and incomplete
numeric/runtime metadata cannot create a speed record.

## When a number disappears

Checkpoint content, adapter/numeric ABI, executable binaries, donor identity,
hardware, addresses, threads, ordered layer ranges, Edge placement, execution
mode, context limit and session count are bound to the observation. Changing
the selected computers immediately invalidates the displayed plan while a
background refresh runs. A mismatching record says **stale** and exposes no
numerical speed. A missing or unreadable record says **not calibrated**.

Each Share resources instance creates a new runtime epoch. Stopping and
restarting it invalidates previous observations even if its binaries have not
changed. This conservatively avoids reusing a result from a different launch
environment. Restart sharing after updating runtimes or dynamic libraries;
this is not live attestation of arbitrary library changes or a dishonest host.
GPU discovery never substitutes for an actual execution backend.

The runtime fingerprint hashes the installed Lumabri, Segment node, Segment
chat and shim binaries, plus an optional bundled OpenMP library. Replacing or
removing that library invalidates the identity. Unchanged hashes are cached after stat checks. Model
content is identified from the source's existing hash sidecars, without
reading weight payloads again. Before recording, that content is checked
against the signed routing root of the approved session. Per-request routing
names therefore do not prevent reuse of the same checkpoint's observations.
Missing or stale sidecars withhold the measurement; they do not weaken the
signed model checks or prevent an otherwise valid chat.

Upgrade the tracker, requester and donors together: machine inventory is now
version 4. Version 3 reports remain readable, but their missing load/cost
fields are unknown. Older versions are rejected explicitly rather than treated
as a complete calibration identity. The Hosted greeting has optional numeric
ABI metadata. An older host without metadata can still serve chat,
but cannot produce these records.

## Storage and limits

The format stores conditions, numerical measurements and token counts, not
prompts, replies, token IDs or credentials. Records contain at most 32 nodes and 32 KiB.
Integers and binary64 values have an explicit little-endian representation;
C structure padding, locale and host endianness are not part of the format.

Each private directory holds one latest record per checkpoint. A temporary
file is written and synced before atomic replacement. A failed load clears
the output rather than leaving an earlier record available by accident.
Truncated, malformed, non-finite and checksum-invalid records are rejected,
as are unsafe names, symlinks, FIFOs, hardlinked files and non-private paths.
The checksum detects corruption; it does not attest a remote host's work.

Keys require all identity and execution fields; commit provenance is optional
but exact binary identities are required. Empty keys, unterminated
strings, unknown thread counts and empty ranges cannot establish a match.
Changing any matched condition makes a previous speed stale. The producer
verifies where each field came from: structural validation alone would not
make invented build IDs or a partial key authoritative.

New `LMB-CAL4` records store the latest observation's origin (`short_probe` or
`real_session`) as well as optional complete per-range decode RUN averages.
Completed turns update the last value and observation count under the exact
same key; a changed key starts a new count. Counts are not a statistical error
bound or concurrent-session capacity. `LMB-CAL1`, `LMB-CAL2` and `LMB-CAL3` remain readable
without inventing their unknown measurement origin or missing stage costs. The `STAGES1` extension
precedes `PERF1`; a malformed stage profile cannot influence placement, but
does not discard otherwise valid end-to-end timing.

The C TUI and JSON export consume the same owned snapshot. JSON returns
`calibration: null` when absent, and `decode_tok_s: null` when stale. This
feature provides historical per-stage averages for heuristic placement, not
a measured speed for a new plan or a quality ranking across model families.

## Planner resource evidence

The catalogue snapshot and `models --json` distinguish OS load, adapter-derived
memory estimates, runtime identity and actual observations. The JSON adds
`resource_facts` on nodes and `execution_evidence` on models. Per-range memory
separates weights, context/session state, scratch and reservation overhead.
Missing data is `null`; stale observations never populate per-range timings.
The TUI labels a short probe separately from a completed chat and shows load1
as an OS load average, not CPU percent.

Optional operator declarations are `LUMABRI_COST_PER_HOUR` (fixed decimal,
up to six fractional digits), `LUMABRI_COST_CURRENCY` (three uppercase letters),
and `LUMABRI_ESTIMATED_POWER_WATTS` (up to three fractional digits). Start the
sharing service with them set to advertise them. Missing or invalid values
stay unknown; explicit zero cost stays a declared zero. These are whole-machine
prices and estimated power, not billing receipts, incremental inference costs,
live power readings or measured energy savings. No cost-based scheduling is
enabled by publishing them.

The real Edge probes each direct Segment connection once when opening a session:
three 16-byte echoes measure application RTT (median and maximum), followed by
one 64 KiB echo. The effective byte rate counts both payload directions and
includes framing, encryption and scheduling; it is not NIC bandwidth, download
throughput, a p95 estimate or a promise for larger transfers. Requests are bounded
before allocation, authenticated through the same connection as inference, limited
to four echoes per connection, and never accept a target address. Each exchange
has a 250 ms I/O timeout. Failure or an older peer leaves the metric unknown and
reconnects before inference; it does not make chat depend on telemetry support.

`LINKS1` observations reach the typed snapshot through the real host stream and
`LMB-CAL4` records. Links belong to the exact Edge/range plan, not a tracker ping.
They expire after five minutes or a relevant key change. Changed/failed-over
routes cannot publish a partial old chain. The TUI and per-range JSON expose
only current observations. A new session remeasures them.

Preparation duration measures the requester's monotonic COMMIT-to-entire-chain-READY
interval: transfer, loading and host startup, excluding source indexing and owner
approval. It is saved in the private version-4 resident-plan record even when the
TUI detaches, and carried into subsequent calibrated turns. It describes that
preparation, not a prediction for a different cache state. Reopening a resident
chat updates existing matching records after validating identities and current
runtimes; old plans without content provenance do not fabricate observations.

Live energy sensors remain unknown. A tracker RTT, disk benchmark or detected
GPU cannot replace execution evidence. Household GPU execution stays
unavailable until a matching adapter backend and memory contract pass execution
tests. Thus this is the evidence foundation of the inventory milestone, not a
claim that its hardware validation or the multi-model scheduler is complete.

## Verification

Run `make test_calibration && ./test_calibration`. Native macOS workspace CI
runs the same record and key tests; Linux also runs them in the ordinary suite.
`make test_inventory` additionally validates runtime identity fields. Chat UI
tests cover numeric readiness metadata and immediate selection invalidation.

With a real small checkpoint, run:

```sh
python3 tests/integration/home_flow_test.py --models-dir /path/to/model-parent \
  --expect-metrics --expect-calibration --repeat-cached
```

This starts an isolated tracker and two donor TUIs, requires approval,
generates a real reply, reopens the catalogue and invalidates the speed after
context, selection and donor-runtime changes. It also verifies cached weights
still require new approval. Linux and native macOS CI run this loopback gate;
it is not a physical LAN throughput measurement.
