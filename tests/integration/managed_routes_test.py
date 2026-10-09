"""Real resident replica selection, invoked inside the approved household flow.
No fake inference or implicit preparation. Optional bounded replay fault gate.
"""
import http.client
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import select
import socket
import struct
import subprocess
import threading
import time


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
    # The third real allocation changes contention on the SAME donors. A
    # successful two-model envelope is now stale even if its own weights and
    # intervals have not moved. No sleeps or invented timing replacements.
    previous = json.loads(command("capacity-check", "joint-baseline", "30000", "30000", "300"))
    assert previous["state"] == "configuration_changed", previous
    command("model-add", "wrong-checkpoint", first["id"], other["id"], ok=False)
    command("model-add", "duplicate", first["id"], first["id"], ok=False)
    assert len(json.loads(command("list"))["models"]) == 3, "failed registration saved a partial route"
    route = json.loads(command("model-add", "olmoe-managed", first["id"], third["id"]))
    assert route["replicas"] == 2 and route["allocations"] == [first["id"], third["id"]]
    assert route["sessions"] is None and route["state"] == "saved_route" and route["revision"] == 1
    assert route["policy"] == "ordered"
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

    def observe_replica(label):
        status, data = chat(route["id"])
        (artifacts / ("route-observation-" + label + ".log")).write_bytes(data)
        assert status == 200 and b"event: error\n" not in data, (label, status, data)
        done = [json.loads(block.split(b"\ndata: ", 1)[1])
                for block in data.split(b"\n\n") if block.startswith(b"event: done\n")]
        # A successful replay is NOT a measurement of this replica. In
        # particular, macOS may compress an idle process on a shared runner;
        # the residency guard must refuse it, not silently relax its policy.
        # Fail at the unmet prerequisite and preserve the actual SSE evidence,
        # instead of later reporting a mysterious missing policy observation.
        assert len(done) == 1 and done[0]["recovery_attempts"] == 0 and done[0]["observation_saved"], (
            label, "replica did not produce a saved no-replay observation; inspect resident-memory logs", status, data)

    def release(plan):
        result = subprocess.run([str(runtime / "test_chat_ui"), "resident-release", str(plan), tracker],
                                env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr

    held = []
    def hold(allocation):
        process = subprocess.Popen([str(runtime / "test_chat_ui"), "api-hold-allocation",
                                    str(Path(env["HOME"]) / ".lumabri/api-access"), allocation],
                                   env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        held.append(process)
        assert select.select([process.stdout], [], [], 5)[0], "allocation permit not acquired"
        assert process.stdout.readline() == b"ADMITTED\n", process.stderr.read()
        return process

    def unhold(process):
        process.stdin.close(); assert process.wait(timeout=5) == 0
        process.stdout.close(); process.stderr.close(); held.remove(process)

    try:
        command("start", "--port", str(port))
        status, data = request("/api/v1/models")
        assert status == 200 and [m["id"] for m in json.loads(data)["models"]] == [route["id"]]
        assert b"content_id" not in data and b"allocations" not in data, "operator metadata leaked"
        status, data = request("/api/v1/models", bob)
        assert status == 200 and json.loads(data) == {"schema": 1, "models": []}
        for member in (first, third, other):
            assert chat(member["id"])[0] == 403, "logical grant authorized a raw allocation"
        control_path = "/api/v1/allocations/" + first["id"] + "/control"
        assert request(control_path)[0] == 403
        assert request(control_path, body={"action": "retire"})[0] == 403
        cli("operator-grant", "route-bob")
        assert request(control_path, bob)[0] == 200
        assert request(control_path, bob, {"action": "retire"})[0] == 403, "read-only operator gained control"
        cli("manage-grant", "route-bob")
        assert json.loads(request("/api/v1/session", bob)[1])["management"] is True
        assert request("/api/v1/chat", bob, {"model": first["id"], "max_tokens": 8,
                       "messages": [{"role": "user", "content": "hi"}]})[0] == 403, "management granted inference"
        assert request(control_path, bob, {"action": "retire"})[0] == 400
        observed = json.loads(request(control_path, bob)[1])
        assert isinstance(observed["revision"], str), "64-bit revision lost precision in JSON"
        stale = {"action": "retire", "instance": observed["instance"], "revision": str(int(observed["revision"]) + 100)}
        assert request(control_path, bob, stale)[0] == 409
        assert request(control_path, bob, {**stale, "action": "drain"})[0] == 409
        assert json.loads(request(control_path, bob)[1])["state"] == "accepting"
        cli("manage-revoke", "route-bob")
        assert request(control_path, bob, stale)[0] == 403
        cli("manage-grant", "route-bob")
        observe_replica("first")
        first_busy = hold(first["id"])
        observe_replica("alternate")
        # Both actual replicas have now produced matching-length observations
        # under this resident workload. Preferences must use them, without
        # changing allocation permissions or treating missing prices as free.
        policy = json.loads(command("model-policy", route["id"], "observed-decode"))
        assert policy["revision"] == 3 and policy["policy"] == "observed-decode"
        command("model-policy", route["id"], "made-up", ok=False)
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data, (status, data)
        policy_log = Path(env["HOME"]) / ".lumabri/service/api.log"
        assert "evidence=recent_matching_decode_observations" in policy_log.read_text(), policy_log.read_text()
        policy = json.loads(command("model-policy", route["id"], "declared-cost"))
        assert policy["revision"] == 4 and policy["policy"] == "declared-cost"
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data, (status, data)
        assert "evidence=prices_missing_or_mixed_currency" in policy_log.read_text()
        assert json.loads(command("model-policy", route["id"], "ordered"))["revision"] == 5
        third_busy = hold(third["id"])
        status, data = chat(route["id"])
        assert status == 429 and json.loads(data)["error"] == "replicas_busy", (status, data)
        # A second alias must not bypass the exact-allocation limit.
        alias = json.loads(command("model-add", "olmoe-same-capacity", first["id"], third["id"]))
        command("grant", "route-alice", alias["id"])
        assert chat(alias["id"])[0] == 429
        command("grant", "route-alice", first["id"])
        assert chat(first["id"])[0] == 429, "raw allocation bypassed its existing permit"
        command("model-remove", alias["id"])
        unhold(first_busy); unhold(third_busy)
        # Drain is enforced by the actual host, not merely this HTTP gateway:
        # aliases and direct Hosted clients share the same admission boundary.
        def drain(member):
            initial = json.loads(command("replica", member["id"], "status"))
            assert initial["state"] == "accepting" and not initial["weights_unloaded"]
            path = "/api/v1/allocations/" + member["id"] + "/control"
            mutation = {"action": "drain", "instance": initial["instance"], "revision": str(initial["revision"])}
            status, data = request(path, bob, mutation)
            assert status == 200, data
            changed = json.loads(data)
            assert changed["instance"] == initial["instance"] and int(changed["revision"]) == initial["revision"] + 1
            assert request(path, bob, mutation)[0] == 200, "lost-reply retry was not idempotent"
            deadline = time.monotonic() + 5
            while changed["state"] != "drained":
                assert time.monotonic() < deadline, changed
                time.sleep(.05)
                changed = json.loads(command("replica", member["id"], "status"))
            assert changed["connections"] == changed["admitted_requests"] == 0
            return changed

        before_log = len(policy_log.read_text())
        first_drained = drain(first)
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data and b"event: error\n" not in data, (status, data)
        assert "allocation=" + third["id"] in policy_log.read_text()[before_log:], "draining replica admitted new inference"
        third_drained = drain(third)
        status, data = chat(route["id"])
        assert status == 429 and json.loads(data)["error"] == "replicas_busy", (status, data)
        for member, previous in ((first, first_drained), (third, third_drained)):
            resumed = json.loads(command("replica", member["id"], "resume"))
            assert resumed["state"] == "accepting" and resumed["instance"] == previous["instance"]
            assert resumed["revision"] == int(previous["revision"]) + 1 and not resumed["weights_unloaded"]
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data and b"event: error\n" not in data, (status, data)
        print("MANAGED DRAIN: PASS (real resident hosts refuse new turns, alternate approved replica, no capacity, resume same engines)", flush=True)
        # Two actual gateway workers generate concurrently, not just fixture
        # reservations. A barrier aligns submission; each model instance has
        # its own conversation state and exact-allocation admission permit.
        for _ in range(4):
            barrier = threading.Barrier(2)
            def concurrent_chat(_index):
                barrier.wait(timeout=5)
                return chat(route["id"])
            with ThreadPoolExecutor(max_workers=2) as pool:
                replies = list(pool.map(concurrent_chat, range(2)))
            assert all(status == 200 and b"event: done\n" in data and b"event: error\n" not in data
                       for status, data in replies), replies
        saved_status, saved_data = request("/api/v1/conversations", body={"revision": 0, "conversation": {
            "model": route["id"], "title": "Replica availability", "state": "pending",
            "messages": [{"role": "user", "content": "hi"}]}})
        assert saved_status == 200, saved_data
        conversation_id = json.loads(saved_data)["id"]
        if env.get("LUMABRI_TEST_RECOVERY_CONTROL"):
            from managed_recovery_test import verify_recovery
            verify_recovery(env, first, third, route, chat, request, cli, command, port, alice)
        release(first_plan)
        # The preferred allocation is gone. A fresh request must succeed on
        # the independently approved replica, with no reapproval or reload.
        status, data = chat(route["id"])
        assert status == 200 and b"event: done\n" in data and b"event: error\n" not in data, (status, data)
        reduced = json.loads(command("model-set", route["id"], third["id"]))
        assert reduced["revision"] == 6 and reduced["replicas"] == 1 and reduced["policy"] == "ordered"
        # Only the coordinated path may claim a non-interrupting release.
        # It seals the host, retires its retained KV, seals both real nodes,
        # and persists acknowledgements before freeing the Edge keeper last.
        deadline=time.monotonic()+30
        path = "/api/v1/allocations/" + third["id"] + "/control"
        connected = subprocess.Popen([str(runtime / "test_chat_ui"), "resident-hold", str(third_plan), tracker],
                                     env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        held.append(connected)
        assert select.select([connected.stdout], [], [], 10)[0], "host connection was not opened"
        assert connected.stdout.readline() == b"CONNECTED\n"
        fence = json.loads(request(path, bob)[1])
        assert fence["connections"] == 1 and fence["admitted_requests"] == 0
        retirement_request = {"action": "retire", "instance": fence["instance"], "revision": fence["revision"]}
        status, data = request(path, bob, retirement_request)
        # An idle connection is not admitted inference: host drain closes it
        # without waiting for user input. Depending on observation timing the
        # retirement is already complete or needs another explicit reconcile.
        # Deterministic active/queued work retention lives in test_host_drain.
        assert status in (200, 202), (status, data)
        assert json.loads(data)["state"] in ("released", "retirement_pending")
        assert json.loads(request(path, bob)[1])["state"] in ("released", "retirement_pending")
        assert request(path, bob, {**retirement_request, "action": "resume"})[0] == 409
        unhold(connected)
        while True:
            status, data = request(path, bob, retirement_request)
            assert status in (200, 202), (status, data)
            retired=json.loads(data)
            if retired["complete"]: break
            assert retired["state"]=="retirement_pending" and time.monotonic()<deadline, retired
            time.sleep(.05)
        assert retired["state"]=="released" and retired["released_node_mask"]==3
        assert json.loads(request(path, bob, retirement_request)[1])==retired, "completed retirement replayed destructive work"
        assert json.loads(request(path, bob)[1])==retired, "inspection did not read the completed journal"
        cli_retired=json.loads(command("retire",third["id"]))
        assert all(retired[key]==value for key,value in cli_retired.items()), "CLI and API used different retirement state"
        tui_retired = subprocess.run([str(runtime / "test_chat_ui"), "resident-retire", str(third_plan), tracker],
                                    env=env, text=True, capture_output=True, timeout=15)
        assert tui_retired.returncode == 0 and tui_retired.stdout.strip() == "0", tui_retired.stderr
        assert request(path, bob, {**retirement_request, "action": "resume"})[0] == 409
        # Simulate a coordinator dying after keeper release but before its
        # acknowledgement was saved. Retain the exact live-process seals;
        # rewind only the local release-progress fields, not donor state.
        journal=Path(env["HOME"])/".lumabri/api-access/retirements"/(third["id"]+".retire")
        checkpoint=bytearray(journal.read_bytes())
        assert checkpoint[:8]==b"LMBRET1\0" and struct.unpack_from("<I",checkpoint,80)[0]==2
        struct.pack_into("<IIII",checkpoint,72,4,0,2,0)  # RELEASE, no saved acknowledgements
        journal.write_bytes(checkpoint)
        reconciled=json.loads(command("retire",third["id"]))
        assert reconciled==cli_retired, ("lost acknowledgements were not reconciled",reconciled)
        print("COORDINATED RETIREMENT: PASS (host and two nodes sealed, retained KV closed, exact release, lost-ack reconciliation)",flush=True)
        status, data = chat(route["id"])
        assert status == 503 and json.loads(data)["error"] == "approved_allocation_unavailable", (status, data)
        command("model-remove", route["id"])
        assert chat(route["id"])[0] == 404
        assert request("/api/v1/conversations/" + conversation_id)[0] == 200, "route removal lost history"
        assert request("/api/v1/conversations/" + conversation_id, bob)[0] == 404
        print("MANAGED ROUTES: PASS (real OLMoE replicas, matching observations, explicit preferences, unknown costs, immutable contracts, stable grants, bounded shared admission, selective loss, private history)", flush=True)
    finally:
        for process in held[:]:
            unhold(process)
        cli("stop")
