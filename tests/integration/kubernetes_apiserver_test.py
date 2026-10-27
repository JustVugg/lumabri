"""Real Kubernetes CRD/admission/RBAC/status/finalizer acceptance on disposable kind.

The Lumabri policy endpoint is a contract double here; the separate resident
gate exercises the exact controller against real OLMoE engines and API.
"""
import argparse
import base64
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from kubernetes_control_test import Cluster, FINALIZER, ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--context", required=True)
    args = parser.parse_args()
    if os.environ.get("LUMABRI_DISPOSABLE_CLUSTER") != "1" or not args.context.startswith("kind-lumabri-policy-"):
        raise SystemExit("Use an explicitly disposable kind-lumabri-policy-* context")
    namespace = "lumabri-policy-test-" + str(os.getpid())

    def kubectl(*parts, body=None, success=True):
        result = subprocess.run(["kubectl", "--context", args.context, "-n", namespace, *parts],
                                input=None if body is None else json.dumps(body), text=True,
                                capture_output=True, timeout=60)
        if success:
            assert result.returncode == 0, (parts, result.stderr)
        return result

    kubectl("apply", "-f", str(ROOT / "deploy/kubernetes/replicapolicy-crd.yaml"))
    kubectl("wait", "--for=condition=Established", "crd/replicapolicies.lumabri.io", "--timeout=60s")
    kubectl("create", "namespace", namespace)
    try:
        kubectl("apply", "-f", str(ROOT / "deploy/kubernetes/rbac.yaml"))
        # A fresh quota initially has unknown usage. Admission correctly
        # refuses creates until the quota controller has observed the CRD.
        deadline = time.monotonic() + 60
        resource = "count/replicapolicies.lumabri.io"
        while True:
            quota = json.loads(kubectl("get", "resourcequota", "lumabri-policy-count", "-o", "json").stdout)
            status = quota.get("status", {})
            if status.get("hard", {}).get(resource) == "32" and resource in status.get("used", {}):
                break
            assert time.monotonic() < deadline, quota
            time.sleep(.5)
        identity = "system:serviceaccount:" + namespace + ":lumabri-policy-controller"
        for resource in ("secrets", "pods", "deployments", "roles"):
            denied = kubectl("auth", "can-i", "get", resource, "--as", identity, success=False)
            assert denied.stdout.strip() == "no", (resource, denied.stdout)
        assert kubectl("auth", "can-i", "patch", "replicapolicies/status", "--as", identity).stdout.strip() == "yes"
        with tempfile.TemporaryDirectory(prefix="lmb-kube-native-") as directory:
            cluster = Cluster(directory)
            try:
                config = json.loads(kubectl("config", "view", "--minify", "--raw", "-o", "json").stdout)
                endpoint = config["clusters"][0]["cluster"]
                ca = Path(directory) / "kube-ca.pem"
                ca.write_bytes(base64.b64decode(endpoint["certificate-authority-data"], validate=True))
                token = Path(directory) / "kube-token"
                token.write_text(kubectl("create", "token", "lumabri-policy-controller", "--duration=10m").stdout.strip())
                token.chmod(0o600)
                obj = copy.deepcopy(cluster.object)
                obj["metadata"] = {"name": obj["metadata"]["name"], "namespace": namespace}
                name = obj["metadata"]["name"]
                kubectl("create", "-f", "-", body=obj)

                def run():
                    result = subprocess.run([str(ROOT / "lumabri-kube"), "--server", endpoint["server"].rstrip("/"),
                        "--ca", str(ca), "--service-token", str(token), "--api-token", str(Path(directory) / "api.token"),
                        "--namespace", namespace, "--api-port", str(cluster.api.server_port), "--once"],
                        capture_output=True, text=True, timeout=30)
                    assert result.returncode == 0, result.stderr

                run()
                assert cluster.posts == [], "configuration preceded durable finalizer"
                run()
                run()
                observed = json.loads(kubectl("get", "replicapolicy", name, "-o", "json").stdout)
                assert observed["status"]["phase"] == "Applied", observed
                assert FINALIZER in observed["metadata"]["finalizers"]
                assert len(cluster.posts) == 1
                invalid = {"spec": {"max_replicas": 0}}
                rejected = kubectl("patch", "replicapolicy", name, "--type=merge", "-p", json.dumps(invalid), success=False)
                assert rejected.returncode != 0, "server accepted invalid replica count"
                replacement = {"spec": {"id": "9" * 64}}
                rejected = kubectl("patch", "replicapolicy", name, "--type=merge", "-p", json.dumps(replacement), success=False)
                assert rejected.returncode != 0, "server allowed model identity replacement"
                kubectl("delete", "replicapolicy", name, "--wait=false")
                run()
                run()
                gone = kubectl("get", "replicapolicy", name, success=False)
                assert gone.returncode != 0 and "NotFound" in gone.stderr, gone.stderr
                assert [p["action"] for p in cluster.posts] == ["configure", "disable"]
                assert cluster.policy["adopt_allocations"] == ["3" * 64]
            finally:
                cluster.close()
    finally:
        kubectl("delete", "namespace", namespace, "--wait=false", success=False)
    print("KUBERNETES API: PASS (real admission, namespaced RBAC, CAS, status, restart, finalizer)")


if __name__ == "__main__":
    main()
