"""Two real model placements, not assembled per-model speed estimates.

All donors run on loopback. Their prices are explicit test declarations, not
cloud bills. Record the complete experiment, including retained allocations
and post-retirement invalidation; never call routing alone a realized saving.
"""
import http.client
import json
import os
import secrets
import subprocess
import socket


def open_probe(api, name, allocations, clients=2, rounds=3):
    connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=40)
    connection.request("POST", "/api/v1/capacity/measure", json.dumps({"record": name,
        "clients": clients, "rounds": rounds, "max_tokens": 8, "allocations": [a["id"] for a in allocations]}),
        {"Authorization": "Bearer " + api.token, "Content-Type": "application/json"})
    response = connection.getresponse()
    assert response.status == 200, (response.status, response.read())
    assert response.getheader("Content-Type") == "text/event-stream"
    return connection, response


def probe_events(response):
    event = None
    while True:
        line = response.readline()
        if not line:
            return
        if line.startswith(b"event: "):
            event = line[7:].strip().decode()
        elif line.startswith(b"data: "):
            yield event, json.loads(line[6:])


def check_probe(api, name):
    code, checked = api.http("/api/v1/capacity/check", {"record": name,
        "ttft_ms": 30000, "gap_ms": 30000, "max_age_seconds": 3600})
    assert code == 200, checked
    return checked


