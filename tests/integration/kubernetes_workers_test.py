"""Disposable Kubernetes data-plane acceptance using real reduced OLMoE.

Image must contain the verified runtime and test-only /fixtures + Python.
No AI service, provider, real user's credentials or existing namespace is used.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
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
        tracker = pod("tracker", ["tracker", "--port", "47300", "--household-discovery"]) + ":47300"
        for name in ("worker-a", "worker-b"):
            pod(name, ["lumabri", "donor", "--headless", "--join", tracker, "--name", name, "--ram-gb", "1.5"], donor=True)
        init_gateway = '''import os,pathlib
h=pathlib.Path(os.environ["HOME"])/".lumabri"; h.mkdir(mode=0o700,exist_ok=True)
p=h/"home.conf"; p.write_text("models=/fixtures\\ntracker="+os.environ["TEST_TRACKER"]+"\\ntoken="+os.environ["LUMABRI_TOKEN"]+"\\n"); p.chmod(0o600)
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
        excluded_nodes = set()
        def inventory():
            code, workspace = http("/api/v1/workspace")
            assert code == 200
            nodes[:] = [n["id"] for n in workspace["nodes"] if n["workload"] is not None and n["runtime_threads"] and n["id"] not in excluded_nodes]
            return len(nodes) == 2
        until(inventory)
        reviews = set()
        def prepare_model(model, selected):
            allocations = []
            intent = {"models": [model], "nodes": selected, "context": 128, "sessions": 2, "max_new": 8}
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
            return allocations[0]

        # These fixtures fit on one worker. Exercise co-residence deliberately;
        # selecting a pool does not promise that every worker enters the plan.
        allocations = [prepare_model(model, nodes[:1]) for model in ("model-a", "model-b")]
        code, workspace = http("/api/v1/workspace")
        assert code == 200
        colocated = [a for a in workspace["allocations"] if a["id"] in allocations]
        assert len(colocated) == 2
        for allocation in colocated:
            assert {r["node"] for r in allocation["ranges"]} == set(nodes[:1])
        whole_plan = next(a for a in colocated if a["id"] == allocations[0])
        whole_bytes = sum(r["reserved_bytes"] for r in whole_plan["ranges"])
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
        def lock_evidence(worker, expected_models, budget=1500000000):
            state = status(worker)
            assert state["live"] and state["model_count"] == expected_models, state
            log = execute(worker, "cat", "/var/lib/lumabri/.lumabri/home/engines.log").stdout
            assert log.count("event=ready policy=engine-locked fault=0") >= expected_models
            locks = re.findall(r"\[engine-memory\] allocations=(\d+) requested_bytes=(\d+) locked_bytes=(\d+) retained_locked_bytes=(\d+)", log)
            assert len(locks) >= expected_models and all(int(n) > 0 and 0 < int(wanted) <= int(locked) for n, wanted, locked, retained in locks)
            assert "event=lost" not in log and "event=ready policy=observe" not in log
            assert state["reserved_total_bytes"] <= budget
            return {"model_count": state["model_count"], "reserved_bytes": state["reserved_total_bytes"], "locks_verified": True}
        occupied = [w for w in ("worker-a", "worker-b") if status(w)["model_count"]]
        assert len(occupied) == 1
        report["co_resident"] = {occupied[0]: lock_evidence(occupied[0], 2)}

        # Release one model through the ordinary coordinated retirement;
        # the other must keep serving from the same resident worker.
        def retire(allocation):
            until(lambda: json.loads(cli("retire", allocation, "--tracker", tracker))["complete"], 60)
        retire(allocations[1])
        report["surviving_turn"] = turn(allocations[0])
        retire(allocations[0])
        until(lambda: all(status(w)["model_count"] == 0 for w in ("worker-a", "worker-b")))

        # Use measured planner reservations to make this SAME checkpoint too
        # large for either offered budget. Restart only our empty test workers;
        # no invented performance hint or planner override forces the split.
        excluded_nodes.update(nodes)
        limited_bytes = whole_bytes - 65536
        assert limited_bytes > 32 << 20
        for worker in ("worker-a", "worker-b"):
            kube("delete", "pod", worker, "--wait=true", "--timeout=60s")
            pod(worker, ["lumabri", "donor", "--headless", "--join", tracker, "--name", worker,
                         "--ram-gb", format(limited_bytes / 1e9, ".9f")], donor=True)
        until(inventory)
        for node in nodes:
            code, refused = http("/api/v1/preparation", {"action": "preview", "models": ["model-a"],
                                "nodes": [node], "context": 128, "sessions": 2, "max_new": 8})
            assert code == 422, ("single worker unexpectedly fits", refused)
        split = prepare_model("model-a", nodes)
        code, workspace = http("/api/v1/workspace")
        assert code == 200
        plan = next(a for a in workspace["allocations"] if a["id"] == split)
        assert {r["node"] for r in plan["ranges"]} == set(nodes), plan
        cli("grant", "cluster-test", split, "--tracker", tracker)
        report["split_turn"] = turn(split)
        report["split_ranges"] = plan["ranges"]
        report["split_offer_bytes"] = limited_bytes
        report["workers"] = {w: lock_evidence(w, 1, limited_bytes) for w in ("worker-a", "worker-b")}
        report["complete"] = True
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print("KUBERNETES WORKERS: PASS (co-resident models, selective retirement, memory-required cross-Pod Segment chain, real streaming, effective locks)")
    finally:
        if not report["complete"]:
            for name in ("tracker", "gateway", "worker-a", "worker-b"):
                print(name, kube("logs", name, "--tail=30", check=False).stdout)
                if name.startswith("worker"):
                    print(execute(name, "lumabri", "service", "status", "--json", check=False).stdout)
                    print(execute(name, "tail", "-n", "35", "/var/lib/lumabri/.lumabri/home/engines.log", check=False).stdout)
        kube("delete", "namespace", namespace, "--wait=false", check=False)


if __name__ == "__main__":
    main()
