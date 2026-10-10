"""Apply a measured two-model portfolio to actual resident OLMoE replicas.
Executed only after independent donor approvals for all three allocations.
"""
import http.client
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import subprocess
import threading


def verify_capacity_application(runtime, env, tracker, first_name, artifacts, policy=False):
    def cli(*args, ok=True):
        result = subprocess.run([str(runtime / "lumabri"), "api", *args, "--tracker", tracker],
                                env=env, text=True, capture_output=True, timeout=40)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result.stdout.strip()

    models = json.loads(cli("list"))["models"]
    assert len(models) == 3
    first = next(m for m in models if m["name"] == first_name)
    third = next(m for m in models if "replica-checkpoint" in m["name"])
    other = next(m for m in models if m["id"] not in (first["id"], third["id"]))
    routes = [json.loads(cli("model-add", "portfolio-first", first["id"])),
              json.loads(cli("model-add", "portfolio-other", other["id"]))]
    before = json.loads(cli("workspace"))
    measured = json.loads(cli("capacity-measure", "joint-alternate", "2", "3", "8", third["id"], other["id"]))
    assert measured["stable"] and all(m["timing_samples"] == m["completed"] == 3 for m in measured["models"]), measured
    reader, manager, chatter = [cli("user-add", name) for name in ("portfolio-reader", "portfolio-manager", "portfolio-user")]
    cli("operator-grant", "portfolio-reader")
    cli("manage-grant", "portfolio-manager")
    for route in routes:
        cli("grant", "portfolio-user", route["id"])
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]

    def request(path, token, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=40)
        try:
            connection.request("POST" if body is not None else "GET", path, json.dumps(body) if body is not None else None,
                               {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def current_routes():
        return {m["id"]: m for m in json.loads(cli("list"))["models"] if m["id"] in {r["id"] for r in routes}}

    body = {"record": "joint-alternate", "ttft_ms": 30000, "gap_ms": 30000, "max_age_seconds": 300,
            "models": [{"id": r["id"], "revision": r["revision"]} for r in routes]}
    path = "/api/v1/capacity/apply"
    try:
        cli("start", "--port", str(port))
        assert request(path, reader, body)[0] == 403
        assert request(path, chatter, body)[0] == 403
        initial = current_routes()
        stale = {**body, "models": [body["models"][0], {**body["models"][1], "revision": 100}]}
        assert request(path, manager, stale)[0] == 409
        assert current_routes() == initial, "first model changed before second revision failed"
        wrong = {**body, "models": list(reversed(body["models"]))}
        assert request(path, manager, wrong)[0] == 409
        assert current_routes() == initial, "content mismatch published a partial portfolio"
        # An old two-allocation observation cannot authorize a new placement
        # after adding the third allocation to the same donors.
        baseline = json.loads(cli("capacity-check", "joint-baseline", "30000", "30000", "300"))
        baseline_models = {first["id"]: body["models"][0], other["id"]: body["models"][1]}
        stale_evidence = {**body, "record": "joint-baseline",
                          "models": [baseline_models[m["allocation"]] for m in baseline["models"]]}
        status, answer = request(path, manager, stale_evidence)
        assert status == 422 and json.loads(answer)["state"] == "configuration_changed", (status, answer)
        assert current_routes() == initial
        before_log = len((Path(env["HOME"]) / ".lumabri/service/api.log").read_bytes())
        status, answer = request(path, manager, body)
        applied = json.loads(answer)
        assert status == 200 and applied["applied"] and not applied["production_capacity_certified"], (status, applied)
        assert not applied["weights_loaded"] and not applied["weights_released"]
        assert [r["revision"] for r in applied["models"]] == [2, 2]
        assert [r["allocations"] for r in applied["models"]] == [[third["id"]], [other["id"]]]
        after_apply = current_routes()
        assert request(path, manager, body)[0] == 409, "old expected revisions applied twice"
        assert current_routes() == after_apply
        barrier = threading.Barrier(2)
        def chat(route):
            barrier.wait(timeout=5)
            return request("/api/v1/chat", chatter, {"model": route["id"], "max_tokens": 8,
                           "messages": [{"role": "user", "content": "hi"}]})
        with ThreadPoolExecutor(max_workers=2) as pool:
            replies = list(pool.map(chat, routes))
        assert all(code == 200 and b"event: done\n" in data and b"event: error\n" not in data for code, data in replies), replies
        log = (Path(env["HOME"]) / ".lumabri/service/api.log").read_text()[before_log:]
        assert "allocation=" + third["id"] in log and "allocation=" + other["id"] in log
        assert "allocation=" + first["id"] not in log, "old first replica still served new turns"
        # CLI delegates to the same typed operation and respects the same fences.
        cli_applied = json.loads(cli("capacity-apply", "joint-alternate", "30000", "30000", "300",
                                    *(r["id"] + ":2" for r in routes)))
        assert [r["revision"] for r in cli_applied["models"]] == [3, 3]
        if policy:
            from portfolio_policy_test import verify_portfolio_policy
            verify_portfolio_policy(cli, request, port, reader, manager, chatter, cli_applied["models"],
                                    first, third, other, env, artifacts)
        after = json.loads(cli("workspace"))
        assert {a["id"] for a in before["allocations"]} == {a["id"] for a in after["allocations"]}
        assert {n["id"]: n["workload"]["reserved_bytes"] for n in before["nodes"]} == {
            n["id"]: n["workload"]["reserved_bytes"] for n in after["nodes"]}
        (artifacts / "capacity-application.json").write_text(json.dumps({"measurement": measured, "application": applied,
                                                                        "cli_application": cli_applied}, indent=2))
    finally:
        cli("stop")
        for route in routes:
            cli("model-remove", route["id"])
    print("CAPACITY APPLICATION: PASS (two-model atomic change, actual alternate replica, HTTP/CLI same operation, stale/conflicting/unauthorized requests inert, resident weights unchanged)", flush=True)
