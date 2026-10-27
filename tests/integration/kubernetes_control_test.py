"""Native controller against TLS Kubernetes and authenticated API contracts.

The in-process API doubles check transport/CAS reconciliation, not native
Kubernetes admission or engine execution; the resident flow supplies the latter.
"""
import copy
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
FINALIZER = "lumabri.io/pause-replica-policy"


class Cluster:
    def __init__(self, home, external_api=None):
        self.home = Path(home)
        self.token = "operator." + "a" * 64
        self.service_token = "isolated-service-token"
        for name, value in (("api.token", self.token), ("service.token", self.service_token)):
            path = self.home / name
            path.write_text(value)
            path.chmod(0o600)
        self.config = dict(action="configure", id="1" * 64, revision=1, route_revision=1,
                           enabled=True, min_replicas=1, max_replicas=2, cooldown_seconds=30,
                           idle_seconds=60, up_rejections=3, ttft_ms=0,
                           max_reserved_bytes="4000000000", source_model="olmoe", nodes=["2" * 64],
                           context=128, sessions=2, max_new=8, adopt_allocations=["3" * 64])
        self.policy = {**copy.deepcopy(self.config), "enabled": False, "faulted": False}
        self.object = {"apiVersion": "lumabri.io/v1alpha1", "kind": "ReplicaPolicy", "metadata": {
            "name": "model-" + "1" * 32 + "." + "1" * 32, "namespace": "test", "uid": "a1b2-c3d4",
            "resourceVersion": "1", "generation": 1, "finalizers": ["example.org/retained"]},
            "spec": copy.deepcopy(self.config)}
        self.posts = []
        self.patches = []
        self.lose_write = False
        self.api_status = self.kube_status = 200
        self.patch_status = 200
        self.continuation = ""
        self.auth_errors = []
        self.threads = []
        self.external_api = external_api
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def reply(self, code, body):
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)

            def authorized(self):
                token = owner.service_token if self.server.kube else owner.token
                if self.headers.get("Authorization") != "Bearer " + token:
                    owner.auth_errors.append("credential mismatch")
                    self.reply(401, {"error": "denied"})
                    return False
                return True

            def do_GET(self):
                if not self.authorized():
                    return
                if self.server.kube:
                    self.reply(owner.kube_status, {"metadata": {"continue": owner.continuation},
                                                 "items": [copy.deepcopy(owner.object)]})
                else:
                    self.reply(owner.api_status, {"policies": [copy.deepcopy(owner.policy)]})

            def do_POST(self):
                if not self.authorized():
                    return
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.posts.append(copy.deepcopy(body))
                if self.path != "/api/v1/elastic-policy" or self.server.kube:
                    self.reply(404, {})
                    return
                if body["revision"] != owner.policy["revision"]:
                    self.reply(409, {"error": "revision"})
                    return
                if body["action"] == "configure":
                    if body["route_revision"] != owner.policy["route_revision"]:
                        self.reply(409, {"error": "route"})
                        return
                    owner.policy.update(body)
                else:
                    assert body["action"] == "disable"
                    owner.policy["enabled"] = False
                owner.policy["revision"] += 1
                if owner.lose_write:
                    owner.lose_write = False
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                self.reply(200, owner.policy)

            def do_PATCH(self):
                if not self.authorized():
                    return
                patch = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.patches.append(copy.deepcopy(patch))
                if owner.patch_status != 200:
                    self.reply(owner.patch_status, {})
                    return
                updated = copy.deepcopy(owner.object)
                for action in patch:
                    bits = action["path"].strip("/").split("/")
                    cursor = updated
                    for bit in bits[:-1]:
                        cursor = cursor[bit]
                    if action["op"] == "test":
                        if cursor.get(bits[-1]) != action["value"]:
                            self.reply(409, {})
                            return
                    else:
                        assert action["op"] == "add"
                        cursor[bits[-1]] = action["value"]
                updated["metadata"]["resourceVersion"] = str(int(updated["metadata"]["resourceVersion"]) + 1)
                owner.object = updated
                self.reply(200, updated)

        self.api = None
        if external_api is None:
            self.api = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            self.api.kube = False
        self.kube = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.kube.kube = True
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                        "-keyout", str(self.home / "server.key"), "-out", str(self.home / "ca.pem")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.home / "ca.pem"), str(self.home / "server.key"))
        self.kube.socket = context.wrap_socket(self.kube.socket, server_side=True)
        for server in (self.api, self.kube):
            if server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                self.threads.append(thread)

    def run(self, expected=0, override=()):
        api_port = self.external_api.port if self.external_api else self.api.server_port
        args = [str(ROOT / "lumabri-kube"), "--server", f"https://127.0.0.1:{self.kube.server_port}",
                "--ca", str(self.home / "ca.pem"), "--service-token", str(self.home / "service.token"),
                "--api-token", str(self.home / "api.token"), "--namespace", "test", "--api-port", str(api_port), "--once"]
        result = subprocess.run([*args, *override], capture_output=True, text=True, timeout=25)
        assert result.returncode == expected, (result.returncode, result.stderr)
        assert not self.auth_errors, self.auth_errors
        assert self.token not in result.stdout + result.stderr
        return result

    def close(self):
        for server in (self.api, self.kube):
            if server:
                server.shutdown()
                server.server_close()


