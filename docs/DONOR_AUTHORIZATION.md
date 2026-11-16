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

## From the page on the same computer

`lumabri start` shares this computer's memory without the **Share resources**
screen. The page it opens shows the waiting request instead: the requester's
identity, the model, the layer range, the memory and the limits, with
**Decline**, **Accept** and, for a resident Segment request, **Allow repeat
loads for 1 hour**. They are the TUI's three actions, sent to the same sharing
service with the same fences: its incarnation, the immutable allocation and the
request revision that were displayed. A request that changed or expired is
refused and shown again. Nothing is preselected, no key confirms, and an
unanswered request is dropped after two minutes exactly as before. After a
repeat permission was saved from the page, the page offers **Revoke repeat
permission** while that allocation is the one shown.

```
GET  /api/v1/donor/approval
POST /api/v1/donor/approval  {"action","instance","allocation","revision"}
```

`action` is `accept`, `decline`, `allow_repeat` or `revoke_repeat`. Both
methods need the explicit management permission; an operator or inference
token receives 403. The route acts only on the sharing service of the computer
and OS user that run this gateway: a request carries no address or computer
name. It is refused when the gateway is published through a TLS proxy
(`lumabri api start --https-origin`), so the page of another computer cannot
decide for this one. The page does not read requests while a response is being
generated; use the TUI or the CLI meanwhile.

The loopback gate `tests/integration/web_approval_test.py` prepares a real
reduced checkpoint through the page's API, declines once and checks that
nothing was loaded, then allows, accepts and revokes, with real inference.

## Standing consent for this computer's household

A computer's owner can let it accept its own household's requests without a
person clicking. It is one switch per computer, off unless stated otherwise
below, and it replaces the click and nothing else. With it on, a request is
taken through the same acceptance as a click, so it is still refused when it
does not come from an authenticated member of this household, when the
requester's identity does not match its pinned key, when it does not fit the
memory this computer shares or the memory that is free after the reserve,
when the residency policy cannot be honoured, or when the checkpoint fails
its integrity check. The household planner reads the same budget and refuses
a plan that does not fit before it is sent.

- **Where it is kept.** One owner-only file (mode 0600) in this OS user's
  private service directory, `~/.lumabri/service/standing-consent`. It is
  bound to one household, by its address and a digest of its key: another
  household, or a later one at the same address, does not inherit it. A
  missing, damaged or unsafe file means "ask each time".
- **Who can change it.** Only this computer: `lumabri consent on|off`, the
  **/settings** screen of the terminal workspace, or the switch under
  **Computers & models** on this computer's own page with the management
  permission. No household message carries it, and the page route is refused
  through a TLS proxy, so a manager on another computer cannot turn it on.
- **Default.** `lumabri start` turns it on when it creates a household on a
  fresh installation, and says so in its output together with how to turn it
  off. If that computer later joins another household from the terminal
  workspace and its owner never touched the switch, it stays on for the
  household joined, and the workspace says so. An installation that already
  had a household is never changed: it stays off until its owner turns it on.
- **Turning it off** holds from the next request. Nothing that is loaded is
  unloaded. Turning it on while a request is waiting accepts that request.
- **What it accepted** is recorded: time, requester identity, model, layers
  and memory, in `lumabri consent status`, on the page under the switch and
  as `[standing-consent] accepted …` in the donor log.

```sh
lumabri consent status [--json]
lumabri consent on
lumabri consent off
```

```
GET  /api/v1/donor/consent
POST /api/v1/donor/consent  {"enabled": true|false}
```

Both methods need the management permission and act only on the computer and
OS user that run the gateway. `GET /api/v1/donor/approval` reports the switch
as `automatic_consent`.

Standing consent is deliberately wider than a bounded repeat permission: it
covers any model, layer range and requester of the household, for as long as
it is on. Anyone who holds the household key and a paired computer can
therefore make this computer load a model that fits its shared memory. Leave
it off on a computer whose owner wants to review each load.

The loopback gate `tests/integration/standing_consent_test.py` starts fresh
households, loads a generated checkpoint with no approval, checks the record,
refuses every caller but the local manager, fills the share, turns the switch
off and checks that the next request waits while the loaded model stays.
Windows donors do not have this switch yet and ask each time.

## Local administration

Only the donor's OS owner can create these grants. An API management credential
on a different computer cannot authorize that donor on its owner's behalf; the
page route above is served on loopback by the owner's own gateway.

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
