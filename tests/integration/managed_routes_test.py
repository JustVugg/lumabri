"""Real resident replica selection, invoked inside the approved household flow.
No fake inference, no implicit preparation, no automatic generation replay.
"""
import http.client
import json
import socket
import struct
import subprocess


def verify_managed_routes(runtime, env, tracker, first_plan, third_plan, first_name, artifacts):
    def root(plan):
        data = plan.read_bytes(); offset = 4; strings = []
        for _ in range(4):
            size = struct.unpack_from("<H", data, offset)[0]; offset += 2
            strings.append(data[offset:offset + size].decode()); offset += size
        return strings[3]
    assert root(first_plan) != root(third_plan), "replicas reused one signed allocation root"
    def cli(*args, ok=True):
        result = subprocess.run([str(runtime / "lumabri"), "api", *args], env=env,
                                text=True, capture_output=True, timeout=40)
        assert (result.returncode == 0) == ok, (args, result.stderr, result.stdout)
        return result.stdout.strip()

    def command(*args, ok=True):
        return cli(*args, "--tracker", tracker, ok=ok)

    models = json.loads(command("list"))["models"]
    assert len(models) == 3, models
    first = next(m for m in models if m["name"] == first_name)
    third = next(m for m in models if "replica-checkpoint" in m["name"])
    other = next(m for m in models if m["id"] not in (first["id"], third["id"]))
    command("model-add", "wrong-checkpoint", first["id"], other["id"], ok=False)
    command("model-add", "duplicate", first["id"], first["id"], ok=False)
    assert len(json.loads(command("list"))["models"]) == 3, "failed registration saved a partial route"
    route = json.loads(command("model-add", "olmoe-managed", first["id"], third["id"]))
    assert route["replicas"] == 2 and route["allocations"] == [first["id"], third["id"]]
    assert route["sessions"] is None and route["state"] == "saved_route" and route["revision"] == 1
    assert route["numeric_class"] and route["adapter"] == "olmoe" and len(route["content_id"]) == 64
    command("model-set", route["id"], other["id"], ok=False)
    command("model-add", "olmoe-managed", third["id"], ok=False)
    updated = json.loads(command("model-set", route["id"], first["id"], third["id"]))
    assert updated["revision"] == 2 and updated["id"] == route["id"]
    (artifacts / "managed-model.json").write_text(json.dumps(updated, indent=2))
    alice, bob = cli("user-add", "route-alice"), cli("user-add", "route-bob")
    command("grant", "route-alice", route["id"])
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0)); port = reserve.getsockname()[1]

    def request(path, token=alice, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=40)
        headers = {"Authorization": "Bearer " + token}
        if body is not None:
            headers["Content-Type"] = "application/json"
        conn.request("POST" if body is not None else "GET", path, json.dumps(body) if body is not None else None, headers)
        response = conn.getresponse(); result = response.status, response.read()
        conn.close(); return result

    def chat(model):
        return request("/api/v1/chat", body={"model": model, "max_tokens": 8,
                                            "messages": [{"role": "user", "content": "hi"}]})

    def release(plan):
        result = subprocess.run([str(runtime / "test_chat_ui"), "resident-release", str(plan), tracker],
                                env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr

    try:
        command("start", "--port", str(port))
        status, data = request("/api/v1/models")
        assert status == 200 and [m["id"] for m in json.loads(data)["models"]] == [route["id"]]
        assert b"content_id" not in data and b"allocations" not in data, "operator metadata leaked"
        status, data = request("/api/v1/models", bob)
        assert status == 200 and json.loads(data) == {"schema": 1, "models": []}
        for member in (first, third, other):
            assert chat(member["id"])[0] == 403, "logical grant authorized a raw allocation"
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data and b"event: error\n" not in data, (status, data)
        saved_status, saved_data = request("/api/v1/conversations", body={"revision": 0, "conversation": {
            "model": route["id"], "title": "Replica availability", "state": "pending",
            "messages": [{"role": "user", "content": "hi"}]}})
        assert saved_status == 200, saved_data
        conversation_id = json.loads(saved_data)["id"]
        release(first_plan)
        # The preferred allocation is gone. A fresh request must succeed on
        # the independently approved replica, with no reapproval or reload.
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data and b"event: error\n" not in data, (status, data)
        reduced = json.loads(command("model-set", route["id"], third["id"]))
        assert reduced["revision"] == 3 and reduced["replicas"] == 1
        release(third_plan)
        status, data = chat(route["id"])
        assert status == 503 and json.loads(data)["error"] == "approved_allocation_unavailable", (status, data)
        command("model-remove", route["id"])
        assert chat(route["id"])[0] == 404
        assert request("/api/v1/conversations/" + conversation_id)[0] == 200, "route removal lost history"
        assert request("/api/v1/conversations/" + conversation_id, bob)[0] == 404
        print("MANAGED ROUTES: PASS (real OLMoE replicas, immutable checkpoint/numeric contract, stable grant, selective loss before submission, bounded unavailable, private history)", flush=True)
    finally:
        cli("stop")