def verify_placement_comparison(api, until, service, env, approve, inventory_ready, artifacts):
    api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
    names = api.start_request["models"]
    code, workspace = api.http("/api/v1/workspace")
    assert code == 200
    identities = {}
    for name in ("a", "b"):
        identities[name] = subprocess.run([str(api.runtime / "lumabri"), "peer-key"], env=env(name),
            capture_output=True, text=True, check=True, timeout=15).stdout.strip()
    # The catalogue names include a hyphen; do not infer a model from order or
    # from a machine's display name. Match the exact reviewed name suffix.
    managed = [next(a for a in workspace["allocations"] if a["name"].endswith("-" + name)) for name in names]
    assert len({a["id"] for a in managed}) == 2
    assert all({r["node"] for r in a["ranges"]} == {identities["a"]} for a in managed), managed
    assert service("a")["donor"]["model_count"] == 2 and service("b")["donor"]["model_count"] == 0
    static = []
    preparations = []
    for name, owner in zip(names, ("b", "a")):
        intent = {"models": [name], "nodes": [identities[owner]], "context": 128, "sessions": 2, "max_new": 8}
        code, preview = api.http("/api/v1/preparation", {"action": "preview", **intent})
        assert code == 200 and preview["plan"]["requires_approval"], preview
        operation = secrets.token_hex(32)
        request = {"action": "start", **intent, "operation": operation, "review": preview["review"]}
        code, started = api.http("/api/v1/preparation", request)
        assert code == 202, started
        state = None
        def complete():
            nonlocal state
            approve()  # Explicit fixture owner accepts only the displayed offer.
            code, state = api.http("/api/v1/preparation", {"action": "status", "operation": operation})
            assert code == 200 and state["state"] != "failed", state
            return state["state"] == "done"
        until(complete, "static comparison placement did not prepare", 180)
        assert state["results_known"] and len(state["allocations"]) == 1, state
        allocation = state["allocations"][0]
        assert allocation["name"] == name and allocation["state"] == "ready_observed", state
        code, workspace = api.http("/api/v1/workspace")
        assert code == 200
        actual = next(a for a in workspace["allocations"] if a["id"] == allocation["id"])
        assert {r["node"] for r in actual["ranges"]} == {identities[owner]}, actual
        static.append(actual)
        preparations.append(state)
        inventory_ready()
    assert service("a")["donor"]["model_count"] == 3 and service("b")["donor"]["model_count"] == 1
    before = {name: service(name)["donor"]["reserved_total_bytes"] for name in ("a", "b")}
    code, demand_before = api.http("/api/v1/demand")
    assert code == 200
    # Cancel only this explicitly requested calibration. The last incomplete
    # record remains distinguishable; neither approved model is unloaded.
    connection, response = open_probe(api, "placement-cancelled", managed, rounds=16)
    try:
        event, progress = next(probe_events(response))
        assert event == "progress" and progress["completed_rounds"] == 0
        code, busy = api.http("/api/v1/capacity/measure", {"record": "placement-competing", "clients": 2,
            "rounds": 3, "max_tokens": 8, "allocations": [a["id"] for a in managed]})
        assert code == 409 and busy["error"] == "capacity_probe_not_started", busy
        # HTTPConnection drops its socket reference for Connection: close;
        # the response owns it. Shutdown the actual stream, not just its file.
        response.fp.raw._sock.shutdown(socket.SHUT_RDWR)
    finally:
        response.close(); connection.close()
    cancelled = None
    def cancellation_saved():
        nonlocal cancelled
        code, listed = api.http("/api/v1/capacity")
        assert code == 200 and listed["scope"] == "historical_not_revalidated", listed
        cancelled = next((r for r in listed["records"] if r["name"] == "placement-cancelled"), None)
        return cancelled is not None
    until(cancellation_saved, "cancelled probe was not cleaned up", 20)
    assert not cancelled["observation"]["stable"] and cancelled["observation"]["state"] == "incomplete", cancelled
    code, removed = api.http("/api/v1/capacity/remove", {"record": "placement-cancelled"})
    assert code == 200 and removed["removed"] and not removed["weights_changed"], removed
    assert {name: service(name)["donor"]["reserved_total_bytes"] for name in ("a", "b")} == before

    def measure(label, allocations, clients=2, require_pass=True):
        # The owner probe uses real gateway admission and per-response timing.
        # All four allocations already coexist before EITHER measurement.
        connection, response = open_probe(api, label, allocations, clients)
        try:
            events = list(probe_events(response))
        finally:
            response.close(); connection.close()
        assert [data["completed_rounds"] for event, data in events if event == "progress"] == [0, 1, 2, 3], events
        results = [data for event, data in events if event == "result"]
        assert len(results) == 1 and events[-1][0] == "result", events
        record = results[0]
        assert record["stable"] and record["state"] == "measured", record
        assert all(m["requests"] == clients*3//2 and
                   m["completed"]+m["rejected"]+m["failed"] == m["requests"] and
                   m["timing_samples"] == m["completed"] and not m["failed"] for m in record["models"]), record
        checked = check_probe(api, label)
        rejected = any(m["rejected"] for m in record["models"])
        assert checked["state"] == ("requests_rejected_or_failed" if rejected else "observed_workload_passed"), checked
        if require_pass:
            assert not rejected, record
        return record

    # Engine conversation slots do not enlarge the gateway's one active-turn
    # permit per allocation. Keep every overload outcome; do not censor BUSY
    # samples or reinterpret per-engine KV slots as throughput capacity.
    overload = {"static": measure("placement-static-overload", static, 4, False),
                "managed": measure("placement-managed-overload", managed, 4, False)}
    baseline = measure("placement-static", static)
    alternative = measure("placement-managed", managed)
    assert baseline["cost"]["micro_per_hour"] == 400000, baseline
    assert alternative["cost"]["micro_per_hour"] == 100000, alternative
    code, selected = api.http("/api/v1/capacity/select", {"ttft_ms": 30000, "gap_ms": 30000,
        "max_age_seconds": 3600, "records": ["placement-static", "placement-managed"]})
    assert code == 200
    assert selected == json.loads(api.cli("capacity-select", "30000", "30000", "3600", "placement-static", "placement-managed"))
    assert selected["selected"] == "placement-managed" and not selected["applied"], selected
    assert all(c["state"] == "observed_workload_passed" for c in selected["candidates"]), selected
    code, demand_after = api.http("/api/v1/demand")
    assert code == 200 and demand_after["models"] == demand_before["models"] == [], demand_after
    assert demand_after["telemetry_gaps"] == demand_before["telemetry_gaps"] == 0, demand_after
    if os.environ.get("LUMABRI_TEST_BROWSER") == "1":
        from capacity_browser_flow import verify_browser_capacity
        verify_browser_capacity(api, managed, static, artifacts)
    routes = [json.loads(api.cli("model-add", "comparison-" + name, a["id"], "--tracker", api.tracker))
              for name, a in zip(names, static)]
    config = {"action": "configure", "revision": 0, "enabled": True,
              "records": ["placement-static", "placement-managed"], "current": 0,
              "models": [{"id": r["id"], "revision": r["revision"]} for r in routes],
              "ttft_ms": 30000, "gap_ms": 30000, "max_age_seconds": 3600, "cooldown_seconds": 30,
              "horizon_seconds": 3600, "min_saving_bps": 1000, "currency": "EUR",
              "ceiling_micro_per_hour": "1000000", "switch_cost_micro": "1000"}
    path = "/api/v1/portfolio-policy"
    code, configured = api.http(path, config)
    assert code == 200 and configured["enabled"], configured
    applied = None
    def switched():
        nonlocal applied
        code, applied = api.http(path)
        assert code == 200 and not applied["faulted"], applied
        return applied["current"] == 1 and not applied["pending"]
    until(switched, "measured lower-cost portfolio was not selected", 90)
    current = {m["id"]: m for m in json.loads(api.cli("list", "--tracker", api.tracker))["models"]}
    assert all(current[r["id"]]["allocations"] == [a["id"]] for r, a in zip(routes, managed)), current
    assert {name: service(name)["donor"]["reserved_total_bytes"] for name in ("a", "b")} == before
    user = "placement-user"
    token = api.cli("user-add", user)
    for route in routes:
        api.cli("grant", user, route["id"])
        connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=40)
        try:
            connection.request("POST", "/api/v1/chat", json.dumps({"model": route["id"], "max_tokens": 8,
                "messages": [{"role": "user", "content": "hello after cost selection"}]}),
                {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            response = connection.getresponse(); data = response.read()
            assert response.status == 200 and b"event: done\n" in data and b"event: error\n" not in data, data
        finally:
            connection.close()
    # Freeze this experiment's policy before deliberately invalidating its
    # co-resident allocation-set evidence. Retire ONLY its obsolete allocations.
    code, disabled = api.http(path, {"action": "disable", "revision": configured["revision"]})
    assert code == 200 and not disabled["enabled"], disabled
    for allocation in static:
        def released():
            return json.loads(api.cli("retire", allocation["id"], "--tracker", api.tracker))["state"] == "released"
        until(released, "obsolete static allocation did not retire", 60)
    inventory_ready()
    assert service("a")["donor"]["model_count"] == 2 and service("b")["donor"]["model_count"] == 0
    assert service("a")["donor"]["reserved_total_bytes"] < before["a"]
    assert service("b")["donor"]["reserved_total_bytes"] == 0
    stale = check_probe(api, "placement-managed")
    assert stale["state"] == "configuration_changed", stale
    after_release = measure("placement-after-retirement", managed)
    report = {"schema": 1, "scope": "loopback_reduced_olmoe_declared_prices_not_billing",
              "production_capacity_certified": False, "billed_savings": None,
              "limits": {"clients": 2, "rounds": 3, "output_tokens": 8, "ttft_ms": 30000, "gap_ms": 30000},
              "overload_clients": 4, "overload": overload,
              "baseline": baseline, "alternative": alternative, "selection": selected,
              "configured": configured, "applied": applied, "preparations": preparations,
              "stale_after_retirement": stale, "after_retirement": after_release,
              "unused_node": identities["b"], "server_shutdown": False}
    (artifacts / "placement-comparison.json").write_text(json.dumps(report, indent=2))
    api.cli("stop")
    print("PLACEMENT COMPARISON: PASS (two real models, measured joint latency, lower declared footprint, automatic routing, explicit retirement, stale evidence invalidation, remeasurement; no billed savings claimed)", flush=True)
