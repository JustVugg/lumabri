"""Sustained real-engine workload on this fixture's two approved models.

No cloud, external peers, imported speed estimates or generated-text logging.
The bounded run retains every request outcome. Quiescent RSS is an observation,
not a no-leak or production-capacity certificate; engine locks are checked by
the calling household gate separately.
"""
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
import math
from pathlib import Path
import subprocess
import threading
import time


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1] if values else None


def summaries(rows):
    result = []
    for phase, model in sorted({(r["phase"], r["model"]) for r in rows}):
        selected = [r for r in rows if (r["phase"], r["model"]) == (phase, model)]
        complete = [r for r in selected if r["outcome"] == "completed"]
        result.append({"phase": phase, "model": model, "requests": len(selected),
            "completed": len(complete), "rejected": sum(r["outcome"] == "rejected" for r in selected),
            "client_disconnected": sum(r["outcome"] == "client_disconnected" for r in selected),
            "failed": sum(r["outcome"] == "failed" for r in selected),
            "percentiles_scope": "completed_requests_only",
            "client_ttft_p95_seconds": percentile([r["ttft_seconds"] for r in complete], .95),
            "client_completion_p95_seconds": percentile([r["elapsed_seconds"] for r in complete], .95),
            "response_token_gap_p95_seconds": percentile(
                [r["timing"]["token_notification_gaps"]["p95_seconds"] for r in complete], .95)})
    return result


def process_sample(roots):
    output = subprocess.run(["ps", "-axo", "pid=,ppid=,rss=,comm="],
        text=True, capture_output=True, check=True, timeout=10).stdout
    rows = {}
    for line in output.splitlines():
        pid, parent, rss, command = line.split(None, 3)
        rows[int(pid)] = {"parent": int(parent), "rss_bytes": int(rss) * 1024,
                          "program": Path(command).name}
    assert roots <= rows.keys(), "fixture service exited during sustained workload"
    owned = set(roots)
    while True:
        children = {pid for pid, row in rows.items() if row["parent"] in owned}
        if children <= owned:
            break
        owned |= children
    result = {str(pid): rows[pid] for pid in sorted(owned)}
    for pid, row in result.items():
        descriptors = Path("/proc") / pid / "fd"
        try:
            row["descriptors"] = len(list(descriptors.iterdir())) if descriptors.is_dir() else None
        except (PermissionError, FileNotFoundError):
            # Hardened service processes can deliberately deny /proc/fd.
            # Never disable that protection or invent a zero descriptor count.
            row["descriptors"] = None
    return {"processes": result, "rss_bytes": sum(row["rss_bytes"] for row in result.values())}


def hosts_quiescent(request, allocations):
    """Observe exact hosts; gateway END is not a remote cancellation ACK."""
    idle = True
    for allocation in allocations:
        code, state = request(f"/api/v1/allocations/{allocation}/control")
        assert code == 200, "host cleanup evidence unavailable"
        assert state["allocation"] == allocation and state["state"] == "accepting"
        counts = (state["connections"], state["admitted_requests"])
        assert all(type(n) is int and n >= 0 for n in counts)
        assert counts[1] <= counts[0]
        idle = idle and not any(counts)
    return idle


