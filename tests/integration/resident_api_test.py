"""Called inside the two-model real-engine gate, not a fake generation server.
The second model keeps its TUI conversation while API users use the first.
"""
import base64
import http.client
import json
from pathlib import Path
import socket
import shutil
import subprocess


def verify_resident_api(runtime, env, tracker, first_model_name, artifacts):
    def cli(*args):
        result = subprocess.run([str(runtime / "lumabri"), "api", *args], env=env,
                                text=True, capture_output=True, timeout=20)
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    models = json.loads(cli("list", "--tracker", tracker))["models"]
    assert len(models) == 2
    # Identify the first model using the caller's saved plan name, not order.
    first = next(m for m in models if m["name"] == first_model_name)
    second = next(m for m in models if m["id"] != first["id"])
    alice, bob = cli("user-add", "alice"), cli("user-add", "bob")
    cli("grant", "alice", first["id"], "--tracker", tracker)
    cli("grant", "bob", second["id"], "--tracker", tracker)
    with socket.socket() as reserving:
        reserving.bind(("127.0.0.1", 0)); port = reserving.getsockname()[1]
    def service(action):
        result = subprocess.run([str(runtime / "lumabri"), "service", action,
                                 *(["--json"] if action == "status" else [])],
                                env=env, text=True, capture_output=True, timeout=20)
        assert result.returncode == 0, result.stderr
        return {s["role"]: s for s in json.loads(result.stdout)["services"]} if action == "status" else None

    def request(path, token, body=None, extra=None):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
        headers = {"Authorization": "Bearer " + token}
        if body is not None:
            headers["Content-Type"] = "application/json"
        headers.update(extra or {})
        conn.request("POST" if body is not None else "GET", path, body, headers)
        response = conn.getresponse(); status, data = response.status, response.read()
        conn.close(); return status, data

    def chat(token, target, prompt="hi"):
        return request("/api/v1/chat", token, json.dumps({"model": target, "max_tokens": 8,
                       "messages": [{"role": "user", "content": prompt}]}))

    try:
        cli("start", "--tracker", tracker, "--port", str(port))
        before = service("status")["api"]
        assert before["live"] and before["phase"] == 2
        service("restart")
        after = service("status")["api"]
        assert after["instance"] == before["instance"] and after["pid"] == before["pid"]
        assert request("/api/v1/models", "wrong")[0] == 401
        for token, own in ((alice, first), (bob, second)):
            status, data = request("/api/v1/models", token)
            assert status == 200 and [m["id"] for m in json.loads(data)["models"]] == [own["id"]]
        assert request("/api/v1/models", alice, extra={"Origin": "https://foreign.test"})[0] == 403
        assert chat(bob, first["id"])[0] == 403
        assert chat(alice, second["id"])[0] == 403
        assert request("/api/v1/chat", alice, '{"model":"x","model":"x"}')[0] == 400
        # More opens than available slots verifies acknowledged retirement.
        for turn in range(6):
            status, data = chat(alice, first["id"])
            assert status == 200, (status, data)
            events = []
            for block in data.decode().split("\n\n"):
                if not block:
                    continue
                lines = block.splitlines()
                assert len(lines) == 2 and lines[0].startswith("event: ") and lines[1].startswith("data: "), block
                events.append((lines[0][7:], json.loads(lines[1][6:])))
            assert events and events[-1][0] == "done" and all(e[0] in ("delta", "done") for e in events), events
            raw = b"".join(base64.b64decode(e[1]["bytes"], validate=True) for e in events if e[0] == "delta")
            assert raw and "STAT " in events[-1][1]["stats"]
        # Interrupted transport releases only the current API conversation.
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
        conn.request("POST", "/api/v1/chat", json.dumps({"model": first["id"], "max_tokens": 8,
                     "messages": [{"role": "user", "content": "cancel"}]}),
                     {"Authorization": "Bearer " + alice, "Content-Type": "application/json"})
        assert conn.getresponse().status == 200
        conn.close()
        status, data = chat(alice, first["id"])
        assert status == 200 and b"event: done" in data
        cli("revoke", "alice")
        assert request("/api/v1/models", alice)[0] == 401
        assert request("/api/v1/models", bob)[0] == 200
        print("RESIDENT API: PASS (real OLMoE, approved allocations, distinct users, TUI coexistence, manager restart, streaming, repeated close, cancellation, revoke)", flush=True)
    finally:
        cli("stop")
        assert not service("status")["api"]["live"]
        source = Path(env["HOME"]) / ".lumabri/service/api.log"
        if source.exists():
            shutil.copyfile(source, artifacts / "resident-api.log")
