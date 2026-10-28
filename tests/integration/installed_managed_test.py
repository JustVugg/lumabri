"""Full installed-package managed lifecycle, without shipped test executables.

Called by the real donor-approval fixture. This is a clean-install check, not
a fleet upgrade, a provider test or a claim about trained-model quality.
"""
import base64
import hashlib
import http.client
import json
from pathlib import Path
import platform

from installed_gateway_update_test import native_package
from managed_soak_test import verify_managed_soak


def checked_package(directory, require_agent=True):
    directory = directory.resolve(strict=True)
    manifest = native_package.verify(directory)
    if (manifest["system"] != platform.system() or
            manifest["architecture"] != platform.machine()):
        raise ValueError("installed managed gate requires a native package")
    for key in ("lumabri", "colibri_checkout"):
        if manifest[key]["tracked_changes"]:
            raise ValueError("record clean source revisions before installed acceptance")
    launcher = "Lumabri.command" if platform.system() == "Darwin" else "start-lumabri"
    binaries = native_package.BINARIES if require_agent else native_package.CORE_BINARIES
    required = {f"bin/{name}" for name in binaries} | {launcher}
    if not required <= manifest["files"].keys():
        raise ValueError("complete household package and launcher are required")
    for name in manifest["files"]:
        if (Path(name).name.startswith("test_") or name.endswith("segment_chat.real") or
                name.startswith(("tests/", "build/"))):
            raise ValueError("test helpers or replacement engines must not be in the package")
    return directory, manifest


