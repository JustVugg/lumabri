"""Real compiled web UI and private history, with explicit transport fixtures.
Real Colibri inference is a separate resident_api_test gate, not claimed here.
Requires Playwright Chromium; never included in native runtime dependencies.
"""
import asyncio
import base64
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from playwright.async_api import async_playwright, expect

ROOT = Path(__file__).resolve().parents[2]
MODEL = "01" * 32


async def exercise(port, alice, bob, artifacts, env):
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True)
        context = await browser.new_context(viewport={"width": 1365, "height": 900}, color_scheme="light")
        page = await context.new_page()
        failures = []
        page.on("pageerror", lambda error: failures.append(str(error)))
        mode = "complete"
        requests = []

        async def model_fixture(route):
            await route.fulfill(json={"schema": 1, "models": [{"id": MODEL, "name": "OLMoE · local test",
                                "max_tokens": 16, "context": 128, "sessions": 2, "state": "saved_plan"}]})

        async def engine_fixture(route):
            requests.append(route.request.post_data_json)
            if mode == "busy":
                await route.fulfill(status=429, json={"error": "replicas_busy"})
                return
            if mode == "wait":
                await asyncio.sleep(2)
            wire = ""
            # Exact UTF-8 preservation across byte-codec DATA frames.
            for data in (b"Hello ", b"\xe2", b"\x82\xac", b" <script>alert(1)</script>"):
                wire += "event: delta\ndata: " + json.dumps({"bytes": base64.b64encode(data).decode()}) + "\n\n"
                if mode == "recovered" and data == b"\xe2":
                    wire += 'event: recovering\ndata: {"message":"Replaying approved replica"}\n\n'
            if mode == "nul": wire += 'event: delta\ndata: {"bytes":"AA=="}\n\n'
            if mode == "recovered": wire += 'event: done\ndata: {"stats":"fixture","recovery_attempts":1,"observation_saved":false}\n\n'
            elif mode in ("complete", "nul"): wire += 'event: done\ndata: {"stats":"fixture"}\n\n'
            elif mode == "error": wire += 'event: error\ndata: {"message":"explicit fixture failure"}\n\n'
            try: await route.fulfill(status=200, content_type="text/event-stream", body=wire)
            except Exception:
                if mode != "wait": raise

        await page.route("**/api/v1/models", model_fixture)
        await page.route("**/api/v1/chat", engine_fixture)
        await page.goto(f"http://127.0.0.1:{port}/")
        await page.screenshot(path=str(artifacts / "web-login.png"))
        await page.locator("#token").fill(alice)
        await page.locator("#login-form button").click()
        await expect(page.locator("#composer-area")).to_be_visible()
        await expect(page.locator("#workspace-open")).to_be_hidden()
        await page.locator("#prompt").fill("First private question")
        await page.locator("#send").click()
        await expect(page.locator("#save-status")).to_have_text("Saved privately on this computer")
        await expect(page.locator(".assistant .content")).to_have_text("Hello € <script>alert(1)</script>")
        assert await page.locator(".assistant script").count() == 0
        assert len(requests) == 1 and requests[0]["messages"] == [{"role": "user", "content": "First private question"}]
        await page.locator("#prompt").fill("Second question")
        await page.locator("#prompt").press("Enter")
        await expect(page.locator("#stop")).to_be_hidden()
        assert len(requests) == 2 and len(requests[-1]["messages"]) == 3
        await expect(page.locator(".message.user")).to_have_count(2)
        async with page.expect_download() as download:
            await page.locator("#export").click()
        exported = await download.value
        payload = json.loads(Path(await exported.path()).read_text())
        assert payload["conversation"]["state"] == "complete" and len(payload["conversation"]["messages"]) == 4
        await page.screenshot(path=str(artifacts / "web-chat.png"))
        await page.locator("#collapse").click()
        await expect(page.locator("#sidebar")).to_be_hidden()
        await page.locator("#expand").click()
        await expect(page.locator("#sidebar")).to_be_visible()
        # A hard reload must require the token again; browser storage is empty.
        assert await page.evaluate("[localStorage.length, sessionStorage.length]") == [0, 0]
        await page.reload()
        await expect(page.locator("#login")).to_be_visible()
        await page.locator("#token").fill(alice); await page.locator("#login-form button").click()
        await page.locator("#history button").click()
        await expect(page.locator(".message.user")).to_have_count(2)
        # A concurrent update must reject the stale tab BEFORE generation.
        headers = {"Authorization": "Bearer " + alice}
        listing = await page.request.get(f"http://127.0.0.1:{port}/api/v1/conversations", headers=headers)
        chat_id = (await listing.json())["conversations"][0]["id"]
        url = f"http://127.0.0.1:{port}/api/v1/conversations/{chat_id}"
        record = await (await page.request.get(url, headers=headers)).json()
        changed = await page.request.post(url, headers=headers, data={"revision": record["revision"], "conversation": record["conversation"]})
        assert changed.status == 200
        await page.locator("#prompt").fill("Must not reach the engine"); await page.locator("#send").click()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator("#notice")).to_contain_text("changed in another window")
        assert len(requests) == 2
        await page.locator("#history button").click()
        # Explicit errors persist a partial answer, never a completed turn.
        mode = "error"
        await page.locator("#prompt").fill("Incomplete question"); await page.locator("#send").click()
        await expect(page.locator("#retry")).to_be_visible()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator(".message.interrupted")).to_have_count(1)
        await expect(page.locator("#prompt")).to_be_disabled()
        mode = "complete"; await page.locator("#retry").click()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator("#retry")).to_be_hidden()
        assert requests[-1]["messages"][-1] == {"role": "user", "content": "Incomplete question"}
        await expect(page.locator(".message.user")).to_have_count(3)
        mode = "nul"
        await page.locator("#prompt").fill("Preserve every byte"); await page.locator("#send").click()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator("#save-status")).to_contain_text("reply contains a NUL byte")
        await expect(page.locator("#prompt")).to_be_disabled()
        assert (await page.locator(".assistant .content").last.text_content()).endswith("\0")
        await page.locator("#new-chat").click()
        # Busy is a saved question with explicit manual retry, never a hidden
        # engine submission loop or an empty completed assistant message.
        mode = "busy"; before_busy = len(requests)
        await page.locator("#prompt").fill("Capacity question"); await page.locator("#send").click()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator("#notice")).to_contain_text("All approved replicas")
        await expect(page.locator("#retry")).to_be_visible()
        await expect(page.locator(".message.user")).to_have_count(1)
        await expect(page.locator(".message.assistant")).to_have_count(0)
        assert len(requests) == before_busy + 1
        mode = "complete"; await page.locator("#retry").click()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator("#retry")).to_be_hidden()
        await expect(page.locator(".message.user")).to_have_count(1)
        await page.locator("#new-chat").click()
        # Cancellation must leave an interrupted question and permit retry.
        mode = "recovered"
        await page.locator("#prompt").fill("Recover on an approved replica"); await page.locator("#send").click()
        await expect(page.locator("#stop")).to_be_hidden()
        await expect(page.locator(".assistant .content")).to_have_text("Hello € <script>alert(1)</script>")
        await expect(page.locator("#notice")).to_contain_text("Recovered")
        await expect(page.locator("#save-status")).to_have_text("Saved privately on this computer")
        await page.locator("#new-chat").click()
        mode = "wait"
        await page.locator("#prompt").fill("Please stop"); await page.locator("#send").click()
        await expect(page.locator("#stop")).to_be_visible(); await page.locator("#stop").click()
        await expect(page.locator("#retry")).to_be_visible()
        await expect(page.locator("#stop")).to_be_hidden()
        # Browser private-history isolation, not only a server unit test.
        await page.locator("#sign-out").click()
        await page.locator("#token").fill(bob); await page.locator("#login-form button").click()
        await expect(page.locator("#composer-area")).to_be_visible()
        assert await page.locator("#history button").count() == 0
        assert "First private question" not in await page.locator("body").inner_text()
        await expect(page.locator("#workspace-open")).to_be_visible()
        # Real authorization plus an explicit display fixture, not a measured
        # performance claim. The native service gate checks actual inventory.
        workspace_online = True
        async def workspace_fixture(route):
            if not workspace_online:
                await route.fulfill(status=503, json={"error": "workspace_unavailable"})
                return
            await route.fulfill(json={"schema": 1, "captured_at": int(time.time()), "inventory_ok": True,
                "registry_ok": True, "inventory_ttl_ms": 15000,
                "nodes": [{"id": "node-a", "name": "Studio workstation <script>bad()</script>",
                    "cpu": "Intel Core i7", "os": "Linux", "arch": "x86_64", "age_ms": 1200,
                    "threads": 12, "runtime_threads": 4, "ram_total_bytes": 32 * 2**30,
                    "ram_available_bytes": 18 * 2**30, "ram_offered_bytes": 12 * 2**30,
                    "gpu_detected": 1, "vram_inventory_bytes": 8 * 2**30,
                    "workload": {"allocations": 2, "reserved_bytes": 10 * 2**30, "active": 1, "queued": 2},
                    "machine_cost": None, "power": None}],
                "models": [{"name": "OLMoE", "adapter": "olmoe", "replicas": 2,
                    "context": 4096, "max_tokens": 256, "revision": 3}],
                "allocations": [{"id": MODEL, "name": "OLMoE · approved allocation", "adapter": "olmoe",
                    "context": 4096, "session_limit": 4, "preparation_seconds": 12.5,
                    "observation": {"state": "obsolete", "decode_tok_s": None},
                    "ranges": [{"node": "node-a", "begin": 0, "end": 16, "edge": True,
                        "reserved_bytes": 10 * 2**30, "report_present": True}]}]})
        await page.route("**/api/v1/workspace", workspace_fixture)
        await page.locator("#workspace-open").click()
        await expect(page.locator("#workspace")).to_be_visible()
        await expect(page.locator("#workspace-nodes")).to_contain_text("2 queued")
        await expect(page.locator("#workspace-allocations")).to_contain_text("Obsolete")
        assert await page.locator("#workspace script").count() == 0
        assert await page.locator(".allocation-actions button").count() == 0, "read-only operator got mutation controls"
        await page.screenshot(path=str(artifacts / "web-workspace.png"))
        # Real credential-bound management grant; explicit transport fixture
        # below exercises browser confirmation and exact uint64 fences. The
        # native resident flow separately invokes actual host/keeper handlers.
        subprocess.run([str(ROOT / "lumabri"), "api", "manage-grant", "bob"], env=env,
                       capture_output=True, text=True, check=True, timeout=10)
        await page.locator("#refresh").click()
        await page.locator("#workspace-refresh").click()
        await expect(page.locator(".allocation-actions button")).to_have_count(4)
        management_requests = []
        control_state = {"schema": 1, "allocation": MODEL, "instance": "02" * 32,
                         "revision": "9007199254740993", "state": "accepting"}
        async def control_fixture(route):
            if route.request.method == "POST":
                body = route.request.post_data_json
                assert body["instance"] == control_state["instance"] and body["revision"] == control_state["revision"]
                assert set(body) == {"action", "instance", "revision"}
                management_requests.append(body)
                control_state["revision"] = str(int(control_state["revision"]) + 1)
                control_state["state"] = {"drain": "drained", "resume": "accepting", "retire": "released"}[body["action"]]
            await route.fulfill(json=control_state)
        await page.route("**/api/v1/allocations/*/control", control_fixture)
        page.once("dialog", lambda dialog: dialog.dismiss())
        await page.get_by_role("button", name="Release RAM", exact=True).click()
        await expect(page.get_by_role("button", name="Release RAM", exact=True)).to_be_enabled()
        assert not management_requests, "cancelled confirmation mutated the allocation"
        for action, label, state in (("drain", "Drain", "drained"), ("resume", "Resume", "accepting"), ("retire", "Release RAM", "released")):
            page.once("dialog", lambda dialog: dialog.accept())
            await page.get_by_role("button", name=label, exact=True).click()
            await expect(page.locator("#notice")).to_contain_text(": " + state)
            assert management_requests[-1]["action"] == action
        assert management_requests[0]["revision"] == "9007199254740993", "browser rounded the fence"
        await page.screenshot(path=str(artifacts / "web-management.png"))
        subprocess.run([str(ROOT / "lumabri"), "api", "manage-revoke", "bob"], env=env,
                       capture_output=True, text=True, check=True, timeout=10)
        await page.locator("#refresh").click(); await page.locator("#workspace-refresh").click()
        await expect(page.locator(".allocation-actions button")).to_have_count(0)
        await page.set_viewport_size({"width": 390, "height": 844})
        await page.locator("#collapse").click()
        assert await page.evaluate("document.documentElement.scrollWidth <= innerWidth")
        await page.screenshot(path=str(artifacts / "web-workspace-mobile.png"))
        workspace_online = False
        await page.locator("#workspace-refresh").click()
        await expect(page.locator("#workspace-status")).to_contain_text("Workspace unavailable")
        assert await page.locator("#workspace-nodes article").count() == 0, "failed refresh left stale resources visible"
        await page.locator("#expand").click(); await page.locator("#new-chat").click()
        await expect(page.locator("#workspace")).to_be_hidden()
        await expect(page.locator("#composer-area")).to_be_visible()
        await page.locator("#expand").click(); await page.locator("#sign-out").click()
        await expect(page.locator("#workspace-open")).to_be_hidden()
        assert await page.locator("#workspace-nodes article").count() == 0
        await page.locator("#collapse").click()
        await page.screenshot(path=str(artifacts / "web-mobile.png"))
        assert not failures, failures
        await browser.close()


