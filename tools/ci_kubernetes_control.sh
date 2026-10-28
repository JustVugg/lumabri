#!/bin/sh
set -eu
exec sh "$(dirname "$0")/ci_kubernetes_cluster.sh" tests/integration/kubernetes_apiserver_test.py
