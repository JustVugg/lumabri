"""Disposable Kubernetes data-plane acceptance using real reduced OLMoE.

Image must contain the verified runtime and test-only /fixtures + Python.
No AI service, provider, real user's credentials or existing namespace is used.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import secrets
import subprocess
import time


HTTP = '''import http.client,json,sys
r=json.load(sys.stdin)
c=http.client.HTTPConnection("127.0.0.1",47380,timeout=40)
b=r.get("body")
h={"Authorization":"Bearer "+r["token"]}
if b is not None: h["Content-Type"]="application/json"
c.request("POST" if b is not None else "GET",r["path"],None if b is None else json.dumps(b),h)
s=c.getresponse(); data=s.read().decode("utf-8")
print(json.dumps({"status":s.status,"body":data}))
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--context", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get("LUMABRI_DISPOSABLE_CLUSTER") != "1" or not args.context.startswith("kind-lumabri-policy-"):
        raise SystemExit("Only an explicit disposable kind-lumabri-policy-* cluster is allowed")
    namespace = "lumabri-resident-test-" + str(os.getpid())
    secret = secrets.token_hex(32)
    report = {"schema": 1, "scope": "reduced_olmoe_separate_kubernetes_pods", "complete": False}

    def kube(*parts, body=None, check=True, timeout=90):
        p = subprocess.run(["kubectl", "--context", args.context, "-n", namespace, *parts],
                           input=body, text=True, capture_output=True, timeout=timeout)
        assert not check or p.returncode == 0, (parts[:3], p.stderr)
        return p

    def create(obj):
        kube("create", "-f", "-", body=json.dumps(obj))

    def execute(pod, *parts, body=None, check=True):
        return kube("exec", "-i", pod, "--", *parts, body=body, check=check)

    def cli(*parts):
        return execute("gateway", "lumabri", "api", *parts).stdout.strip()

    def status(pod):
        return next(s for s in json.loads(execute(pod, "lumabri", "service", "status", "--json").stdout)["services"] if s["role"] == "donor")

    def until(f, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if f():
                return
            time.sleep(.3)
        raise AssertionError("cluster acceptance deadline exceeded")

    def pod(name, command, tracker=None, donor=False):
        env = [{"name": "LUMABRI_TOKEN", "valueFrom": {"secretKeyRef": {"name": "household-key", "key": "token"}}},
               {"name": "LUMABRI_RAM_RESERVE_MB", "value": "256"},
               {"name": "LUMABRI_NO_DISK_PROBE", "value": "1"},
               {"name": "LUMABRI_IO_TIMEOUT_MS", "value": "15000"},
               {"name": "COLI_NO_OMP_TUNE", "value": "1"},
               {"name": "PIN", "value": "off"},
               {"name": "OMP_NUM_THREADS", "value": "2"},
               {"name": "LUMABRI_ADVERTISE", "valueFrom": {"fieldRef": {"fieldPath": "status.podIP"}}}]
        if tracker:
            env.append({"name": "TEST_TRACKER", "value": tracker})
        container = {"name": "runtime", "image": args.image, "imagePullPolicy": "Never", "command": command,
                     "env": env, "resources": {"requests": {"cpu": "100m", "memory": "256Mi"},
                                               "limits": {"cpu": "2", "memory": "2Gi" if donor else "512Mi"}},
                     "securityContext": {"allowPrivilegeEscalation": False, "readOnlyRootFilesystem": True,
                                         "capabilities": {"drop": ["ALL"], "add": ["IPC_LOCK"] if donor else []}},
                     "volumeMounts": [{"name": "state", "mountPath": "/var/lib/lumabri"}, {"name": "temp", "mountPath": "/tmp"}]}
        if donor:
            container["readinessProbe"] = {"exec": {"command": ["lumabri", "service", "live", "donor"]}, "periodSeconds": 2, "timeoutSeconds": 3}
        create({"apiVersion": "v1", "kind": "Pod", "metadata": {"name": name},
                "spec": {"restartPolicy": "Never", "automountServiceAccountToken": False,
                         "securityContext": {"runAsUser": 10001, "runAsGroup": 10001, "runAsNonRoot": True,
                                             "fsGroup": 10001, "seccompProfile": {"type": "RuntimeDefault"}},
                         "containers": [container], "volumes": [{"name": "state", "emptyDir": {}}, {"name": "temp", "emptyDir": {}}]}})
        kube("wait", "--for=condition=Ready", "pod/" + name, "--timeout=90s", timeout=100)
        return json.loads(kube("get", "pod", name, "-o", "json").stdout)["status"]["podIP"]

    kube("create", "namespace", namespace)
    try:
        create({"apiVersion": "v1", "kind": "Secret", "metadata": {"name": "household-key"}, "stringData": {"token": secret}})
        tracker = pod("tracker", ["tracker", "--port", "47300"]) + ":47300"
        for name in ("worker-a", "worker-b"):
            pod(name, ["lumabri", "donor", "--headless", "--join", tracker, "--name", name, "--ram-gb", "1.5"], donor=True)
        init_gateway = '''import os,pathlib
h=pathlib.Path(os.environ["HOME"])/".lumabri"; h.mkdir(mode=0o700,exist_ok=True)
p=h/"home.conf"; p.write_text("models=/fixtures\\ntracker="+os.environ["TEST_TRACKER"]+"\\n"); p.chmod(0o600)
os.execvp("lumabri",["lumabri","api","serve","--tracker",os.environ["TEST_TRACKER"],"--port","47380"])
'''
        pod("gateway", ["python3", "-c", init_gateway], tracker=tracker)
        token = cli("user-add", "cluster-test")
        cli("manage-grant", "cluster-test")
        cli("operator-grant", "cluster-test")

        def http(path, body=None, raw=False):
            result = json.loads(execute("gateway", "python3", "-c", HTTP,
                                body=json.dumps({"token": token, "path": path, "body": body})).stdout)
            return result["status"], result["body"] if raw else json.loads(result["body"])

        nodes = []
        def inventory():
            code, workspace = http("/api/v1/workspace")
            assert code == 200
            nodes[:] = [n["id"] for n in workspace["nodes"] if n["workload"] is not None and n["runtime_threads"]]
            return len(nodes) == 2
        until(inventory)
        allocations = []
        reviews = set()
        for model in ("model-a", "model-b"):
            intent = {"models": [model], "nodes": nodes, "context": 128, "sessions": 2, "max_new": 8}
            code, preview = http("/api/v1/preparation", {"action": "preview", **intent})
            assert code == 200, preview
            operation = secrets.token_hex(32)
            request = {"action": "start", "operation": operation, "review": preview["review"], **intent}
            code, reply = http("/api/v1/preparation", request)
            assert code == 202, reply
            assert http("/api/v1/preparation", request)[0] == 200

            def prepare():
                for worker in ("worker-a", "worker-b"):
                    if status(worker)["phase"] != 1:
                        continue
                    inspected = execute(worker, "lumabri", "donor-policy", "inspect", check=False)
                    if inspected.returncode:
                        continue
                    review = json.loads(inspected.stdout)["review"]
                    if review in reviews:
                        continue
                    reviews.add(review)
                    bad = execute(worker, "lumabri", "donor-policy", "accept-current", "0" * 64, check=False)
                    assert bad.returncode != 0
                    approved = json.loads(execute(worker, "lumabri", "donor-policy", "accept-current", review).stdout)
                    assert approved["state"] == "accepted" and not approved["repeat_permission"]
                code, state = http("/api/v1/preparation", {"action": "status", "operation": operation})
                assert code == 200 and state["state"] != "failed", state
                if state["state"] == "done":
                    assert len(state["allocations"]) == 1
                    allocations.append(state["allocations"][0]["id"])
                    return True
                return False
            until(prepare, 180)
        code, workspace = http("/api/v1/workspace")
        assert code == 200
        for allocation in workspace["allocations"]:
            assert {r["node"] for r in allocation["ranges"]} == set(nodes), "model did not span both workers"
        for allocation in allocations:
            cli("grant", "cluster-test", allocation, "--tracker", tracker)

        def turn(allocation):
            code, stream = http("/api/v1/chat", {"model": allocation, "max_tokens": 8,
                              "messages": [{"role": "user", "content": "hello"}]}, raw=True)
            assert code == 200 and "event: done" in stream and "event: error" not in stream
            assert "event: delta" in stream
            return {"allocation": allocation, "complete": True}
        with ThreadPoolExecutor(max_workers=2) as pool:
            report["turns"] = list(pool.map(turn, allocations))
        report["workers"] = {}
        for worker in ("worker-a", "worker-b"):
            state = status(worker)
            assert state["live"] and state["model_count"] == 2, state
            log = execute(worker, "cat", "/var/lib/lumabri/.lumabri/home/engines.log").stdout
            assert log.count("event=ready policy=engine-locked fault=0") >= 2
            assert "[engine-memory] allocations=" in log and "event=lost" not in log
            report["workers"][worker] = {"model_count": state["model_count"], "reserved_bytes": state["reserved_total_bytes"], "locks_verified": True}
        report["complete"] = True
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print("KUBERNETES WORKERS: PASS (two resident OLMoE identities, two worker Pods, real streaming, local approval, effective locks)")
    finally:
        if not report["complete"]:
            for name in ("tracker", "gateway", "worker-a", "worker-b"):
                print(name, kube("logs", name, "--tail=30", check=False).stdout)
                if name.startswith("worker"):
                    print(execute(name, "tail", "-n", "35", "/var/lib/lumabri/.lumabri/home/engines.log", check=False).stdout)
        kube("delete", "namespace", namespace, "--wait=false", check=False)


if __name__ == "__main__":
    main()