def verify_installed_managed(api, until, service, artifacts, directory, manifest, detach, soak_seconds=10):
    if type(soak_seconds) is not int or not 10 <= soak_seconds <= 3600:
        raise ValueError("installed workload duration must be 10–3600 seconds")
    assert api.runtime.resolve() == directory / "bin"
    report = {"schema": 1, "scope": "installed_managed_service_reduced_olmoe_loopback",
        "run_complete": False, "full_fleet_update_tested": False,
        "package_commit": manifest["lumabri"]["commit"],
        "colibri_commit": manifest["colibri_checkout"]["commit"],
        "manifest_sha256": hashlib.sha256((directory / "manifest.json").read_bytes()).hexdigest(),
        "workload_seconds_requested": soak_seconds,
        "harness_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                           for name in ("household_service_flow_test.py", "installed_managed_test.py", "managed_soak_test.py")},
        "runtime_sha256": {name: manifest["files"]["bin/" + name]["sha256"]
                           for name in native_package.BINARIES}}

    def residents():
        result = {}
        for name in ("a", "b"):
            state = service(name)["donor"]
            assert state["live"]
            result[name] = {key: state[key] for key in ("instance", "pid", "segment_pid", "host_pid",
                "model", "model_count", "reserved_total_bytes")}
        return result

    def turn(model, token):
        connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=30)
        response = None
        try:
            connection.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
                "messages": [{"role": "user", "content": "Count from one to ten."}]}),
                {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            response = connection.getresponse()
            assert response.status == 200, (response.status, response.read())
            assert response.getheader("Content-Type") == "text/event-stream"
            event = None; done = False; text = bytearray()
            while True:
                line = response.readline()
                if not line:
                    break
                assert len(line) <= 1 << 20
                if line.startswith(b"event: "):
                    event = line[7:].strip()
                elif line.startswith(b"data: "):
                    assert not done
                    data = json.loads(line[6:])
                    if event == b"delta":
                        text.extend(base64.b64decode(data["bytes"], validate=True))
                    else:
                        assert event == b"done" and data["timing"]["token_notifications"] == 8
                        done = True
            assert text and done
            return text.decode("utf-8", errors="replace")
        finally:
            if response:
                response.close()
            connection.close()

    def idle():
        status, demand = api.http("/api/v1/demand")
        assert status == 200 and not demand["telemetry_gaps"]
        return all(not m["admitted"] and not m["opening"] for m in demand["models"])

    def retire(model):
        return json.loads(api.cli("retire", model, "--tracker", api.tracker))["state"] == "released"

    original = residents()
    assert sum(r["model_count"] for r in original.values()) == 2
    # Do not substitute two exclusive donors for the co-residence requirement.
    owner, = [name for name, state in original.items() if state["model_count"] == 2]
    report["co_resident_node"] = owner
    try:
        detach()
        assert residents() == original, "closing the installed TUI unloaded an allocation"
        before_managers = {name: service(name)["manager"]["instance"] for name in ("owner", "a", "b", "chatter")}
        for name in before_managers:
            service(name, "restart")
            assert service(name)["manager"]["instance"] != before_managers[name]
        assert residents() == original, "manager restart changed installed allocations"
        report["detach_and_manager_restart"] = True

        # Reuse the real measured workload, not simulated completions. This
        # default smoke and opt-in sustained run both include distinct users,
        # bounded overload and client stops, followed by the same lifecycle.
        verify_managed_soak(api, until, service, artifacts, soak_seconds)
        assert residents() == original
        report["workload_report"] = "managed-soak.json"
        api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
        code, workspace = api.http("/api/v1/workspace")
        assert code == 200 and len(workspace["allocations"]) == 2
        allocations = workspace["allocations"]
        survivor, = [a for a in allocations if a["name"] == original[owner]["model"]]
        first, = [a for a in allocations if a["id"] != survivor["id"]]
        tokens = [api.cli("user-add", name) for name in ("installed-a", "installed-b")]
        for name, allocation in zip(("installed-a", "installed-b"), (first, survivor)):
            api.cli("grant", name, allocation["id"], "--tracker", api.tracker)
        answers = [turn(a["id"], t) for a, t in zip((first, survivor), tokens)]
        until(idle, "installed turns did not release admission", 15)
        conversation = {"model": first["id"], "title": "Installed real response", "state": "complete",
            "messages": [{"role": "user", "content": "Count from one to ten."},
                         {"role": "assistant", "content": answers[0]}]}
        code, saved = api.http("/api/v1/conversations", {"revision": 0, "conversation": conversation}, tokens[0])
        assert code == 200 and saved["conversation"] == conversation
        path = "/api/v1/conversations/" + saved["id"]
        assert api.http(path, token=tokens[1])[0] == 404
        assert api.http(path)[0] == 404, "manager credential leaked a user's history"
        api.cli("stop")
        assert residents() == original
        api.cli("start", "--tracker", api.tracker, "--port", str(api.port))
        assert api.http(path, token=tokens[0]) == (200, saved)
        report["private_history_and_gateway_restart"] = True

        until(lambda: retire(first["id"]), "first installed allocation did not retire", 60)
        after = residents()
        assert after[owner]["model_count"] == 1
        assert 0 < after[owner]["reserved_total_bytes"] < original[owner]["reserved_total_bytes"]
        for key in ("instance", "pid", "segment_pid", "host_pid", "model"):
            assert after[owner][key] == original[owner][key], "selective retirement replaced the surviving runtime"
        assert turn(survivor["id"], tokens[1])
        until(idle, "surviving installed model did not drain", 15)
        assert retire(first["id"]), "retirement receipt was not idempotent"
        assert residents() == after
        assert api.http(path, token=tokens[0]) == (200, saved), "retirement removed private history"
        report["selective_retirement"] = True
        until(lambda: retire(survivor["id"]), "last installed allocation did not retire", 60)
        final = residents()
        assert all(not s["model_count"] and not s["reserved_total_bytes"] for s in final.values())
        assert checked_package(directory) == (directory, manifest), "installed package changed during use"
        report["run_complete"] = True
        print("INSTALLED MANAGED: PASS (two co-resident models, real overload/stop, private history, restarts, selective release)", flush=True)
    finally:
        try:
            api.cli("stop")
        except BaseException:
            report["run_complete"] = False
            raise
        finally:
            (artifacts / "installed-managed.json").write_text(json.dumps(report, indent=2) + "\n")
