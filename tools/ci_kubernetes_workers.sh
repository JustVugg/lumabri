#!/bin/sh
# Owns only its explicit disposable kind cluster and private kubeconfig.
set -eu
: "${KUBE_WORKER_TEST_IMAGE:?set the locally built fixture image}"
kube_tools_dir=$(mktemp -d /tmp/lumabri-kube-tools.XXXXXX)
kube_cluster_name="lumabri-policy-$$"
export KUBECONFIG="$kube_tools_dir/kubeconfig"
export PATH="$kube_tools_dir:$PATH"
cleanup() {
    if [ -x "$kube_tools_dir/kind" ]; then
        "$kube_tools_dir/kind" delete cluster --name "$kube_cluster_name"
    fi
    rm -rf "$kube_tools_dir"
}
trap cleanup EXIT HUP INT TERM
curl --fail --location --retry 2 https://kind.sigs.k8s.io/dl/v0.30.0/kind-linux-amd64 --output "$kube_tools_dir/kind"
curl --fail --location --retry 2 https://kind.sigs.k8s.io/dl/v0.30.0/kind-linux-amd64.sha256sum --output "$kube_tools_dir/kind.sha256"
kind_hash=$(cut -d ' ' -f 1 "$kube_tools_dir/kind.sha256")
printf '%s  %s\n' "$kind_hash" "$kube_tools_dir/kind" | sha256sum --check
curl --fail --location --retry 2 https://dl.k8s.io/release/v1.34.0/bin/linux/amd64/kubectl --output "$kube_tools_dir/kubectl"
curl --fail --location --retry 2 https://dl.k8s.io/release/v1.34.0/bin/linux/amd64/kubectl.sha256 --output "$kube_tools_dir/kubectl.sha256"
kubectl_hash=$(cat "$kube_tools_dir/kubectl.sha256")
printf '%s  %s\n' "$kubectl_hash" "$kube_tools_dir/kubectl" | sha256sum --check
chmod 755 "$kube_tools_dir/kind" "$kube_tools_dir/kubectl"
kind create cluster --name "$kube_cluster_name" --wait 120s --image kindest/node:v1.34.0@sha256:7416a61b42b1662ca6ca89f02028ac133a309a2a30ba309614e8ec94d976dc5a
kind load docker-image "$KUBE_WORKER_TEST_IMAGE" --name "$kube_cluster_name"
LUMABRI_DISPOSABLE_CLUSTER=1 python3 tests/integration/kubernetes_workers_test.py --context "kind-$kube_cluster_name" --image "$KUBE_WORKER_TEST_IMAGE" --report build/kubernetes-workers.json
