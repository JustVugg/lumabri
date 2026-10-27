# Resident workers in containers

The worker uses the same native household runtime, approvals and resource
accounting as the desktop. The image contains no checkpoint or credentials.
It runs `lumabri donor --headless` under the container supervisor. The optional
policy controller talks to the gateway API; workers do not access Kubernetes
Secrets or create Pods through the Kubernetes API themselves.

## Build from a verified native candidate

```sh
python3 tools/prepare_container_context.py --package /path/to/linux-candidate --output /tmp/lumabri-image
docker build --tag lumabri-resident:local /tmp/lumabri-image
```

The context includes only the candidate files and package verifier. Hashes,
file modes and architecture are checked again inside the build. Build each
architecture from its own native package. This is a consistency check, not a
publisher signature. Use an immutable image digest for a deployment; this
command does not publish an image or create servers.

The runtime image uses Ubuntu 24.04 and its OpenMP library. Colibri is already
linked into the native Segment executables. No Python interpreter is shipped
in the final runtime image. GPU images are not claimed by this CPU deployment.

## Configure an authorized worker pool

`deploy/kubernetes/workers.example.yaml` is an explicit starting configuration,
not an installer. Set your image, tracker Service, household Secret, replica
count and per-worker resource/storage budgets before applying it in your own
namespace. Requests and limits should account for the **whole process set**,
including preparation buffers and the system reserve, not just model weights.
The example's 2 GiB limit and 1.5 GB donation are for small models; increase
both deliberately for larger ones. CPU and memory facts respect the container
limits. Each worker gets separate persistent state and endpoint identity.

The tracker and gateway must run the same compatible runtime. Run `tracker`
with its authenticated household configuration. The gateway can run
`lumabri api serve --tracker TRACKER:PORT`; its HTTP listener stays on loopback.
Expose it through the documented HTTPS proxy in the same Pod, not by disabling
API origin checks or publishing the private donor control socket. The gateway
needs its configured checkpoint source during preparation.

Worker Pods advertise their actual Pod IP. Allow authenticated direct traffic
between gateway, tracker and workers on the configured household ports (by
default 47300–47315), plus required DNS traffic. Do not add public host ports.
Use namespace isolation and the network policies supported by your cluster.

`IPC_LOCK` is the example's sole added capability and is required to lock
approved engine memory under typical container runtimes. It is not permission
to exceed the cgroup memory limit. If cluster policy prohibits effective locks,
preparation must fail; there is no disk/swap fallback. The test inspects actual
engine lock evidence instead of treating a declared capability as proof.
Checkpoint staging uses persistent disk **before** READY; never mount it on a
memory-backed volume without charging those bytes to the container budget.

## Approval and lifecycle

Applying a worker pool does not authorize any model. The owner can attach a
TUI with the same private state or use an authenticated administrative exec:

```sh
kubectl exec lumabri-workers-0 -- lumabri donor-policy inspect
kubectl exec lumabri-workers-0 -- lumabri donor-policy accept-current REVIEW
```

Review the requester, checkpoint, interval, RAM and session limits before
accepting. This is a one-off consent. Repeat grants remain separate and bounded.
Gateway and agent credentials cannot manufacture donor approval.

The worker readiness probe establishes a live local keeper and scheduler. It
does **not** mean a model is ready. The inference gateway admits a chat only
after the entire approved plan has reached READY and its endpoints/identities
have been checked. Starting or scaling the StatefulSet is not enough.

The example uses `OnDelete` updates to avoid an automatic rolling eviction of
resident segments. Before replacing a worker, use model-level drain, wait for
in-flight and retained sessions to finish, and retire or replace its affected
allocations. SIGTERM then releases owned resources. Forced eviction is a worker
failure. A restarted donor starts empty and requires fresh authority; its old
process IDs and approvals are not replayed. PVCs survive Pod deletion and have
their own storage lifecycle and cost.

## Acceptance scope

`kubernetes_workers_test.py` runs only against an explicitly disposable kind
context. It uses the candidate's real native engines, two reduced OLMoE
checkpoint identities, separate tracker/gateway/worker Pods, live owner
approvals, streaming chat and effective lock observations. It does not certify
large-model throughput, multiple physical hosts, ARM hardware or a specific
datacenter network. Those require their corresponding deployment evidence.
