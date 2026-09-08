# Generation observations, version 1

Segment collects these observations during an ordinary completed chat turn.
It does not run an extra inference, reload a checkpoint or start a benchmark.

- `generated_tokens`: all selected output IDs, including a terminating EOS.
- `prefill_seconds`: generation start through selection of the first token.
- `decode_steps`: `generated_tokens - 1`; the first token came from prefill.
- `decode_seconds`: first-token selection through last-token selection. This
  includes intervening streaming/backpressure and recovery, if any, but excludes
  final text formatting and cleanup after selection of the last token.
- `total_seconds`: generation start through finalization.

Decode rate is `decode_steps / decode_seconds`. A one-token reply has no decode
rate. A client's first visible text can arrive later than first-token selection
(transport, buffering, incomplete UTF-8); the TUI labels that time separately.

The existing serve-codec `STAT` prefix remains readable by older clients. The
tail adds `PERF1 generated_tokens decode_steps prefill_seconds decode_seconds
total_seconds`. Non-finite, negative, inconsistent, truncated or unknown-version
observations do not produce a speed. Legacy engines without the extension keep
the previous display; their client-side timing is not an exact calibration.
`PERF_UNAVAILABLE` explicitly suppresses a speed when the engine could not
validate its clocks. This never discards a successfully completed reply or KV
history: measurement failure is not generation failure.

`segment_chat --json` adds a `generation_metrics` object with `version: 1` and
the same fields. Historical top-level `decode_tokens`/`decode_seconds` fields
remain for compatibility and retain their original convention; new consumers
should use the versioned object.

These are observations, not capacity promises. Persisting them against complete
checkpoint/build/hardware/plan keys, invalidating stale records, and using them
in the catalogue are separate unfinished parts of roadmap gate 4. The catalogue
must continue to say **not calibrated** until those requirements are met.
