"""Public repeated lifecycle, not seeded fake receipts or a reduced cap.

Runs only in disposable household fixture homes. Explicit owner approval on
each turn; real reduced OLMoE inference, exact retirement and zero remaining
charged allocations before the next cycle. It never removes durable records.
"""
import hashlib
import http.client
import json
import secrets
import time

from home_flow_test import approval_frame_ready


def verify_lifecycle_history(api, until, service, terminals, artifacts, cycles):
    api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
    code, initial = api.http("/api/v1/preparation", {"action": "status", "operation": api.operation})
    assert code == 200 and initial["state"] == "done", initial
    retired = []
    operations = [api.operation]
    rows = []

    def retire(allocation):
        def released():
            state = json.loads(api.cli("retire", allocation, "--tracker", api.tracker))
            assert state["state"] in ("released", "waiting_for_admitted_work"), state
            return state["state"] == "released"
        until(released, "cycle allocation did not retire", 60)
        retired.append(allocation)
        until(lambda: all(service(n)["donor"]["reserved_total_bytes"] == 0 for n in terminals),
              "retirement left charged donor memory", 30)

    # Retire the first pair together before asserting zero total reservations.
    for item in initial["allocations"]:
        allocation = item["id"]
        until(lambda: json.loads(api.cli("retire", allocation, "--tracker", api.tracker))["state"] == "released",
              "initial models did not retire", 60)
        retired.append(allocation)
    until(lambda: all(service(n)["donor"]["reserved_total_bytes"] == 0 for n in terminals), "initial RAM held", 30)
    source = api.start_request["models"][0]
    node = api.start_request["nodes"][0]
    intent = {"models": [source], "nodes": [node], "context": 128, "sessions": 1, "max_new": 4}
    empty_set = hashlib.sha256(b"lumabri-allocation-set-v1\0").hexdigest()
    route = None
    for cycle in range(cycles):
        started_at = time.monotonic()
        preview = None

        def reviewed_empty():
            nonlocal preview
            code, preview = api.http("/api/v1/preparation", {"action": "preview", **intent})
            return code == 200 and all(n["allocation_set"] == empty_set for n in preview["plan"]["nodes"])

        until(reviewed_empty, "zero-reservation inventory not observed", 30)
        operation = secrets.token_hex(32)
        request = {"action": "start", "operation": operation, "review": preview["review"], **intent}
        code, result = api.http("/api/v1/preparation", request)
        assert code == 202, (cycle, result)
        approvals = set()

        def ready():
            for name, terminal in terminals.items():
                state = service(name)["donor"]
                if state["phase"] == 1 and name not in approvals and approval_frame_ready(terminal, state["model"]):
                    approvals.add(name)
                    terminal.send("\x1b[A\r")
            code, status = api.http("/api/v1/preparation", {"action": "status", "operation": operation})
            assert code == 200 and status["state"] != "failed", (cycle, status)
            return status["state"] == "done"

        until(ready, "approved cycle preparation did not complete", 120)
        assert len(approvals) == 1, approvals
        code, receipt = api.http("/api/v1/preparation", request)
        assert code == 200 and not receipt["approval_replayed"] and len(receipt["allocations"]) == 1, receipt
        allocation = receipt["allocations"][0]["id"]
        assert allocation not in retired
        if route is None:
            route = json.loads(api.cli("model-add", "lifecycle-olmoe", allocation, "--tracker", api.tracker))
            api.cli("grant", "preparer", route["id"], "--tracker", api.tracker)
        else:
            route = json.loads(api.cli("model-set", route["id"], allocation, "--tracker", api.tracker))
        connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=30)
        connection.request("POST", "/api/v1/chat", json.dumps({"model": route["id"], "max_tokens": 4,
            "messages": [{"role": "user", "content": "hello lifecycle"}]}),
            {"Authorization": "Bearer " + api.token, "Content-Type": "application/json"})
        response = connection.getresponse(); body = response.read(); connection.close()
        assert response.status == 200 and b"event: done\n" in body and b"event: error\n" not in body, (cycle, response.status, body)
        retire(allocation)
        operations.append(operation)
        rows.append({"cycle": cycle + 1, "operation": operation, "allocation": allocation,
                     "fresh_approvals": len(approvals), "elapsed_seconds": time.monotonic() - started_at})
        (artifacts / "lifecycle-history-progress.json").write_text(json.dumps(rows, indent=2))
        if (cycle + 1) % 10 == 0 or cycle + 1 == cycles:
            print(f"LIFECYCLE HISTORY: {cycle + 1}/{cycles} approved, inferred and fully retired", flush=True)
        if cycle + 1 in (64, 256):
            api.cli("stop"); api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
            service("chatter", "restart")

    # Fresh process reads every original key; old approvals never replay.
    api.cli("stop"); api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
    for allocation in retired:
        state = json.loads(api.cli("retire", allocation, "--tracker", api.tracker))
        assert state["state"] == "released" and state["complete"], state
    seen = set(); cursor = ""; pages = 0
    while True:
        code, history = api.http("/api/v1/preparation" + ("/after/" + cursor if cursor else ""))
        assert code == 200 and len(history["operations"]) <= 256, history
        page_ids = {item["operation"] for item in history["operations"]}
        assert not seen & page_ids
        seen |= page_ids; pages += 1
        next_cursor = history["next_cursor"]
        assert not next_cursor or next_cursor > cursor
        if not next_cursor:
            break
        cursor = next_cursor
    assert seen == set(operations), (len(seen), len(operations))
    for operation in operations:
        code, saved = api.http("/api/v1/preparation", {"action": "status", "operation": operation})
        assert code == 200 and saved["state"] == "done" and not saved["approval_replayed"], saved
    journal_counts = {}
    for name in ("chatter", "a", "b"):
        journal_counts[name] = {suffix: len(list((artifacts / name / ".lumabri").rglob("*." + suffix)))
                                for suffix in ("intent", "state", "retire", "release", "lifetime")}
    if cycles >= 256:
        assert journal_counts["chatter"]["intent"] > 256 and journal_counts["chatter"]["retire"] > 256
        assert max(journal_counts[n]["release"] for n in terminals) > 256
    report = {"schema": 1, "scope": "reduced_olmoe_public_lifecycle_loopback", "cycles": cycles,
              "rows": rows, "history_pages": pages, "journal_counts": journal_counts,
              "historical_receipts_preserved": True, "unexpected_failures": 0}
    (artifacts / "lifecycle-history.json").write_text(json.dumps(report, indent=2))
    api.cli("stop")
    print(f"LIFECYCLE HISTORY: PASS ({cycles} complete cycles, {pages} history pages, restart and exact receipt replay)", flush=True)
