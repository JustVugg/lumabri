# Gateway demand observations

An operator can inspect `GET /api/v1/demand` with the same bearer credential
and explicit operator grant used for `GET /api/v1/workspace`. The web
**Computers & models** view includes these observations. Inference permission
alone does not expose other models' activity.

The observer counts authorized, syntactically valid model requests through
**this gateway**. Direct TUI/Hosted requests, other gateways, unauthenticated
traffic, local CLI calibration probes and per-user rejections before the request body is read are not
attributed to a model. A logical model ID and a direct allocation ID are
separate targets; their counts must not be added to machine-level active counts
as if they were independent users.

Per target it reports:

- **opening**: model admission/connection is still being attempted, not a queue;
- **admitted**: a host conversation was acquired and has not finished cleanup;
- starts, admissions and terminal outcomes in the current 60 one-second buckets;
- completed, capacity-rejected, failed, cancelled, invalid and interrupted turns;
- first-visible-text and cleanup-inclusive completion p50/p95 for at most the
  last 64 successful turns still in that window, with sample counts. Missing
  timing is `null`, not zero. Percentiles use nearest rank.

Starts and completions are independently windowed: a long turn can finish in a
different window from the one where it began. The counters are observations,
not sustainable capacity, CPU/GPU utilization, a tok/s prediction or an SLO.
Latency for failed turns is not mixed into successful-turn percentiles; failures
remain visible separately. Per-response exact token-notification gaps remain
available in the chat completion event.

## Freshness and limits

Snapshots carry the gateway instance, age and `window_complete`. A new gateway
starts an empty, incomplete window; restarting only the manager preserves the
still-running gateway's instance. Stale snapshots and telemetry gaps do not
become evidence of idle capacity. A live worker with missing events keeps the
window incomplete even if it lives longer than 60 seconds. Abrupt worker exit
clears its live counters and records an interruption when the model is known.

The parent owns bounded counters for 64 model/allocation IDs and 16 HTTP workers.
Idle model records can be reused only after their window expires. Exhausted
telemetry space marks evidence incomplete; it never evicts active counters.
`telemetry_gaps` and `connection_limit_rejections` are cumulative for this gateway
instance, unlike the per-model window counters. Connection-limit rejections
occur before authentication, so they cannot be attributed to a model.

Workers send three small private datagrams per admitted turn, not per token.
They do not block inference waiting for telemetry. The parent processes them
on its existing event loop. No transcript, prompt, username, token or checkpoint
bytes enter this observer; no statistics files are written. The window is
deliberately not restored after gateway restart.

This supplies traffic evidence for an elastic policy. It does not itself add
or remove replicas, authorize donor work, or provision servers.