def verify_managed_soak(api, until, service, artifacts, seconds, max_growth_mib=64):
    assert 10 <= seconds <= 3600 and max_growth_mib > 0
    report = {"schema": 1, "scope": "reduced_olmoe_two_identity_loopback_soak",
        "production_capacity_certified": False, "run_complete": False,
        "requested_seconds": seconds, "max_quiescent_rss_growth_bytes": max_growth_mib << 20,
        "checkpoint_note": "Two independently resident identities derived from the same tiny weights; not a model-quality comparison.",
        "requests": [], "resources": [], "cancellation_cleanup": []}
    started = None

    def save():
        report["summaries"] = summaries(report["requests"])
        (artifacts / "managed-soak.json").write_text(json.dumps(report, indent=2) + "\n")

    def sample_request(index, barrier, phase, turn, tokens):
        record = {"phase": phase, "cycle": turn, "client": index,
                  "model": model_ids[index % 2], "outcome": "failed"}
        connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=20)
        response = None
        begin = time.monotonic()
        try:
            connection.connect(); barrier.wait(timeout=10)
            begin = time.monotonic()
            connection.request("POST", "/api/v1/chat", json.dumps({"model": record["model"],
                "max_tokens": 8, "messages": [{"role": "user", "content": "Count from one to ten."}]}),
                {"Authorization": "Bearer " + tokens[index], "Content-Type": "application/json"})
            response = connection.getresponse(); record["http_status"] = response.status
            if response.status != 200:
                data = json.loads(response.read())
                # Only this precise capacity refusal is expected under load.
                if response.status == 429 and data.get("error") == "replicas_busy":
                    record["outcome"] = "rejected"
                else:
                    record["error"] = "unexpected_http_result"
            else:
                assert response.getheader("Content-Type") == "text/event-stream"
                event = None; done = False; visible = False
                while True:
                    assert time.monotonic() - begin < 60, "response exceeded workload deadline"
                    line = response.readline()
                    if not line:
                        break
                    assert len(line) <= 1 << 20
                    if line.startswith(b"event: "):
                        event = line[7:].strip().decode()
                    elif line.startswith(b"data: "):
                        assert not done, "event after terminal DONE"
                        data = json.loads(line[6:])
                        if event == "delta":
                            if base64.b64decode(data["bytes"], validate=True):
                                visible = True
                                record.setdefault("ttft_seconds", time.monotonic() - begin)
                                if phase == "disconnect":
                                    # This records a CLIENT stop, not a claim
                                    # that firmware/kernel work was interrupted.
                                    record["outcome"] = "client_disconnected"
                                    break
                        elif event == "done":
                            assert visible and data["recovery_attempts"] == 0
                            timing = data["timing"]
                            assert timing["scope"] == "gateway_observed" and timing["token_notifications"] == 8
                            assert timing["token_notification_gaps"]["count"] == 7
                            assert 0 <= timing["ttft_seconds"] <= timing["generation_seconds"] <= timing["completion_seconds"]
                            assert 0 <= timing["token_notification_gaps"]["p50_seconds"] <= timing["token_notification_gaps"]["p95_seconds"]
                            record["timing"] = timing; record["outcome"] = "completed"; done = True
                        else:
                            raise AssertionError("unexpected generation or recovery event")
                if not done and record["outcome"] != "client_disconnected":
                    record["error"] = "incomplete_stream"
        except Exception as error:
            record["outcome"] = "failed"
            record["error"] = type(error).__name__  # no tokens/text/exception payload
        finally:
            if response is not None:
                response.close()
            connection.close()
            record["elapsed_seconds"] = time.monotonic() - begin
        return record

    def quiescent():
        code, data = api.http("/api/v1/demand")
        assert code == 200
        assert not data["telemetry_gaps"], "gateway demand evidence incomplete"
        return all(not m["opening"] and not m["admitted"] for m in data["models"])

    def burst(phase, count, turn):
        barrier = threading.Barrier(count)
        with ThreadPoolExecutor(max_workers=count) as pool:
            rows = list(pool.map(lambda i: sample_request(i, barrier, phase, turn, tokens), range(count)))
        report["requests"].extend(rows)
        assert all(r["outcome"] != "failed" for r in rows), rows
        if phase != "overload":
            wanted = "client_disconnected" if phase == "disconnect" else "completed"
            assert all(r["outcome"] == wanted for r in rows), rows
        until(quiescent, "requests remained admitted after close", 15)
        if phase == "disconnect":
            # Cancellation is asynchronous: END releases gateway admission,
            # while the host can still be draining/resetting the codec slot.
            # Wait for authenticated host observations, not a fixed delay or
            # retries of the next chat. Unknown/stuck hosts still fail.
            began = time.monotonic()
            until(lambda: hosts_quiescent(api.http, model_ids),
                  "cancelled host conversations did not close", 15)
            report["cancellation_cleanup"].append({"cycle": turn,
                "elapsed_seconds": time.monotonic() - began, "hosts_idle": True})

    def reservation_sample():
        result = {}
        for name in ("a", "b"):
            donor = service(name)["donor"]
            assert donor["live"]
            result[name] = {"instance": donor["instance"], "pid": donor["pid"],
                "models": donor["model_count"], "reserved_bytes": donor["reserved_total_bytes"]}
        return result

    def resource_sample(baseline=None):
        assert reservation_sample() == reservations, "resident allocation or reservation changed during workload"
        deadline = time.monotonic() + 10
        previous = None
        while True:
            observed = process_sample(roots)
            # HTTP helper children exit after sending the final response.
            # Require a quiet sample, never assert on that transient fork.
            ids = set(observed["processes"])
            stable = (ids == set(baseline["processes"])) if baseline else ids == previous
            if stable:
                break
            assert time.monotonic() < deadline, "owned process set did not return to quiescence"
            previous = ids; time.sleep(.2)
        observed["elapsed_seconds"] = time.monotonic() - started if started else 0
        report["resources"].append(observed)
        if baseline:
            assert observed["rss_bytes"] <= baseline["rss_bytes"] + (max_growth_mib << 20), "quiescent RSS exceeded declared growth bound"
            for pid, row in observed["processes"].items():
                before = baseline["processes"][pid]
                assert row["program"] == before["program"] and row["parent"] == before["parent"]
                if row["descriptors"] is not None and before["descriptors"] is not None:
                    assert row["descriptors"] <= before["descriptors"] + 2, "persistent descriptor growth"
        return observed

    try:
        api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
        code, workspace = api.http("/api/v1/workspace")
        assert code == 200 and workspace["inventory_ok"] and len(workspace["allocations"]) == 2
        model_ids = sorted(a["id"] for a in workspace["allocations"])
        report["allocations"] = model_ids
        report["runtime_sha256"] = {name: hashlib.sha256((api.runtime / name).read_bytes()).hexdigest()
            for name in ("lumabri", "segment_node", "segment_chat")}
        report["residency_policy"] = api.env.get("LUMABRI_RESIDENCY_POLICY", "observe")
        tokens = [api.cli("user-add", f"soak-{i}") for i in range(8)]
        for i in range(8):
            api.cli("grant", f"soak-{i}", model_ids[i % 2], "--tracker", api.tracker)
        reservations = reservation_sample(); report["reservations"] = reservations
        roots = {s["pid"] for s in reservations.values()} | {service("chatter")["api"]["pid"]}
        for i in range(3):
            burst("warmup", 2, i)
        burst("overload", 8, -1); burst("disconnect", 2, -1); burst("after-disconnect", 2, -1)
        baseline = resource_sample()
        started = time.monotonic(); next_report = started; cycles = 0
        while time.monotonic() - started < seconds:
            burst("steady", 2, cycles)
            if cycles % 5 == 0:
                burst("overload", 8, cycles)
                burst("disconnect", 2, cycles)
                burst("after-disconnect", 2, cycles)
            cycles += 1
            if time.monotonic() >= next_report:
                resource_sample(baseline)
                report["elapsed_seconds"] = time.monotonic() - started
                report["cycles"] = cycles; save()
                print(f"MANAGED SOAK: {report['elapsed_seconds']:.0f}/{seconds}s, {len(report['requests'])} requests, no failed turns", flush=True)
                next_report = time.monotonic() + 30
            time.sleep(.25)
        burst("final", 2, cycles)
        resource_sample(baseline)
        assert any(r["outcome"] == "rejected" for r in report["requests"]), "overload case did not exercise refusal"
        report["cycles"] = cycles; report["elapsed_seconds"] = time.monotonic() - started
        report["run_complete"] = True
    finally:
        try:
            api.cli("stop")
        except BaseException:
            report["run_complete"] = False
            raise
        finally:
            save()
    print(f"MANAGED SOAK: PASS ({report['elapsed_seconds']:.1f}s, {len(report['requests'])} requests; two resident identities, bounded overload, client disconnects and resource checks)", flush=True)
