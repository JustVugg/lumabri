# Observing a Segment chain without another benchmark

Every completed `segment_chat` turn collects client-side elapsed time around
each range's RUN call. No extra token or network request is generated.
Prefill calls/rows and decode calls are separate, even for one-row prefill.
The first generated token comes from prefill and is not a decode call.

The standalone `--json` result contains `stage_observations` version 1.
Hosted writes the same object as a `[segment-stage] request=N` line in the
Edge engine log. Hosted also emits optional `STAGES1` fields before `PERF1`
in DONE/STAT. Each logged range carries its exclusive layer end, lease identifier,
route generation, fencing epoch, call counts, accumulated seconds and
minimum/maximum decode-call duration. No conversation or activation bytes
are logged. Counters reset on every turn, including a turn that reuses KV.
The engine log preserves long lines verbatim instead of inserting newlines
at each 511-byte display chunk; the interactive diagnostic tail stays bounded.

The scope is **client_run_round_trip**, not remote kernel time: it includes
serialization, network transfer, queueing, remote compute and any transparent
transport retries. It excludes embedding, token selection, OPEN/loading,
snapshot transfer and text streaming. The sum of decode range times is thus
only part of the complete decode interval. It is not the throughput of a
filled multi-session pipeline. Min/max are not percentiles.

A failed RUN or invalid timer invalidates the whole profile. Successful
recovery may still produce a complete reply, but its range profile is
`valid:false, stages:[]`: replay and old/new owners must not be conflated.
The end-to-end generation measurement continues to include recovery costs.

The bounded wire extension carries only ranges, decode counts and accumulated
seconds. The requester stores per-RUN averages only for a complete chain
matching the approved ranges and decode step count. Old hosts remain usable
without stage costs. One-token replies supply no decode cost. These historical
averages can guide a new split under unchanged execution conditions, but the
new split must be measured before showing its speed.

Tests cover bounded accumulation, bad times and overflow; the standalone
whole/split oracle validates per-range counts against generated tokens and
times against the full decode interval. The real two-donor Hosted test checks
that reported ranges match the approved plan. A loopback test remains a
loopback test, not a physical-LAN speed claim.
