"""Real preparation API gate, driven by the household donor-consent fixture."""
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
        self.runtime, self.env, self.tracker = runtime, env, tracker
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
        assert self.http("/api/v1/preparation", self.start_request)[0] == 200
        status, history = self.http("/api/v1/preparation")
        assert status == 200 and len(history["operations"]) == 1, history
        assert history["operations"][0]["operation"] == self.operation
        (Path(self.env["HOME"]).parent / "preparation-api.json").write_text(json.dumps(result, indent=2))
        self.cli("stop")
