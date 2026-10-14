"""Real resident OLMoE replica lifecycle. Reuses the authenticated household
fixture, not a mocked planner/engine. Contention is controlled through the
same allocation permit used by the gateway so tiny fast turns cannot race
past the pressure probe. No cloud resources, costs or throughput claims.
"""
import fcntl
import http.client
import json
import re
import select
from pathlib import Path
import subprocess
import time


def verify_elastic(api, until, service, env, terminal, artifacts):
    api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
    path = "/api/v1/elastic-policy"
    status, workspace = api.http("/api/v1/workspace")
    assert status == 200
    first = next(a for a in reversed(workspace["allocations"]) if a["name"] == api.reloaded_name)
    other = next(a for a in workspace["allocations"] if a["name"].endswith("-model-b"))
    route = json.loads(api.cli("model-add", "elastic-olmoe", first["id"], "--tracker", api.tracker))
    second = json.loads(api.cli("model-add", "elastic-other", other["id"], "--tracker", api.tracker))
    token = api.cli("user-add", "elastic-user")
    for r in (route, second):
        api.cli("grant", "elastic-user", r["id"])
    config = {"action": "configure", "id": route["id"], "revision": 0, "route_revision": route["revision"],
              "enabled": False, "min_replicas": 1, "max_replicas": 2, "cooldown_seconds": 30,
              "idle_seconds": 60, "up_rejections": 2, "ttft_ms": 0, "max_reserved_bytes": str(4 << 30),
              "source_model": "model-a", "nodes": api.start_request["nodes"],
              "context": 128, "sessions": api.start_request["sessions"], "max_new": 8,
              "adopt_allocations": [first["id"]]}

    def chat(model):
        c = http.client.HTTPConnection("127.0.0.1", api.port, timeout=40)
        c.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
                  "messages": [{"role": "user", "content": "hello elastic service"}]}),
                  {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        res = c.getresponse(); result = res.status, res.read(); c.close()
        return result

    def infer(model):
        code, data = chat(model)
        assert code == 200 and b"event: done\n" in data and b"event: error\n" not in data, (code, data)
        return data

    def policy():
        code, data = api.http(path)
        assert code == 200, data
        return next(p for p in data["policies"] if p["id"] == route["id"])

    def await_policy(predicate, message, seconds=180, approve=False):
        last = None
        def observed():
            nonlocal last
            if approve:
                for name in ("a", "b"):
                    donor = service(name)["donor"]
                    if donor["phase"] == 1:
                        def owner(*args):
                            p = subprocess.run([str(api.runtime / "lumabri"), "donor-policy", *args],
                                               env=env(name), text=True, capture_output=True, timeout=25)
                            assert p.returncode == 0, p.stderr
                            return json.loads(p.stdout)
                        reviewed = owner("inspect")
                        assert donor["model"].endswith("-model-a"), donor
                        # Explicit fixture-owner authorization. The controller
                        # itself cannot grant or expand this repeat permission.
                        granted = owner("allow-current", reviewed["review"], "3600", "2")
                        assert granted["max_allocations"] == 2
            last = policy()
            assert not last["faulted"], last
            return predicate(last)
        try:
            until(observed, message, seconds)
        except AssertionError as exc:
            raise AssertionError((message, last)) from exc
        return last

    def allocation_lock(alloc):
        # Unlocked fixed cells are reused by other allocations. Materialize
        # this allocation's hint immediately before taking the real permit.
        infer(route["id"])
        dispatch = Path(api.env["HOME"]) / ".lumabri/api-access/dispatch"
        matches = [p for p in dispatch.glob("*.lease") if p.read_bytes() == bytes.fromhex(alloc)]
        assert len(matches) == 1, matches
        held = matches[0].open("r+b")
        deadline = time.monotonic() + 5
        while True:
            try:
                fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                assert time.monotonic() < deadline, "completed turn did not release gateway permit"
                time.sleep(.01)
        return held

    events = {}; held_chat = None; permits = []
    try:
        assert api.http(path, config, api.reader)[0] == 403
        assert api.http(path, token=token)[0] == 403
        assert api.http(path, {**config, "revision": 1})[0] == 409
        assert api.http(path, {**config, "all_nodes": True})[0] == 400
        code, configured = api.http(path, config)
        assert code == 200 and configured["revision"] == 1 and not configured["enabled"], (code, configured)
        events["configured"] = configured
        infer(route["id"]); infer(second["id"])
        code, enabled = api.http(path, {**config, "revision": 1, "enabled": True})
        assert code == 200 and enabled["revision"] == 2, (code, enabled)
        # Installing the older routing controller second must not create
        # another autonomous writer for the same logical model.
        portfolio = {"action": "configure", "revision": 0, "records": ["one", "two"], "current": 0,
                     "models": [{"id": route["id"], "revision": route["revision"]}],
                     "ttft_ms": 30000, "gap_ms": 30000, "max_age_seconds": 3600,
                     "cooldown_seconds": 60, "horizon_seconds": 3600, "min_saving_bps": 1000,
                     "currency": "EUR", "ceiling_micro_per_hour": "1000000", "switch_cost_micro": "0", "enabled": True}
        assert api.http("/api/v1/portfolio-policy", portfolio)[0] == 409
        library = Path(api.env["HOME"]) / ".lumabri/resident-plans"
        before = set(library.glob("*.plan"))
        with allocation_lock(first["id"]):
            for _ in range(3):
                code, busy = chat(route["id"])
                assert code == 429 and b"replicas_busy" in busy, (code, busy)
        grown = await_policy(lambda p: p["replicas"] == 2 and p["phase"] == "idle",
                             "demand did not prepare/publish a second replica", approve=True)
        events["grown"] = grown
        print("ELASTIC: second OLMoE replica prepared and published", flush=True)
        current = next(m for m in json.loads(api.cli("list", "--tracker", api.tracker))["models"] if m["id"] == route["id"])
        assert len(current["allocations"]) == len(set(current["allocations"])) == 2
        added, = set(current["allocations"]) - {first["id"]}
        new_plan, = set(library.glob("*.plan")) - before
        with allocation_lock(first["id"]):
            infer(route["id"])  # must execute on the newly prepared replica
        infer(second["id"])
        # The direct TUI is outside this gateway's traffic observer. The
        # final live drain must protect its admitted generation regardless.
        held_chat = terminal(new_plan)
        until(lambda: held_chat.has("receives the text"), "new replica TUI connection unavailable")
        connected = json.loads(api.cli("replica", added, "--tracker", api.tracker))
        assert connected["connections"] >= 1, connected
        api.cli("stop"); api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
        restarted = policy()
        assert restarted["replicas"] == 2 and restarted["route_revision"] == grown["route_revision"]
        events["restarted"] = restarted
        registry = Path(api.env["HOME"]) / ".lumabri/api-access/models/access.lock"
        with registry.open("r+b") as guard:
            # Hold the real publication lock while a withdrawal is selected,
            # so even a slow test runner can admit the direct TUI turn before
            # drain. Production timers, planner and inference stay unchanged.
            fcntl.flock(guard, fcntl.LOCK_EX)
            await_policy(lambda p: p["phase"] == "withdrawing_replica", "idle replica not selected", seconds=210)
            for name in ("a", "b"):
                held = subprocess.Popen([str(api.runtime / "test_chat_ui"), "compute-hold"], env=env(name),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                permits.append(held)
                assert select.select([held.stdout], [], [], 10)[0], "CPU permit was not granted"
                assert held.stdout.readline().strip() == "COMPUTE HELD"
            held_chat.send("hello while replica drains\n")
            until(lambda: json.loads(api.cli("replica", added, "--tracker", api.tracker))["admitted_requests"] >= 1,
                  "direct TUI turn was not admitted", 10)
        waiting = await_policy(lambda p: p["replicas"] == 1 and p["phase"] == "retiring" and
                               p["reason"] == "draining_admitted_work", "admitted generation did not delay retirement", seconds=20)
        assert waiting["reserved_bytes"] == grown["reserved_bytes"], waiting
        # A subsequent query can race the socket idle timeout. Check the
        # exact instance's drain refusal, not a later unrelated observation.
        logs = "\n".join((Path(env(n)["HOME"]) / ".lumabri/home/engines.log").read_text() for n in ("a", "b"))
        assert re.search(r"action=retire instance=" + connected["instance"] + r"[^\n]*conflict=1 connections=[1-9][^\n]*admitted=[1-9]", logs)
        for held in permits:
            held.communicate("release\n", timeout=5); assert held.returncode == 0
        permits.clear()
        until(lambda: held_chat.has("hosted stream"), "admitted turn did not finish before retirement", 30)
        print("ELASTIC: withdrawal retained memory until an admitted real generation completed", flush=True)
        infer(second["id"])
        held_chat.send("/quit\n"); until(lambda: held_chat.p.poll() is not None, "draining TUI did not close")
        held_chat = None
        released = await_policy(lambda p: p["phase"] == "idle" and p["replicas"] == 1,
                                "drained replica not released")
        assert int(released["reserved_bytes"]) < int(waiting["reserved_bytes"])
        events["waiting"] = waiting; events["released"] = released
        print("ELASTIC: idle replica released; other model still serves", flush=True)
        infer(route["id"]); infer(second["id"])
        # Scale to zero preserves the model ID, numeric contract and grants.
        cold_config = {**config, "enabled": True, "revision": released["revision"],
                       "route_revision": released["route_revision"], "min_replicas": 0}
        code, cold = api.http(path, cold_config)
        assert code == 200, (code, cold)
        dormant = await_policy(lambda p: p["replicas"] == 0 and p["phase"] == "idle", "on-demand model did not become dormant", 240)
        assert dormant["reserved_bytes"] == "0"
        listed = next(m for m in json.loads(api.cli("list", "--tracker", api.tracker))["models"] if m["id"] == route["id"])
        assert listed["state"] == "dormant" and listed["content_id"] == route["content_id"]
        infer(second["id"])
        # This version reports preparation, not an imaginary queued response.
        code, cold_reply = chat(route["id"])
        assert code == 503 and b"model_dormant_preparation_required" in cold_reply, (code, cold_reply)
        waking = await_policy(lambda p: p["phase"] == "preparing", "cold request did not wake preparation", 90)
        operation = waking["pending_operation"]
        intents = set((Path(api.env["HOME"]) / ".lumabri/api-access/preparations").glob("*.intent"))
        api.cli("stop"); api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
        awake = await_policy(lambda p: p["phase"] == "idle" and p["replicas"] == 1, "cold model did not return after API restart", approve=True)
        # The phase is journalled before the first intent write. A restart
        # in that interval may create that same intent, but never another ID.
        expected = intents | {Path(api.env["HOME"]) / ".lumabri/api-access/preparations" / (operation + ".intent")}
        assert set((Path(api.env["HOME"]) / ".lumabri/api-access/preparations").glob("*.intent")) == expected
        assert api.http("/api/v1/preparation", {"action": "status", "operation": operation})[1]["state"] == "done"
        infer(route["id"]); infer(second["id"])
        events.update(dormant=dormant, waking=waking, awake=awake)
        # Explicit manual routing always stops the controller's old ownership
        # fence; no following tick may silently undo this operator revision.
        api.cli("model-policy", route["id"], "ordered", "--tracker", api.tracker)
        until(lambda: policy()["faulted"], "manual route revision did not stop controller", 30)
        assert policy()["reason"] == "route_changed_operator_review_required"
        events["manual_change"] = policy()
        (artifacts / "elastic-replicas.json").write_text(json.dumps(events, indent=2))
        print("ELASTIC REPLICAS: PASS (real OLMoE pressure -> approved prepare -> publish -> infer, independent model, restart, admitted generation drain, release, scale to zero, demand wake, exact operation reuse, manual-edit fence)", flush=True)
    finally:
        for held in permits:
            held.communicate("release\n", timeout=5)
        if held_chat:
            held_chat.send("/quit\n")
        observed = policy()
        if observed["enabled"]:
            def disable():
                current = policy()
                if not current["enabled"]:
                    return True
                code, reply = api.http(path, {"action": "disable", "id": route["id"], "revision": current["revision"]})
                assert code in (200, 409, 503), (code, reply)
                return code == 200
            until(disable, "controller did not release its configuration guard", 30)
        api.cli("stop")
