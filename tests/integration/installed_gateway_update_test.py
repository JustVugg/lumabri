"""Installed gateway old -> new -> old, with approved engines left running.

This is not a fleet upgrade, a data migration or an automatic updater. The
caller owns isolated homes, two prepared tiny models and every service used.
"""
import base64
import hashlib
import http.client
import importlib.util
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("native_package", ROOT / "tools/package_household.py")
native_package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native_package)


def candidate_pair(directories):
    """Validate both packages before executing either; never fetch a release."""
    if len(directories) != 2:
        raise ValueError("two independently built native packages are required")
    candidates = []
    for directory in directories:
        directory = directory.resolve(strict=True)
        manifest = native_package.verify(directory)
        if (manifest["system"] != platform.system() or
                manifest["architecture"] != platform.machine()):
            raise ValueError("package is not native to this test machine")
        if any(manifest[key]["tracked_changes"] for key in ("lumabri", "colibri_checkout")):
            raise ValueError("record clean source revisions before the upgrade test")
        launcher = "Lumabri.command" if platform.system() == "Darwin" else "start-lumabri"
        if launcher not in manifest["files"] or "bin/lumabri" not in manifest["files"]:
            raise ValueError("installed launcher and gateway are required")
        candidates.append((directory, manifest, launcher))
    old, new = [candidate[1] for candidate in candidates]
    if (old["lumabri"]["commit"] == new["lumabri"]["commit"] or
            old["files"]["bin/lumabri"]["sha256"] == new["files"]["bin/lumabri"]["sha256"]):
        raise ValueError("identical gateway bytes or revisions are not an upgrade")
    if old["colibri_checkout"]["commit"] != new["colibri_checkout"]["commit"]:
        raise ValueError("this gateway-only test requires the same Colibri revision")
    return candidates