def main():
    artifacts = ROOT / "build/web-chat-artifacts"; artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lmb-web-chat-") as temporary:
        env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key"}
        tokens = []
        for name in ("alice", "bob"):
            result = subprocess.run([str(ROOT / "lumabri"), "api", "user-add", name], env=env,
                                    capture_output=True, text=True, check=True, timeout=10)
            tokens.append(result.stdout.strip())
            file = Path(temporary) / ".lumabri/api-access" / f"{name}.user"
            file.write_bytes(file.read_bytes()[:72] + struct.pack("<I", 1) + bytes.fromhex(MODEL))
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
        subprocess.run([str(ROOT / "lumabri"), "api", "operator-grant", "bob"], env=env,
                       capture_output=True, text=True, check=True, timeout=10)
        server = subprocess.Popen([str(ROOT / "lumabri"), "api", "serve", "--tracker", "127.0.0.1:1", "--port", str(port)],
                                  env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            until = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.2): break
                except OSError:
                    assert server.poll() is None and time.monotonic() < until
                    time.sleep(.05)
            asyncio.run(exercise(port, *tokens, artifacts, env))
        finally:
            server.terminate(); server.wait(timeout=10); server.stderr.close()
    print("WEB CHAT: PASS (compiled assets, chat/history/cancel, private users, operator workspace, stale refresh, mobile)")


if __name__ == "__main__":
    main()
