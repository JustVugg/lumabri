"""Real preparation API gate, driven by the household donor-consent fixture."""
import hashlib
import http.client
import json
import os
import secrets
import socket
import subprocess
import time
from pathlib import Path


class PreparationAPI:
    def __init__(self, runtime, env, tracker, models, sessions):
        self.runtime, self.env, self.tracker, self.models = runtime, env, tracker, models
        settings = Path(env["HOME"]) / ".lumabri/home.conf"
        settings.write_text("\n".join(f"models={models}" if line.startswith("models=") else line
                                      for line in settings.read_text().splitlines()) + "\n")
        self.token = self.cli("user-add", "preparer")
        self.reader = self.cli("user-add", "read-only")
        self.cli("manage-grant", "preparer")
        self.cli("operator-grant", "read-only")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0)); self.port = sock.getsockname()[1]
        self.cli("start", "--tracker", tracker, "--port", str(self.port))
        assert self.http("/api/v1/preparation", {}, self.reader)[0] == 403
        assert self.http("/api/v1/preparation", token=self.reader)[0] == 403
        status, catalogue = self.http("/api/v1/catalogue", token=self.reader)
        assert status == 200 and {m["name"] for m in catalogue["models"]} == {"model-a", "model-b"}, catalogue
        assert all(m["source_ready"] and m["decode_tok_s"] is None for m in catalogue["models"])
        deadline = time.monotonic() + 30
        while True:
            status, workspace = self.http("/api/v1/workspace")
            nodes = [n["id"] for n in workspace["nodes"] if n["workload"] is not None and n["runtime_threads"]]
            if len(nodes) == 2:
                break
            assert time.monotonic() < deadline, workspace
            time.sleep(.2)
        intent = {"models": ["model-a", "model-b"], "nodes": nodes,
                  "context": 128, "sessions": sessions, "max_new": 8}
        status, preview = self.http("/api/v1/preparation", {"action": "preview", **intent})
        assert status == 200, preview
        assert preview["plan"]["requires_approval"] and preview["plan"]["decode_tok_s"] is None
        assert len(preview["plan"]["models"]) == 2
        # A checkpoint metadata change invalidates the reviewed plan before
        # the operation receipt or first donor reservation is created.
        config = models / "model-b/config.json"
        original = config.read_bytes(); config.write_bytes(original + b"\n")
        self.operation = secrets.token_hex(32)
        request = {"action": "start", "operation": self.operation, "review": preview["review"], **intent}
        assert self.http("/api/v1/preparation", request)[0] == 409
        assert not list((Path(env["HOME"]) / ".lumabri/api-access/preparations").glob("*.intent"))
        config.write_bytes(original)
        status, preview = self.http("/api/v1/preparation", {"action": "preview", **intent})
        assert status == 200, preview
        request["review"] = preview["review"]
        if os.environ.get("LUMABRI_TEST_BROWSER") == "1":
            from playwright.sync_api import sync_playwright, expect
            with sync_playwright() as p:
                browser = p.chromium.launch(headless=True)
                page = browser.new_page(viewport={"width": 1365, "height": 1000})
                errors = []; page.on("pageerror", lambda e: errors.append(str(e)))
                page.goto(f"http://127.0.0.1:{self.port}/")
                page.locator("#token").fill(self.token)
                page.locator('#login-form button[type="submit"]').click()
                page.locator("#workspace-open").click()
                expect(page.locator("#prepare-catalogue")).to_be_enabled()
                page.locator("#prepare-catalogue").click()
                expect(page.locator("#prepare-form")).to_be_visible()
                for name in intent["models"]:
                    page.locator(f'#prepare-models input[value="{name}"]').check()
                for node in nodes:
                    page.locator(f'#prepare-nodes input[value="{node}"]').check()
                page.locator("#prepare-context").fill("128")
                page.locator("#prepare-sessions").fill(str(sessions))
                page.locator("#prepare-output").fill("8")
                page.locator("#prepare-preview").click()
                expect(page.locator("#prepare-start")).to_be_visible()
                expect(page.locator("#prepare-review .workspace-card")).to_have_count(2)
                page.screenshot(path=str(Path(env["HOME"]).parent / "preparation-review.png"), full_page=True)
                with page.expect_response(lambda res: res.url.endswith("/api/v1/preparation") and
                                          res.request.method == "POST" and res.request.post_data_json.get("action") == "start") as sent:
                    page.locator("#prepare-start").click()
                status, started = sent.value.status, sent.value.json()
                request = sent.value.request.post_data_json
                self.operation = request["operation"]
                assert not errors, errors
                browser.close()  # preparation must continue without its page
        else:
            status, started = self.http("/api/v1/preparation", request)
        self.start_request = request
        assert status == 202, started
        assert started["operation"] == self.operation
        status, repeated = self.http("/api/v1/preparation", request)
        assert status == 200 and repeated["operation"] == self.operation, repeated
        assert self.http("/api/v1/preparation", {**request, "max_new": 7})[0] == 409
        self.cli("stop")
        self.cli("start", "--tracker", tracker, "--port", str(self.port))
        assert self.http("/api/v1/preparation", request)[0] == 200
        restarted = subprocess.run([str(runtime / "lumabri"), "service", "restart"], env=env,
                                   text=True, capture_output=True, timeout=30)
        assert restarted.returncode == 0, restarted.stderr
        assert self.http("/api/v1/preparation", request)[0] == 200

    def cli(self, *args):
        p = subprocess.run([str(self.runtime / "lumabri"), "api", *args], env=self.env,
                           text=True, capture_output=True, timeout=30)
        # A measurement may succeed as an operation but contain a failed
        # request. Preserve its diagnostics without logging tokens/stdout.
        if p.stderr:
            with (Path(self.env["HOME"]).parent / "management-cli.log").open("a") as log:
                log.write(f"action={args[0]} exit={p.returncode}\n{p.stderr}\n")
        assert p.returncode == 0, (args, p.stderr)
        return p.stdout.strip()

    def http(self, path, body=None, token=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        headers = {"Authorization": "Bearer " + (token or self.token)}
        if body is not None:
            headers["Content-Type"] = "application/json"
        c.request("POST" if body is not None else "GET", path,
                  None if body is None else json.dumps(body), headers)
        response = c.getresponse(); status, data = response.status, json.loads(response.read()); c.close()
        return status, data

    def cancel(self):
        if os.environ.get("LUMABRI_TEST_BROWSER") == "1":
            from playwright.sync_api import sync_playwright, expect
            with sync_playwright() as p:
                browser = p.chromium.launch(headless=True)
                page = browser.new_page(viewport={"width": 1365, "height": 1000})
                page.goto(f"http://127.0.0.1:{self.port}/")
                page.locator("#token").fill(self.token)
                page.locator('#login-form button[type="submit"]').click()
                page.locator("#workspace-open").click()
                expect(page.locator("#prepare-operation")).to_have_value(self.operation)
                expect(page.locator("#prepare-cancel")).to_be_visible(timeout=15000)
                page.on("dialog", lambda dialog: dialog.accept())
                with page.expect_response(lambda res: res.url.endswith("/api/v1/preparation") and
                                          res.request.method == "POST" and res.request.post_data_json.get("action") == "cancel") as sent:
                    page.locator("#prepare-cancel").click()
                status, result = sent.value.status, sent.value.json()
                browser.close()
        else:
            status, result = self.http("/api/v1/preparation", {"action": "cancel", "operation": self.operation})
        assert status == 200 and result["cancel_requested"], result

    def finish(self, success):
        status, result = self.http("/api/v1/preparation", {"action": "status", "operation": self.operation})
        assert status == 200 and result["state"] == ("done" if success else "failed"), result
        assert result["results_known"] and result["result_scope"] == "historical_preparation_not_live_readiness", result
        assert [a["name"] for a in result["allocations"]] == self.start_request["models"], result
        ready = [a for a in result["allocations"] if a["state"] == "ready_observed"]
        assert len(ready) == (2 if success else 1), result
        status, workspace = self.http("/api/v1/workspace")
        assert status == 200, workspace
        assert {a["id"] for a in ready} == {a["id"] for a in workspace["allocations"]}, (result, workspace)
        repeated_code, repeated = self.http("/api/v1/preparation", self.start_request)
        assert repeated_code == 200 and repeated["allocations"] == result["allocations"], repeated
        status, history = self.http("/api/v1/preparation")
        assert status == 200 and len(history["operations"]) == 1, history
        assert history["operations"][0]["operation"] == self.operation
        assert history["operations"][0]["allocations"] == result["allocations"]
        (Path(self.env["HOME"]).parent / "preparation-api.json").write_text(json.dumps(result, indent=2))
        self.cli("stop")

    def reload_authorized(self, until):
        self.cli("start", "--tracker", self.tracker, "--port", str(self.port))
        status, before = self.http("/api/v1/workspace")
        assert status == 200, before
        name = self.start_request["models"][0]
        original = next(a for a in before["allocations"] if a["name"].endswith("-" + name))
        nodes = sorted({r["node"] for r in original["ranges"]})
        before_ids = {a["id"] for a in before["allocations"]}
        def release():
            result = json.loads(self.cli("retire", original["id"], "--tracker", self.tracker))
            return result["state"] == "released"
        until(release, "original allocation did not retire", 60)
        def inventory_ready():
            code, now = self.http("/api/v1/workspace")
            assert code == 200, now
            # Saved plans are history, not live reservations. Retirement
            # retains its plan so lost acknowledgements can be reconciled.
            expected = {node: sum(a["id"] != original["id"] and any(r["node"] == node for r in a["ranges"])
                                  for a in before["allocations"]) for node in nodes}
            reports = {n["id"]: n["workload"] for n in now["nodes"]}
            return all(reports.get(node) is not None and reports[node]["allocations"] == expected[node] for node in nodes)
        until(inventory_ready, "retirement not reflected in inventory", 30)
        intent = {**{k: self.start_request[k] for k in ("context", "sessions", "max_new")}, "models": [name], "nodes": nodes}
        status, preview = self.http("/api/v1/preparation", {"action": "preview", **intent})
        if status != 200:
            args = [str(self.runtime / "lumabri"), "models", "--models-dir", str(self.models), "--tracker", self.tracker,
                    "--json", "--context", str(intent["context"]), "--sessions", str(intent["sessions"]), "--together", name]
            for node in nodes:
                args += ["--node", node]
            diagnostic = subprocess.run(args, env=self.env, text=True, capture_output=True, timeout=30)
            raise AssertionError((preview, self.http("/api/v1/workspace"), diagnostic.stdout, diagnostic.stderr))
        operation = secrets.token_hex(32)
        status, started = self.http("/api/v1/preparation", {"action": "start", "review": preview["review"], "operation": operation, **intent})
        assert status == 202, started
        # No owner command or terminal approval between this start and READY.
        def ready():
            code, state = self.http("/api/v1/preparation", {"action": "status", "operation": operation})
            assert code == 200 and state["state"] != "failed", state
            return state["state"] == "done"
        until(ready, "authorized placement did not reload automatically", 120)
        status, receipt = self.http("/api/v1/preparation", {"action": "status", "operation": operation})
        assert status == 200 and receipt["results_known"] and len(receipt["allocations"]) == 1, receipt
        assert receipt["allocations"][0]["name"] == name and receipt["allocations"][0]["state"] == "ready_observed", receipt
        status, after = self.http("/api/v1/workspace")
        assert status == 200 and len(after["allocations"]) == len(before["allocations"]) + 1
        added = [a for a in after["allocations"] if a["id"] not in before_ids]
        assert len(added) == 1 and added[0]["name"].endswith("-" + name), after
        assert added[0]["id"] == receipt["allocations"][0]["id"] and added[0]["id"] != original["id"], receipt
        # READY is a direct acknowledgement; tracker workload reports have
        # their own heartbeat. Equal counts do not prove the new allocation
        # replaced the retired one in the reviewed inventory. Wait for exact
        # set digests before testing an over-quota OFFER, rather than racing
        # the production review fence and assuming a 202 means it was sent.
        expected_sets = {}
        for node in nodes:
            ids = sorted(bytes.fromhex(a["id"]) for a in after["allocations"]
                         if a["id"] != original["id"] and any(r["node"] == node for r in a["ranges"]))
            expected_sets[node] = hashlib.sha256(b"lumabri-allocation-set-v1\0" + b"".join(ids)).hexdigest()
        def reloaded_inventory():
            code, reviewed = self.http("/api/v1/preparation", {"action": "preview", **intent})
            return code == 200 and all(n["allocation_set"] == expected_sets[n["id"]]
                                      for n in reviewed["plan"]["nodes"])
        until(reloaded_inventory, "reloaded allocation set not reflected in inventory", 30)
        self.repeat_intent = intent
        self.cli("stop")
        return added[0]["name"]

    def duplicate_offer(self):
        self.cli("start", "--tracker", self.tracker, "--port", str(self.port))
        status, preview = self.http("/api/v1/preparation", {"action": "preview", **self.repeat_intent})
        assert status == 200, preview
        operation = secrets.token_hex(32)
        status, state = self.http("/api/v1/preparation", {"action": "start", "review": preview["review"],
            "operation": operation, **self.repeat_intent})
        assert status == 202, state
        return operation

    def assert_operation_running(self, operation):
        status, state = self.http("/api/v1/preparation", {"action": "status", "operation": operation})
        assert status == 200 and state["state"] not in ("failed", "done"), state

    def cancel_operation(self, operation, until):
        status, cancelled = self.http("/api/v1/preparation", {"action": "cancel", "operation": operation})
        assert status == 200 and cancelled["cancel_requested"], cancelled
        until(lambda: self.http("/api/v1/preparation", {"action": "status", "operation": operation})[1]["state"] == "failed",
              "over-quota preparation did not cancel", 30)
        self.cli("stop")
