"""Observed mixed-load envelope, inside the approved real-engine service gate.

Never extrapolate this reduced-checkpoint run to production model throughput.
The artifact retains rejections/failures, client latency and gateway timing;
success-only percentiles are labelled and are not an admission guarantee.
No private prompt, generated text or credential is written to the artifact.
"""
import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import math
import socket
import subprocess
import threading
import time


def verify_mixed_capacity(runtime, env, tracker, artifacts):
    def cli(*args):
        result = subprocess.run([str(runtime / "lumabri"), "api", *args, "--tracker", tracker],
                                env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    models = json.loads(cli("list"))["models"]
    assert len(models) == 2, "measure before adding logical aliases or extra replicas"
    users = [cli("user-add", f"capacity-{i}") for i in range(8)]
    for i in range(8):
        cli("grant", f"capacity-{i}", models[i % 2]["id"])
    observer = cli("user-add", "capacity-observer")
    cli("operator-grant", "capacity-observer")
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0)); port = reserve.getsockname()[1]

    def snapshot():
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        try:
            conn.request("GET", "/api/v1/workspace", headers={"Authorization": "Bearer " + observer})
            response = conn.getresponse(); data = json.loads(response.read())
            assert response.status == 200 and data["inventory_ok"], data
            return data
        finally:
            conn.close()

    def one(index, barrier):
        model = models[index % 2]["id"]
        record = {"client": index, "model": model, "completed": False}
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        try:
            # Establish all connections before the barrier, so setup does not
            # deliberately serialize submissions. This is a burst, not an
            # open-loop requests/second load generator.
            conn.connect(); barrier.wait(timeout=10)
            start = time.monotonic()
            conn.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
                "messages": [{"role": "user", "content": "hi"}]}), headers={
                "Authorization": "Bearer " + users[index], "Content-Type": "application/json"})
            response = conn.getresponse(); record["http_status"] = response.status
            if response.status != 200:
                record["error"] = json.loads(response.read()).get("error", "unknown")
            else:
                event = None
                while True:
                    line = response.readline()
                    if not line:
                        break
                    if line.startswith(b"event: "):
                        event = line[7:].strip().decode()
                    elif line.startswith(b"data: "):
                        data = json.loads(line[6:])
                        if event == "delta" and base64.b64decode(data["bytes"], validate=True):
                            record.setdefault("client_ttft_seconds", time.monotonic() - start)
                        elif event == "done":
                            record["completed"] = True
                            record["gateway_timing"] = data["timing"]
                            record["engine_statistics"] = data["stats"]
                            assert data["recovery_attempts"] == 0
                        elif event == "error":
                            record["error"] = "generation_failed"
                if not record["completed"]:
                    record.setdefault("error", "incomplete_stream")
            record["client_completion_seconds"] = time.monotonic() - start
        finally:
            conn.close()
        return record

    def rank(values, fraction):
        return sorted(values)[math.ceil(len(values) * fraction) - 1] if values else None

    report = {"schema": 1, "scope": "real_reduced_checkpoint_loopback_bursts",
              "production_capacity_certified": False, "run_complete": False, "rounds": [], "summaries": []}
    try:
        cli("start", "--port", str(port))
        report["inventory_before"] = snapshot()
        cases = [("solo-a", [0]), ("solo-b", [1])]
        cases += [(f"mixed-{n}", list(range(n))) for n in (2, 4, 8)]
        for label, clients in cases:
            rows = []
            for turn in range(3):
                barrier = threading.Barrier(len(clients))
                with ThreadPoolExecutor(max_workers=len(clients)) as pool:
                    samples = list(pool.map(lambda i: one(i, barrier), clients))
                report["rounds"].append({"case": label, "turn": turn, "samples": samples})
                rows.extend(samples)
                for row in samples:
                    if row["completed"]:
                        timing = row["gateway_timing"]
                        assert timing["scope"] == "gateway_observed" and timing["token_notifications"] == 8, row
                        assert 0 <= timing["ttft_seconds"] <= timing["generation_seconds"] <= timing["completion_seconds"], row
                        gaps = timing["token_notification_gaps"]
                        assert gaps["count"] == 7 and 0 <= gaps["p50_seconds"] <= gaps["p95_seconds"], row
                    else:
                        assert row["http_status"] == 429 and row["error"] == "replicas_busy", row
                if len(clients) <= 2:
                    assert all(row["completed"] for row in samples), samples
            successful = [r for r in rows if r["completed"]]
            summary = {"case": label, "requests": len(rows), "completed": len(successful),
                       "rejected": len(rows) - len(successful), "percentiles_scope": "successful_requests_only"}
            for field in ("client_ttft_seconds", "client_completion_seconds"):
                values = [r[field] for r in successful]
                summary[field] = {"p50": rank(values, .5), "p95": rank(values, .95)}
            report["summaries"].append(summary)
        report["inventory_after"] = snapshot()
        assert {a["id"] for a in report["inventory_before"]["allocations"]} == {
            a["id"] for a in report["inventory_after"]["allocations"]}, "benchmark changed resident allocations"
        # The production C probe uses the same real gateway admission and
        # timing paths. Its bounded durable observations feed the planner;
        # these are not imported JSON fixtures or claims about full models.
        ids = [m["id"] for m in models]
        measured = json.loads(cli("capacity-measure", "joint-baseline", "2", "3", "8", *ids))
        assert measured["state"] == "measured" and measured["stable"], measured
        assert all(m["completed"] == m["timing_samples"] == 3 for m in measured["models"]), measured
        checked = json.loads(cli("capacity-check", "joint-baseline", "30000", "30000", "300"))
        assert checked["state"] == "observed_workload_passed", checked
        overloaded = json.loads(cli("capacity-measure", "joint-overloaded", "8", "3", "8", *ids))
        assert overloaded["state"] == "measured" and sum(m["rejected"] for m in overloaded["models"]) > 0, overloaded
        checked_overload = json.loads(cli("capacity-check", "joint-overloaded", "30000", "30000", "300"))
        assert checked_overload["state"] == "requests_rejected_or_failed", checked_overload
        selected = json.loads(cli("capacity-select", "30000", "30000", "300", "joint-baseline", "joint-overloaded"))
        assert selected["selected"] == "joint-baseline" and not selected["applied"], selected
        assert selected["candidates"][1]["state"] == "workload_not_comparable", selected
        repeat = json.loads(cli("capacity-measure", "joint-repeat", "2", "3", "8", *ids))
        assert repeat["state"] == "measured", repeat
        selected = json.loads(cli("capacity-select", "30000", "30000", "300", "joint-baseline", "joint-repeat"))
        assert selected["selected"] == "joint-baseline", selected
        assert all(c["state"] == "observed_workload_passed" for c in selected["candidates"]), selected
        assert selected["objective"] == "operator_order_prices_unknown_or_mixed", selected
        assert "resident allocations and conversations unchanged" in cli("capacity-remove", "joint-repeat")
        assert {a["id"] for a in snapshot()["allocations"]} == {a["id"] for a in report["inventory_before"]["allocations"]}
        report["production_probe"] = {"baseline": measured, "overloaded": overloaded, "selection": selected}
        report["run_complete"] = True
    finally:
        try:
            cli("stop")
        except BaseException:
            report["run_complete"] = False
            raise
        finally:
            (artifacts / "mixed-capacity.json").write_text(json.dumps(report, indent=2))
    print("MIXED CAPACITY: PASS (two resident models, solo baselines, 2/4/8-client bursts, per-response timing and explicit rejections; no production SLO claim)", flush=True)
