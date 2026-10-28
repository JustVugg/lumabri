#!/bin/sh
set -eu
: "${KUBE_WORKER_TEST_IMAGE:?set the locally built fixture image}"
exec sh "$(dirname "$0")/ci_kubernetes_cluster.sh" tests/integration/kubernetes_workers_test.py --image "$KUBE_WORKER_TEST_IMAGE" --report build/kubernetes-workers.json
