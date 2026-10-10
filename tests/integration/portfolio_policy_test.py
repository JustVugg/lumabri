"""Actual two-model OLMoE policy, inside the independently approved LAN-style
loopback test. Declared test prices are not billed costs or claimed savings.
"""
import json
from pathlib import Path
import time


def verify_portfolio_policy(cli, request, port, reader, manager, chatter, routes, first, third, other, env, artifacts):
    path = "/api/v1/portfolio-policy"

    def status():
        code, data = request(path, reader)
        assert code == 200, (code, data)
        return json.loads(data)

    def await_state(predicate, timeout=90):
        deadline = time.monotonic() + timeout
        observed = None
        while time.monotonic() < deadline:
            observed = status()
            if predicate(observed):
                return observed
            time.sleep(.3)
        raise AssertionError(("policy did not reach expected state", observed))

    assert status() == {"schema": 1, "configured": False}
    assert request(path, chatter)[0] == 403
    measured = json.loads(cli("capacity-measure", "joint-policy-base", "2", "3", "8", first["id"], other["id"]))
    assert measured["stable"] and all(m["completed"] == 3 for m in measured["models"]), measured
    config = {"action": "configure", "revision": 0, "records": ["joint-policy-base", "joint-alternate"],
              "current": 1, "models": [{"id": r["id"], "revision": r["revision"]} for r in routes],
              "ttft_ms": 30000, "gap_ms": 30000, "max_age_seconds": 3600, "cooldown_seconds": 60,
              "horizon_seconds": 3600, "min_saving_bps": 1000, "currency": "EUR",
              "ceiling_micro_per_hour": "1000000", "switch_cost_micro": "1000", "enabled": True}
    assert request(path, reader, config)[0] == 403
    assert request(path, chatter, config)[0] == 403
    assert request(path, manager, {**config, "command": "anything"})[0] == 400
    assert request(path, manager, {**config, "revision": 1})[0] == 409
    assert request(path, manager, {**config, "current": 0})[0] == 409
    # A real drained host makes the current measured placement unavailable.
    # The alternate is already approved and resident; no consent is replayed.
    drained = False
    try:
        assert json.loads(cli("replica", third["id"], "drain"))["state"] in ("draining", "drained")
        drained = True
        code, data = request(path, manager, config)
        configured = json.loads(data)
        assert code == 200 and configured["enabled"] and configured["revision"] == 1, (code, data)
        assert request(path, manager, config)[0] == 409
        cli("stop")
        cli("start", "--port", str(port))
        applied = await_state(lambda s: s["current"] == 0 and not s["pending"])
        assert not applied["faulted"] and [m["revision"] for m in applied["models"]] == [4, 4], applied
        assert applied["last_change"] and applied["scope"] == "approved_resident_routing_only"
        assert not applied["production_capacity_certified"] and not applied["cloud_provisioning"]
        published = {m["id"]: m for m in json.loads(cli("list"))["models"]}
        assert published[routes[0]["id"]]["allocations"] == [first["id"]]
        assert published[routes[1]["id"]]["allocations"] == [other["id"]]
        log_file = Path(env["HOME"]) / ".lumabri/service/api.log"
        log_at = len(log_file.read_bytes())
        for route in routes:
            code, reply = request("/api/v1/chat", chatter, {"model": route["id"], "max_tokens": 8,
                                 "messages": [{"role": "user", "content": "hello after policy"}]})
            assert code == 200 and b"event: done\n" in reply and b"event: error\n" not in reply, (code, reply)
        log = log_file.read_bytes()[log_at:].decode()
        assert "allocation=" + first["id"] in log and "allocation=" + other["id"] in log
        assert "allocation=" + third["id"] not in log
        cli("stop")
        cli("start", "--port", str(port))
        restarted = status()
        assert restarted["models"] == applied["models"] and restarted["last_change"] == applied["last_change"]
        assert json.loads(cli("replica", third["id"], "resume"))["state"] == "accepting"
        drained = False
        # External operator mutation, even to the same routing policy, must
        # stop automation; it cannot silently adopt a new route revision.
        manual = json.loads(cli("model-policy", routes[0]["id"], "ordered"))
        assert manual["revision"] == 5
        faulted = await_state(lambda s: s["faulted"])
        assert faulted["reason"] == "routes_changed_reconfigure_required", faulted
        current = {m["id"]: m for m in json.loads(cli("list"))["models"]}
        assert [current[r["id"]]["revision"] for r in routes] == [5, 4]
        # Disabling must remain possible even when the model registry is
        # unavailable. This move affects only this isolated test household.
        registry = Path(env["HOME"]) / ".lumabri/api-access/models"
        unavailable = registry.with_name("models-unavailable")
        registry.rename(unavailable)
        try:
            code, data = request(path, manager, {"action": "disable", "revision": 1})
        finally:
            unavailable.rename(registry)
        disabled = json.loads(data)
        assert code == 200 and not disabled["enabled"] and disabled["revision"] == 2, (code, data)
        assert request(path, manager, {"action": "disable", "revision": 1})[0] == 409
        assert json.loads(cli("portfolio-policy")) == status()
        # A same-named observation cannot silently replace consented evidence.
        # Removing a pinned candidate alone must hold the controller too.
        renewed = {**config, "revision": 2, "current": 0,
                   "models": [{"id": r["id"], "revision": current[r["id"]]["revision"]} for r in routes]}
        code, data = request(path, manager, renewed)
        assert code == 200 and json.loads(data)["revision"] == 3, (code, data)
        cli("capacity-remove", "joint-alternate")
        missing = await_state(lambda s: s["faulted"])
        assert missing["reason"] == "evidence_missing_or_replaced", missing
        assert request(path, manager, {"action": "disable", "revision": 3})[0] == 200
        (artifacts / "portfolio-policy.json").write_text(json.dumps({"baseline": measured, "configured": configured,
            "applied": applied, "restarted": restarted, "manual_change": manual, "faulted": faulted,
            "disabled": disabled, "missing_evidence": missing, "final": status()}, indent=2))
    finally:
        observed = status()
        if observed.get("enabled"):
            assert request(path, manager, {"action": "disable", "revision": observed["revision"]})[0] == 200
        if drained:
            cli("replica", third["id"], "resume")
    print("PORTFOLIO CONTROLLER: PASS (real two-model rerouting, explicit bounded policy, measured resident candidates, restart without duplicate change, manual/evidence drift hold, registry-independent disable, permission isolation)", flush=True)