def verify_installed_gateway_update(api, until, service, artifacts, directories):
    candidates = candidate_pair(directories)
    access = Path(api.env["HOME"]) / ".lumabri/api-access"
    # Never present a successful chat-only rollback as compatibility with a
    # newer controller's retirement journal or active autonomous policies.
    for name in ("retirements", "elastic", "portfolio-policy"):
        path = access / name
        assert not path.exists() or not list(path.iterdir()), "controller state needs a separate migration test"
    report = {"schema": 1, "scope": "installed_gateway_only_reduced_olmoe_loopback",
        "full_fleet_upgrade_tested": False, "run_complete": False, "passed": False, "stages": [],
        "packages": [{"commit": manifest["lumabri"]["commit"],
                      "manifest_sha256": hashlib.sha256((directory / "manifest.json").read_bytes()).hexdigest(),
                      "gateway_sha256": manifest["files"]["bin/lumabri"]["sha256"],
                      "colibri_commit": manifest["colibri_checkout"]["commit"]}
                     for directory, manifest, _ in candidates]}

    def save():
        (artifacts / "installed-gateway-update.json").write_text(json.dumps(report, indent=2) + "\n")

    def cli(candidate, *args):
        directory, _, launcher = candidate
        answer = subprocess.run([str(directory / launcher), *args], cwd=directory,
                                env=api.env, text=True, capture_output=True, timeout=30)
        assert answer.returncode == 0, (args[:2], answer.stderr)
        return answer.stdout.strip()

    def request(path, body=None, token=None):
        connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=20)
        try:
            headers = {"Authorization": "Bearer " + (token or api.token)}
            if body is not None:
                headers["Content-Type"] = "application/json"
            connection.request("POST" if body is not None else "GET", path,
                               None if body is None else json.dumps(body), headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def turn(model):
        connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=20)
        try:
            connection.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
                "messages": [{"role": "user", "content": "Count from one to ten."}]}),
                {"Authorization": "Bearer " + api.token, "Content-Type": "application/json"})
            response = connection.getresponse()
            if response.status != 200:
                data = json.loads(response.read())
                raise AssertionError(("installed chat refused", response.status, data.get("error")))
            assert response.getheader("Content-Type") == "text/event-stream"
            event = None; done = False; text = bytearray()
            while True:
                line = response.readline()
                if not line:
                    break
                if line.startswith(b"event: "):
                    event = line[7:].strip()
                elif line.startswith(b"data: "):
                    assert not done, "data after completed response"
                    data = json.loads(line[6:])
                    if event == b"delta":
                        text.extend(base64.b64decode(data["bytes"], validate=True))
                    else:
                        assert event == b"done" and data["timing"]["token_notifications"] == 8
                        done = True
            assert text and done, "installed gateway did not complete real inference"
            return text.decode("utf-8", errors="replace")
        finally:
            connection.close()

    def allocations():
        observed = {}
        for name in ("a", "b"):
            donor = service(name)["donor"]
            assert donor["live"]
            observed[name] = {key: donor[key] for key in
                ("instance", "pid", "segment_pid", "host_pid", "model_count", "reserved_total_bytes")}
        return observed

    original = allocations()
    report["resident_allocations"] = original
    history = None; model_ids = None; current = None
    try:
        for label, index in (("previous", 0), ("upgrade", 1), ("rollback", 0)):
            current = candidates[index]
            assert not service("chatter")["api"]["live"], "stop the fixture gateway before switching versions"
            cli(current, "api", "start", "--tracker", api.tracker, "--port", str(api.port))
            gateway = service("chatter")["api"]
            assert gateway["live"]
            code, workspace = request("/api/v1/workspace")
            assert code == 200 and workspace["inventory_ok"]
            ids = sorted(allocation["id"] for allocation in workspace["allocations"])
            assert len(ids) == 2 and (model_ids is None or ids == model_ids)
            if model_ids is None:
                # Management permission deliberately does not grant chat.
                for model in ids:
                    cli(current, "api", "grant", "preparer", model, "--tracker", api.tracker)
            model_ids = ids
            assert request("/api/v1/workspace", token="invalid-test-token")[0] == 401
            if history is not None:
                path = "/api/v1/conversations/" + history["id"]
                assert request(path) == (200, history), "private history changed across gateway versions"
                assert request(path, token=api.reader)[0] == 404
                assert request(path, {"revision": history["revision"] + 1,
                                     "conversation": history["conversation"]})[0] == 409
            answers = [turn(model) for model in model_ids]
            if history is None:
                code, history = request("/api/v1/conversations", {"revision": 0, "conversation": {
                    "model": model_ids[0], "title": "Installed package lifecycle", "state": "complete",
                    "messages": [{"role": "user", "content": "Count from one to ten."},
                                 {"role": "assistant", "content": answers[0]}]}})
                assert code == 200 and history["revision"] == 1
            else:
                code, history = request("/api/v1/conversations/" + history["id"],
                    {"revision": history["revision"], "conversation": history["conversation"]})
                assert code == 200
            assert allocations() == original, "gateway switch reloaded or lost a resident model"
            # This explicit switch is quiescent, not a zero-downtime upgrade.
            def idle():
                code, data = request("/api/v1/demand")
                assert code == 200 and not data["telemetry_gaps"]
                return all(not model["admitted"] and not model["opening"] for model in data["models"])
            until(idle, "gateway work did not drain before update", 15)
            cli(current, "api", "stop")
            until(lambda: not service("chatter")["api"]["live"], "installed gateway did not stop", 15)
            assert allocations() == original
            report["stages"].append({"stage": label, "package": index,
                "gateway_instance": gateway["instance"], "gateway_pid": gateway["pid"],
                "completed_models": model_ids, "history_revision": history["revision"],
                "resident_allocations_unchanged": True})
            save()
        assert len({stage["gateway_instance"] for stage in report["stages"]}) == 3
        assert candidate_pair(directories) == candidates, "installed package mutated during use"
        report["run_complete"] = True; report["passed"] = True
        print("INSTALLED GATEWAY: PASS (previous, upgrade, rollback; two real models, private history, unchanged keepers)", flush=True)
    finally:
        save()
        if current is not None:
            cli(current, "api", "stop")