class KubernetesControl(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="lmb-kube-test-")
        self.c = Cluster(self.tmp.name)

    def tearDown(self):
        self.c.close()
        self.tmp.cleanup()

    def applied(self):
        c = self.c
        c.run()  # durable finalizer precedes side effects
        self.assertEqual(c.posts, [])
        self.assertIn(FINALIZER, c.object["metadata"]["finalizers"])
        c.run()
        c.run()  # new process observes exact committed revision
        self.assertEqual(c.object["status"]["phase"], "Applied")
        self.assertEqual(len(c.posts), 1)

    def test_apply_restart_dynamic_routes_and_safe_delete(self):
        self.applied()
        c = self.c
        c.policy["adopt_allocations"].append("4" * 64)
        c.policy["route_revision"] += 1
        c.run()
        self.assertEqual(len(c.posts), 1)
        c.object["metadata"]["deletionTimestamp"] = "2026-10-27T00:00:00Z"
        c.run()
        self.assertEqual(c.posts[-1]["action"], "disable")
        self.assertIn(FINALIZER, c.object["metadata"]["finalizers"])
        c.run()
        self.assertNotIn(FINALIZER, c.object["metadata"]["finalizers"])
        self.assertEqual(c.object["metadata"]["finalizers"], ["example.org/retained"])
        self.assertEqual(c.policy["adopt_allocations"], ["3" * 64, "4" * 64])
        self.assertEqual([p["action"] for p in c.posts], ["configure", "disable"])

    def test_lost_apply_reply_is_observed_without_second_write(self):
        self.c.lose_write = True
        self.applied()
        self.assertEqual(self.c.policy["revision"], 2)

    def test_manual_policy_drift_is_not_overwritten_or_disabled(self):
        self.applied()
        c = self.c
        c.policy["revision"] += 1
        c.policy["max_replicas"] = 3
        c.run()
        self.assertEqual(c.object["status"]["phase"], "Conflict")
        c.object["metadata"]["deletionTimestamp"] = "2026-10-27T00:00:00Z"
        c.run()
        self.assertEqual(len(c.posts), 1)
        self.assertIn(FINALIZER, c.object["metadata"]["finalizers"])

    def test_cancel_before_apply_fences_delayed_configure(self):
        c = self.c
        c.run()
        c.object["metadata"]["deletionTimestamp"] = "2026-10-27T00:00:00Z"
        c.run()
        c.run()
        self.assertEqual([p["action"] for p in c.posts], ["disable"])
        self.assertEqual(c.policy["revision"], 2)
        self.assertFalse(c.policy["enabled"])
        self.assertNotIn(FINALIZER, c.object["metadata"]["finalizers"])

    def test_permission_failure_and_partial_listing_cannot_mutate(self):
        c = self.c
        c.patch_status = 403
        c.run()
        self.assertFalse(c.posts)
        c.patch_status = 200
        c.continuation = "more-policies"
        c.run(expected=1)
        self.assertFalse(c.posts)
        c.continuation = ""
        c.kube_status = 403
        c.run(expected=1)
        self.assertFalse(c.posts)

    def test_tls_must_be_verified_and_plain_http_is_forbidden(self):
        c = self.c
        c.run(expected=1, override=("--ca", str(c.home / "missing-ca")))
        c.run(expected=2, override=("--server", f"http://127.0.0.1:{c.kube.server_port}"))
        self.assertFalse(c.posts)

    def test_new_policy_or_wrong_model_name_cannot_be_adopted(self):
        c = self.c
        c.object["spec"]["revision"] = 0
        c.run()
        self.assertEqual(c.object["status"]["phase"], "Invalid")
        self.assertFalse(c.posts)
        c.object["spec"]["revision"] = 1
        c.object["metadata"]["name"] = "other-policy"
        c.run()
        self.assertEqual(c.object["status"]["phase"], "Invalid")
        self.assertFalse(c.posts)


