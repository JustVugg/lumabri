# Kubernetes replica policies

`lumabri-kube` is an optional C controller for the namespaced
`ReplicaPolicy` resource in `lumabri.io/v1alpha1`. It translates an explicit
replica policy into the existing authenticated Lumabri management API.
Lumabri remains responsible for model placement, resident memory, donor
authorization, preparation, drain and release. Kubernetes owns containers
and machines. This controller neither creates VMs nor edits Pods.

The first integration controls **an existing, owner-configured policy**. Start
with a logical model and a paused policy created through Lumabri's ordinary
[replica management](ELASTIC_REPLICAS.md). Creating the Kubernetes resource
is explicit delegation of that policy, including its selected nodes and
bounded memory/replica limits. It cannot create donor consent or inference
grants. A policy whose revision is zero is refused.

## Install and connect

Build with libcurl 7.85 or newer and its development headers:

```sh
make lumabri-kube
```

The household build does not need libcurl. An optional container build is
provided in `deploy/kubernetes/Dockerfile.controller`; build it from the
repository root and pin the resulting image digest in your deployment.

A cluster administrator installs the CRD. Install the namespaced RBAC in the
same namespace as the gateway, replacing `lumabri` if necessary:

```sh
kubectl apply -f deploy/kubernetes/replicapolicy-crd.yaml
kubectl -n lumabri apply -f deploy/kubernetes/rbac.yaml
```

Run the controller **beside the gateway**, sharing its Pod's loopback network,
with a dedicated Lumabri operator/management credential mounted read-only.
Use one namespace per gateway; do not connect unrelated namespaces to the
same gateway credential. Do not give the controller the gateway's HOME, model
weights or owner's shell access. Its service account only reads/patches these
custom resources and their status; it cannot read Secrets, edit workloads,
grant RBAC or access other namespaces.

Explicitly project the service-account token with a limited lifetime and the
cluster CA; projected token files are reread per request. Mount the Lumabri
API credential separately. Files must be regular targets with no world access
or group write permission; mode `0440` with a matching `fsGroup`, or `0400`
with matching ownership, is suitable. Projected-volume symlinks are supported.
Configure the sidecar with these arguments, substituting your namespace:

```text
--server https://kubernetes.default.svc
--ca /var/run/lumabri-kube/ca.crt
--service-token /var/run/lumabri-kube/token
--api-token /var/run/lumabri-api/token
--namespace lumabri
--api-port 47380
```

Use a read-only root filesystem, a non-root user, no added capabilities and
`allowPrivilegeEscalation: false`. The controller itself does not need locked
memory. Inference workers have separate resident-memory requirements. The
Kubernetes API always uses certificate/hostname verification; the inference
management connection stays on loopback. Redirects, proxy-environment routing
and arbitrary per-resource URLs are not supported. Each request is bounded to
15 seconds and 2 MiB. A failed list or an incomplete/oversized list causes no
new policy writes.

## Declarative configuration

The `spec` is the **exact configure body** from the replica-policy API,
including current `revision`, `route_revision`, selected `nodes`, byte budget
and explicit `adopt_allocations`. It is validated by the same C parser used
by the service. There are no default computers or budgets.

The resource name must be `model-<first 32 hex digits>.<last 32 hex digits>`
of the logical model ID. This gives one resource per model in the namespace.
The ID is immutable. The service's current policy and route revisions are
the review fences: a resource with stale revisions reports `Conflict` instead
of overwriting another administrator's decision. To change a policy, inspect
the current policy and route, then submit the new reviewed `spec`.

The controller installs its finalizer before attempting configuration. It
then observes the exact settings at revision `spec.revision + 1`. Restarting,
running another controller instance or losing a write response does not
create a second allocation: policy writes are revision-fenced and actual
preparation stays in Lumabri's durable controller. Changing replica membership
through normal scaling does not cause repeated configuration.

`status.phase: Applied` means **the policy configuration was observed**.
It is not a Pod readiness check, proof of model readiness, capacity promise
or measured speed. Inspect the Lumabri workspace for model/runtime readiness
and live measurements. `Faulted`, `Conflict` and `Unknown` preserve their
meaning; the controller does not reset a fault or abandon an uncertain
allocation merely to make status green.

## Deletion and ownership

Deleting a `ReplicaPolicy` **pauses replica automation and retains resident
weights**. It does not kill inference or delete a server. Already published
withdrawals can finish their safe retirement; pending ownership remains
visible in Lumabri. Use the ordinary fenced drain/retire API for an explicit
model release after stopping relevant routes.

The finalizer is removed only after the disabled revision is observed. Even
deletion before the first configuration consumes the original revision with
a disable operation, so a delayed original configure cannot re-enable it.
A lost response is reobserved; it is not treated as rollback. Manual policy
drift keeps the finalizer and reports a conflict for operator review. Other
finalizers are preserved. Do not force-remove this finalizer while the
outcome is uncertain.

## Verification scope

`make test-kubernetes` exercises the compiled controller with TLS API
contracts: verified CA, no plaintext fallback, explicit permissions, bounded
listing, revision conflicts, lost replies, restart, dynamic replica membership
and safe deletion. This transport test uses isolated API doubles and does
not by itself certify Kubernetes admission, inference on a cluster, ARM
hardware or throughput. Native Kubernetes and resident-model integration
gates exercise those boundaries separately.
