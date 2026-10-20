# Updating and rolling back an installed household

Keep the previous and candidate packages in **separate directories**. Never
unpack over a running installation or combine executables/libraries from
different packages. Verify their manifests and trusted release provenance.
A manifest proves file consistency, not publisher identity.

There are two distinct operations:

- **Gateway only:** stop and replace the API while compatible workers keep
  their weights. The [gateway gate](../PRODUCTION.md#installed-gateway-update-and-rollback)
  tests this narrower operation.
- **Whole household:** retire the resident allocations, stop every service,
  switch packages, then prepare and approve new allocations. This has planned
  downtime and reloads weights; it is not transparent rolling migration.

`service restart` replaces only the manager. It does **not** update live
donor, tracker, preparation, API or engine processes.

## Before changing versions

Test the exact version pair with the installed gate below. Both versions must
understand the retained local state and wire contracts. In particular, check
the atomic model registry, retirement receipts (`LMBRET2`), allocation lifetime
records (`LMBLIF1`) and any saved elastic policy (`LMBELA02`). An older binary
that does not read a format is **not a rollback target**. An unchanged Colibri
revision alone does not establish Lumabri state compatibility.

Plan a maintenance window. Stop accepting new external traffic and close
direct TUI/Segment chats. Inspect and pause any configured
[elastic](ELASTIC_REPLICAS.md) and [portfolio](PORTFOLIO_POLICY.md) automation
through their revision-fenced controls. Finish or explicitly reconcile their
pending preparations, withdrawals and retirements using the current version.
Pausing does not erase owned operations or their memory charges. Do not
proceed with unknown ownership, incomplete receipts or a live policy still
creating work.

Keep a private backup of configuration, identities, grants, private history
and control journals **after** orderly shutdown. It can contain credentials
and conversation text. A backup is disaster-recovery material, not permission
to roll the control ledger backward while workers exist. The verification
below intentionally never restores a snapshot or deletes a journal.

## Whole-household procedure

Use the currently installed package's launcher (`./start-lumabri` on Linux,
`./Lumabri.command` on macOS) until all its services have stopped.

1. Record managed model IDs and their exact allocation IDs in **Computers &
   models**. Drain/retire each intended allocation through the management
   controls, or run:

   ```sh
   ./start-lumabri api retire ALLOCATION_ID --tracker HOUSEHOLD:PORT
   ```

   Wait for `state: "released"` and `complete: true` for every participant.
   `waiting_for_admitted_work` is pending; inspect and repeat the **same**
   operation. A timeout, missing donor or `unknown_or_conflict` is not release.
   Do not remove records, force-free a reservation or change an allocation ID
   to bypass it. Respect any policy-owned pending operation before continuing.

2. On the requester, donors and household owner, stop the local services:

   ```sh
   ./start-lumabri service stop
   ./start-lumabri service status --json
   ```

   Require successful stop and no live/interrupted services. Stop the tracker
   owner last. This is an explicit shutdown, not just closing the TUI. If stop
   cannot confirm ownership has ended, do not launch another installation.

3. Start the new package from its own directory under the **same OS account**.
   Keep the existing private `.lumabri` directory and peer identity; do not
   create a fresh household as an update workaround. Open sharing on the
   selected donors. Check that no old allocation loads automatically.

4. Prepare the desired models again and explicitly approve every donor
   request. A former approval does not authorize a new allocation.
   Preserve a managed model's stable ID and grants by replacing its replicas
   only after the new compatible allocation is READY:

   ```sh
   ./start-lumabri api model-set MANAGED_MODEL_ID NEW_ALLOCATION_ID --tracker HOUSEHOLD:PORT
   ```

   Check model identity, limits and the resulting route revision. Raw
   allocation grants do not transfer automatically. Do not substitute a
   different checkpoint under a managed identity.

5. Verify each model generates, users cannot access each other's history,
   old retirement receipts remain readable and RAM reservations match the new
   approved allocations. Revalidate or remeasure performance observations
   after the runtime/build/placement change. Restore external traffic and
   explicitly reviewed policies only after these checks.

Rollback follows the **same complete retirement and shutdown procedure**,
then selects the separately retained compatible previous package. It uses
the current persistent state, not a stale copy. Never downgrade live workers
under an active fenced placement.

## Reproducible installed-package gate

From a test checkout, with two clean native packages and one existing tiny
OLMoE fixture:

```sh
python3 tests/integration/installed_fleet_update_test.py \
  --previous-package '/path/to/previous package' \
  --candidate-package '/path/to/candidate package' \
  --models-dir build/home-flow-models
```

The test creates four disposable local homes; it cannot select an existing
household. It uses only the packages' production launchers and binaries,
never a replacement engine or a copied development helper. It verifies:

- native, complete, clean-source packages with distinct revisions and gateway
  hashes, and the same pinned Colibri revision;
- previous → candidate → previous for **all services**, not just the gateway;
- two independently approved model identities on one donor at each stage;
- real replies through stable managed model IDs and unchanged user grants;
- private history, stale-write refusal, revoked credentials and cross-user
  access denial across both switches;
- authenticated completed retirements, no old approval replay and no reused
  allocation identity;
- preserved configuration, peer identities, routes, histories and completed
  preparation/retirement/lifetime records, without restoring or deleting them;
- zero remaining reservations, released service/lifetime locks and fresh
  keeper incarnations before the next version is accepted;
- twelve prepared engines with effective CPU payload locks, and byte-unchanged
  package contents.

`installed-fleet-update.json` records the exact packages, runtime hashes,
stage results and history revisions without tokens or conversation text.
Linux and all four native macOS household CI variants test against the pinned
baseline `01e3f86840f27260ea0e1229586116d8d99a26c3`.

This gate has no active autonomous policies or unfinished lifecycle operations;
it does not certify migration of such state. It is a reduced-model loopback,
quiescent update for the tested pair, not physical LAN/datacenter acceptance,
native Windows support, notarization, arbitrary backwards compatibility or
zero downtime. The [deployment checks](../PRODUCTION.md#deployment-acceptance)
still apply to the actual release and machines.
