"""Two real reduced-OLMoE allocations through the shipped TLS proxy recipe."""
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess

from https_proxy_fixture import TLSProxy


def verify_https_resident(api, until, service, artifacts, nginx):
    proxy = TLSProxy(artifacts / "tls", Path(api.env["HOME"]) / ".lumabri/api-access/proxy.sock", nginx)
    report = {"schema": 1, "scope": "verified_tls_reduced_olmoe_loopback", "run_complete": False,
              "production_ingress_certified": False, "completed_turns": 0, "client_stops": 0,
              "gateway_sha256": hashlib.sha256((api.runtime / "lumabri").read_bytes()).hexdigest(),
              "nginx_version": subprocess.run([str(nginx), "-v"], capture_output=True, text=True,
                                               check=True, timeout=5).stderr.strip()}

    def request(path, body=None, token=None):
        status, data, _ = proxy.request(path, api.token if token is None else token,
                                        None if body is None else json.dumps(body))
        return status, json.loads(data)

    def allocation_state():
        result = {}
        for name in ("a", "b"):
            donor = service(name)["donor"]
            assert donor["live"]
            result[name] = {key: donor[key] for key in
                ("instance", "pid", "segment_pid", "host_pid", "model_count", "reserved_total_bytes")}
        return result

    def turn(model, token, stop=False):
        connection = proxy.connection()
        response = None
        try:
            connection.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
                "messages": [{"role": "user", "content": "Count from one to ten."}]}),
                {"Authorization": "Bearer " + token, "Host": proxy.authority,
                 "Origin": proxy.origin, "Content-Type": "application/json"})
            response = connection.getresponse()
            assert response.status == 200, (response.status, response.read())
            assert response.getheader("Content-Type") == "text/event-stream"
            event = None; done = False; text = bytearray()
            while True:
                line = response.readline()
                if not line:
                    break
                assert len(line) <= 1 << 20
                if line.startswith(b"event: "):
                    event = line[7:].strip()
                elif line.startswith(b"data: "):
                    assert not done, "data after DONE"
                    data = json.loads(line[6:])
                    if event == b"delta":
                        text.extend(base64.b64decode(data["bytes"], validate=True))
                        if stop and text:
                            return None  # client stop, not an assertion of interrupted kernels
                    else:
                        assert event == b"done" and data["timing"]["token_notifications"] == 8, event
                        done = True
            assert text and done
            return text.decode("utf-8", errors="replace")
        finally:
            if response:
                response.close()
            connection.close()

    def idle():
        status, demand = request("/api/v1/demand")
        assert status == 200 and not demand["telemetry_gaps"]
        return all(not m["admitted"] and not m["opening"] for m in demand["models"])

    original = allocation_state()
    try:
        assert not service("chatter")["api"]["live"]
        api.cli("start", "--tracker", api.tracker, "--https-origin", proxy.origin)
        proxy.start()
        status, workspace = request("/api/v1/workspace")
        assert status == 200 and workspace["inventory_ok"]
        ids = sorted(a["id"] for a in workspace["allocations"])
        assert len(ids) == 2
        tokens = [api.cli("user-add", name) for name in ("tls-a", "tls-b")]
        for name, model in zip(("tls-a", "tls-b"), ids):
            api.cli("grant", name, model, "--tracker", api.tracker)
        # Two users and two checkpoints, each with independent model authority.
        for i in range(2):
            status, models = request("/api/v1/models", token=tokens[i])
            assert status == 200 and [m["id"] for m in models["models"]] == [ids[i]], models
            body = {"model": ids[1 - i], "max_tokens": 8, "messages": [{"role": "user", "content": "not sent"}]}
            assert request("/api/v1/chat", body, tokens[i])[0] == 403
            assert request("/api/v1/workspace", token=tokens[i])[0] == 403
        with ThreadPoolExecutor(max_workers=2) as pool:
            answers = list(pool.map(lambda i: turn(ids[i], tokens[i]), range(2)))
        report["completed_turns"] += 2
        until(idle, "TLS turns did not drain", 15)
        assert turn(ids[0], tokens[0], stop=True) is None
        report["client_stops"] = 1
        until(idle, "TLS client stop did not release admission", 15)
        assert turn(ids[0], tokens[0])
        report["completed_turns"] += 1
        until(idle, "post-stop TLS turn did not drain", 15)
        conversation = {"model": ids[0], "title": "TLS lifecycle", "state": "complete",
            "messages": [{"role": "user", "content": "Count from one to ten."},
                         {"role": "assistant", "content": answers[0]}]}
        status, history = request("/api/v1/conversations", {"revision": 0, "conversation": conversation}, tokens[0])
        assert status == 200, history
        path = "/api/v1/conversations/" + history["id"]
        assert request(path, token=tokens[1])[0] == 404
        api.cli("stop")
        assert allocation_state() == original
        api.cli("start", "--tracker", api.tracker, "--https-origin", proxy.origin)
        assert request(path, token=tokens[0]) == (200, history)
        assert turn(ids[1], tokens[1])
        report["completed_turns"] += 1
        until(idle, "restarted TLS gateway did not drain", 15)
        assert allocation_state() == original
        report.update(run_complete=True, resident_allocations_unchanged=True, history_isolated=True,
                      certificate_verified=True, gateway_restart_without_model_reload=True)
        print("HTTPS RESIDENT: PASS (two real models/users, stream, client stop, private history, gateway restart)", flush=True)
    finally:
        proxy.stop()
        api.cli("stop")
        (artifacts / "https-resident.json").write_text(json.dumps(report, indent=2) + "\n")
