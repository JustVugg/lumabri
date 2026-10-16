# Managed inference release checks

A green CI run validates a specific commit and test configuration. It does not
certify every checkpoint, GPU, network or workload. Current evidence and
remaining work are tracked in the [release ledger](docs/LAN_RELEASE_CHECKLIST.md).
Do not turn a historical test into a claim about a different installed build.

The release target is a resident inference service on **owned or authorized
machines**: approved model allocations, isolated conversations, bounded
admission, visible recovery and explicit resource limits. Public strangers,
cloud provisioning and Kubernetes integration are not delivered by the
household transport or the replica controller.

## Scope of a supported configuration

Record the exact Lumabri and Colibri commits, build flags, operating systems,
CPU/GPU hardware, backend actually used, checkpoint content identity, numeric
class, layer placement, residency policy, context and conversation limits.
Keep the initial/final resource inventory and test logs with those records.

Registration is not execution proof. Every advertised adapter/backend must pass
its own oracle, memory and lifecycle checks. The pinned upstream adapters and
Lumabri's resident DeepSeek V4.1 adapter have different contracts; see
[backends](docs/HOUSEHOLD_BACKENDS.md) and [V4.1](docs/DEEPSEEK_V41.md).
Household admission currently selects CPU execution. Detecting a GPU does not
make GPU household inference supported.

Residency includes engine weights, required shared tables, state and scratch,
not just checkpoint file size. Test the chosen memory policy under pressure.
Preventing checkpoint reads alone does not prevent OS swap or compression.
For `engine-locked`, require evidence of effective locks on owned CPU payloads;
a denied lock is a failed preparation, not permission to silently switch policy.
See [resident runtime](docs/RESIDENT_RUNTIME.md).

## Local and native CI evidence

Build the complete household runtime, not only its interface:

```sh
make household swarm_probe ENGINE=/path/to/colibri/c
make test ENGINE=/path/to/colibri/c
make test-sanitize
```

Use the pinned Colibri revision in the README and installed test dependencies
from `requirements-test.txt`. The build prepares its engine sources below
`build/`; it must not edit the supplied Colibri checkout.

The versioned CI workflow adds real reduced-checkpoint inference and verifies:

- TUI request, all-party approval, preparation, READY and streamed output;
- manager restart and TUI detach without replayed approvals or duplicate loads;
- distinct resident models, isolated users and selective retirement;
- 1/2/4/8 conversation slots, bounded overload, cancellation and cleanup;
- whole-workload capacity measurements and invalidation after configuration changes;
- actual web measurement, comparison, separate routing confirmation and chat;
- replica growth, drain, release, dormant wake and interrupted-response replay;
- loss of an explicitly owned engine, including Darwin's supervisor hierarchy;
- native Intel/Apple Silicon builds with and without OpenMP, plus installed layouts.

For example, with a compatible tiny OLMoE fixture already in
`build/home-flow-models/olmoe-tiny`:

```sh
LUMABRI_RESIDENCY_POLICY=engine-locked \
python3 tests/integration/household_service_flow_test.py \
  --models-dir build/home-flow-models --joint complete --api-prepare \
  --donor-ram-gb 2 --sessions 2 --placement-comparison
```

This starts isolated local homes and real engines. Its declared machine prices
are test inputs, not bills or demonstrated savings. Tiny synthetic output is
not a model-quality benchmark. Loopback execution is not a physical-LAN test.
The workflow contains the additional fault-proxy and browser setup; those
tools belong in disposable test runtimes, never a shipped package.

## Deployment acceptance

Before declaring a release supported, run its exact installed package on each
advertised platform and deployment topology. At minimum:

1. Verify package contents and hashes; test a clean install, update and rollback.
   A matching manifest is not a release signature. macOS ad-hoc signing is not
   notarization; WSL is not native Windows. See [native candidates](docs/NATIVE_CANDIDATES.md).
2. Pair the actual machines, authenticate endpoints and verify direct data-path
   connectivity. Keep firewalls enabled. Neither an outbound report nor LAN
   discovery proves the donor is reachable for inference.
3. Run a trained checkpoint through the approved resident path. Compare local
   and distributed output under a documented compatible numerical contract.
   Record participation and memory use; a connected donor is not proof it ran layers.
4. Exercise two real models together, private conversations, overload, Stop and
   selective unload. Report each session's first-text and inter-token timings,
   completion and rejection counts, not just aggregate tok/s.
5. Interrupt worker, host and coordinator separately. Verify recovery where an
   authorized alternative exists, and an explicit incomplete conversation where
   it does not. No duplicate text, automatic consent or inferred memory release.
6. Test replica policies through a burst, cooldown and idle period. Confirm
   existing work drains before release and that min/max and byte budgets hold.
   Unreachable ownership stays charged until authoritatively reconciled.
7. Run the documented sustained workload and retain every failure. Report its
   duration and limits; a short probe is not sustained-capacity certification.
8. Check costs against actual billing before claiming server savings. A lower
   declared placement price or a routing change alone does not stop a server,
   release storage or prove a cheaper bill. Provisioning requires a separately
   authorized provider, credentials and spending cap.

`lumabri doctor --json` supplies basic diagnostics, not this acceptance result:
it still treats Segment binaries as optional for legacy installations. A
managed-inference release requires both Segment services and their execution
tests. The web/API remains loopback-only; exposing it to another network needs
a separately designed and tested deployment boundary.

All required checks must pass on the final PR head before normal merge. Retain
native job results and the final package manifest with release evidence.
Rolling changes must follow [protocol compatibility](PROTOCOL_COMPATIBILITY.md);
do not replace a fenced placement underneath an active conversation.

## Legacy Expert performance tools

`production_gate.sh` and `production_check.py` remain developer regression and
Expert speed-claim tools. Their names do **not** mean the managed release above
has passed. With no multi-host specification, the script explicitly skips that
evidence. Its optional multi-host check still requires three measured peers
and a 2x single-chat speedup over its baselines. This is the legacy experiment's
acceptance condition, not a promise or release requirement that adding any
computer must double a resident Segment chat's speed.

Existing experiments may use [SWARM_BENCH.md](SWARM_BENCH.md). They do not replace
managed multi-model, privacy, residency, fault, package or cost validation.
