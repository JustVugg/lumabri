"""Called inside the two-model real-engine gate, not a fake generation server.
The second model keeps its TUI conversation while API users use the first.
"""
import base64
import http.client
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import time
from home_flow_test import assert_stage_record


def verify_resident_api(runtime, env, tracker, first_model_name, artifacts, observed_plan):
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
    operator = cli("user-add", "operator")
    cli("operator-grant", "operator")
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
        assert request("/api/v1/workspace", "wrong")[0] == 401
        assert request("/api/v1/workspace", alice)[0] == 403
        assert json.loads(request("/api/v1/session", alice)[1])["operator"] is False
        assert json.loads(request("/api/v1/session", operator)[1])["operator"] is True
        status, data = request("/api/v1/workspace", operator)
        workspace = json.loads(data)
        assert status == 200 and workspace["inventory_ok"] and workspace["registry_ok"], workspace
        assert len(workspace["allocations"]) == 2 and len(workspace["nodes"]) >= 2
        assert all(n["machine_cost"] is None and n["energy_joules"] is None for n in workspace["nodes"])
        assert not any(n["gpu_execution_verified"] for n in workspace["nodes"])
        assert chat(operator, first["id"])[0] == 403, "operator visibility silently granted inference"
        assert json.loads(request("/api/v1/conversations", operator)[1])["conversations"] == []
        for token, own in ((alice, first), (bob, second)):
            status, data = request("/api/v1/models", token)
            assert status == 200 and [m["id"] for m in json.loads(data)["models"]] == [own["id"]]
        assert request("/api/v1/models", alice, extra={"Origin": "https://foreign.test"})[0] == 403
        assert chat(bob, first["id"])[0] == 403
        assert chat(alice, second["id"])[0] == 403
        assert request("/api/v1/chat", alice, '{"model":"x","model":"x"}')[0] == 400
        if os.environ.get("LUMABRI_TEST_BROWSER") == "1":
            from resident_web_test import verify_resident_browser
            verify_resident_browser(port, alice, bob, first["id"], artifacts)
        # More opens than available slots verifies acknowledged retirement.
        before_samples = assert_stage_record(observed_plan)
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
            assert events[-1][1]["observation_saved"], "valid stable-workload API turn was not observed"
            timing = events[-1][1]["timing"]
            assert timing["scope"] == "gateway_observed" and timing["token_notifications"] == 8, timing
            assert 0 <= timing["ttft_seconds"] <= timing["generation_seconds"] <= timing["completion_seconds"], timing
            assert timing["token_notification_gaps"]["count"] == 7, timing
            if turn == 0:
                conversation = {"model": first["id"], "title": "Real OLMoE API response", "state": "complete",
                                "messages": [{"role": "user", "content": "hi"},
                                             {"role": "assistant", "content": raw.decode("utf-8")}]}
                status, data = request("/api/v1/conversations", alice,
                                       json.dumps({"revision": 0, "conversation": conversation}))
                assert status == 200, (status, data)
                saved = json.loads(data)
                assert saved["conversation"] == conversation and saved["revision"] == 1
                path = "/api/v1/conversations/" + saved["id"]
                assert request(path, bob)[0] == 404
                assert request(path, operator)[0] == 404, "operator could read another user's conversation"
                status, data = request(path, alice)
                assert status == 200 and json.loads(data)["conversation"]["messages"][-1]["content"].encode() == raw
        assert assert_stage_record(observed_plan) >= before_samples + 6, "completed API turns did not update the planner"
        before_rejected = observed_plan.read_bytes()
        status, data = chat(alice, first["id"], "overflow " * 200)
        assert status == 200 and b"event: error" in data and b"event: done" not in data
        assert observed_plan.read_bytes() == before_rejected, "rejected generation changed the speed record"
        status, data = request("/api/v1/workspace", operator)
        allocation = next(a for a in json.loads(data)["allocations"] if a["id"] == first["id"])
        assert status == 200 and allocation["observation"]["state"] == "measured", allocation
        assert allocation["observation"]["decode_tok_s"] > 0
        assert allocation["observation"]["prompt_tokens"] > 0 and allocation["observation"]["generated_tokens"] == 8
        cli("operator-revoke", "operator")
        assert request("/api/v1/workspace", operator)[0] == 403
        assert json.loads(request("/api/v1/session", operator)[1])["operator"] is False
        cli("operator-grant", "operator")
        cli("revoke", "operator")
        assert request("/api/v1/workspace", operator)[0] == 401
        replacement_operator = cli("user-add", "operator")
        assert request("/api/v1/workspace", replacement_operator)[0] == 403, "recreated credential inherited operator access"
        # Interrupted transport releases only the current API conversation.
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
        conn.request("POST", "/api/v1/chat", json.dumps({"model": first["id"], "max_tokens": 8,
                     "messages": [{"role": "user", "content": "cancel"}]}),
                     {"Authorization": "Bearer " + alice, "Content-Type": "application/json"})
        assert conn.getresponse().status == 200
        conn.close()
        # Closing the HTTP socket requests cancellation; it is not an ACK
        # that the gateway worker has already reset/closed its host slot and
        # released the exact-allocation permit. An immediate new turn may
        # correctly receive BUSY until that bounded asynchronous cleanup ends.
        deadline = time.monotonic() + 5
        while True:
            status, data = chat(alice, first["id"])
            if status != 429 or json.loads(data).get("error") != "replicas_busy":
                break
            assert time.monotonic() < deadline, ("cancelled allocation stayed busy", status, data)
            time.sleep(.02)
        assert status == 200 and b"event: done" in data and b"event: error" not in data, (status, data)
        cli("revoke", "alice")
        assert request("/api/v1/models", alice)[0] == 401
        assert request("/api/v1/models", bob)[0] == 200
        print("RESIDENT API: PASS (real OLMoE, approved allocations, distinct users, private history, TUI coexistence, manager restart, streaming, repeated close, cancellation, revoke)", flush=True)
    finally:
        cli("stop")
        assert not service("status")["api"]["live"]
        source = Path(env["HOME"]) / ".lumabri/service/api.log"
        if source.exists():
            shutil.copyfile(source, artifacts / "resident-api.log")
