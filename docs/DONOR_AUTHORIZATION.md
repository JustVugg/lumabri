# Bounded donor authorization

One accepted request still authorizes **one immutable allocation**. It does not
give the requester permission to load another model or reuse an old approval.

For unattended reloading, the donor's local owner can explicitly authorize a
bounded repeat placement. This permission allows matching loads; automatic
placement additionally requires an explicit [replica policy](ELASTIC_REPLICAS.md).
The permission itself creates neither a replica nor a cloud server or charge.

## From the TUI

In **Share resources**, a pending resident Segment request has a third action:
**Allow repeat loads for 1 hour**. The default remains **Decline**. The action
allows one matching allocation at a time, with the displayed memory, context,
session, thread and output limits. Resize to at least 60 × 34 to see this option.

The permission is restricted to that requester, verified checkpoint content,
layer range, Edge placement and household. A different model, range or requester
still requires a new approval. The donor always checks its remaining resources;
permission is not a promise that RAM will be available later.

Press **r** while inspecting the allocation to revoke repeat permission. This
does not evict resident weights or interrupt a conversation. **x** unloads the
selected allocation; the permission can allow another matching load until it
expires. Stopping sharing ends the permission's scope: a new donor incarnation
requires new authorization. Closing the TUI or restarting only the manager does
not change the donor incarnation.

## Local administration

Only the donor's OS owner can create these grants. An API management credential
on a different computer cannot authorize that donor on its owner's behalf.

```sh
lumabri donor-policy inspect
lumabri donor-policy allow-current REVIEW 3600 1
lumabri donor-policy list
lumabri donor-policy revoke POLICY_ID REVISION
```

`inspect` returns a review token and the exact proposed scope. `allow-current`
revalidates that review against the live donor. Lifetimes range from 30 seconds
to seven days; the CLI can allow 1–4 simultaneous matching allocations. Total
matching reserved memory cannot exceed the per-allocation limit times this
count. Existing machine-wide memory and compute admission remain mandatory.

Grants are private, checksummed and atomically saved with revision fences.
Revocation, expiry or revision changes are checked again before COMMIT starts
preparation. Already committed work is not revoked retrospectively. A failed
or unconfirmed save must be inspected, not assumed successful.

## Identity and evidence

Allocation routing names change on each load, and their signed roots include
those names. Repeat authorization therefore reconstructs the stable checkpoint
identity from the source's signed per-file chunk hashes, verifies the complete
routing root, and retains the original routing root for engine verification.
No weights are fetched for this check. Missing, malformed, unsigned, oversized
or timed-out metadata cannot authorize an automatic load.

The focused gate uses two tiny OLMoE checkpoint variants and real Segment/Edge
engines. It covers CLI and TUI grants, retirement, a fresh automatic allocation,
inference after reload, a pending over-quota offer and revocation without eviction.
It is a loopback test, not evidence of production LAN throughput or autoscaling.