def verify_resident_kubernetes(api, allocations):
    """TLS Kubernetes contract + real Lumabri policy and resident inference."""
    import http.client
    with tempfile.TemporaryDirectory(prefix="lmb-kube-resident-") as directory:
        cluster = Cluster(directory, external_api=api)
        route = json.loads(api.cli("model-add", "kube-fixture", allocations[0]["id"], "--tracker", api.tracker))
        try:
            cluster.token = api.token
            (cluster.home / "api.token").write_text(api.token)
            config = {**cluster.config, "id": route["id"], "revision": 0, "route_revision": route["revision"],
                      "enabled": False, "max_replicas": 1, "max_reserved_bytes": str(4 << 30),
                      "source_model": allocations[0]["name"], "nodes": api.start_request["nodes"],
                      "sessions": api.start_request["sessions"], "adopt_allocations": [allocations[0]["id"]]}
            code, policy = api.http("/api/v1/elastic-policy", config)
            assert code == 200 and policy["revision"] == 1, policy
            cluster.object["metadata"]["name"] = "model-" + route["id"][:32] + "." + route["id"][32:]
            cluster.object["spec"] = {**config, "revision": 1, "enabled": True}
            before = api.http("/api/v1/workspace")[1]["allocations"]
            for _ in range(3):
                cluster.run()
            assert cluster.object["status"]["phase"] == "Applied", cluster.object
            token = api.cli("user-add", "kube-inference")
            for model in (route["id"], allocations[1]["id"]):
                api.cli("grant", "kube-inference", model)
                connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=30)
                connection.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
                    "messages": [{"role": "user", "content": "hello from Kubernetes policy control"}]}),
                    {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
                response = connection.getresponse()
                reply = response.read()
                assert response.status == 200 and b"event: done\n" in reply and b"event: error\n" not in reply, (response.status, reply)
                connection.close()
            api.cli("stop")
            api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
            cluster.run()
            assert cluster.object["status"]["phase"] == "Applied"
            cluster.object["metadata"]["deletionTimestamp"] = "2026-10-27T00:00:00Z"
            deadline = time.monotonic() + 15
            while FINALIZER in cluster.object["metadata"]["finalizers"]:
                cluster.run()
                assert time.monotonic() < deadline, (cluster.object, api.http("/api/v1/elastic-policy"))
                time.sleep(.1)
            policies = api.http("/api/v1/elastic-policy")[1]["policies"]
            final = next(p for p in policies if p["id"] == route["id"])
            assert not final["enabled"] and final["revision"] == 3
            after = api.http("/api/v1/workspace")[1]["allocations"]
            assert {a["id"] for a in before} == {a["id"] for a in after}, "policy removal changed resident ownership"
            for allocation in allocations:
                code, state = api.http("/api/v1/allocations/" + allocation["id"] + "/control")
                assert code == 200 and state["state"] == "accepting" and not state["weights_unloaded"], state
        finally:
            cluster.close()
            api.cli("model-remove", route["id"], "--tracker", api.tracker)
    print("RESIDENT KUBERNETES POLICY: PASS (real API, two OLMoE identities, inference, restart, pause without unloading)", flush=True)


if __name__ == "__main__":
    unittest.main()
