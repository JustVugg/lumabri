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


async def exercise(port, alice, bob, artifacts):
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
            if mode == "wait":
                await asyncio.sleep(2)
            wire = ""
            # Exact UTF-8 preservation across byte-codec DATA frames.
            for data in (b"Hello ", b"\xe2", b"\x82\xac", b" <script>alert(1)</script>"):
                wire += "event: delta\ndata: " + json.dumps({"bytes": base64.b64encode(data).decode()}) + "\n\n"
            if mode == "nul": wire += 'event: delta\ndata: {"bytes":"AA=="}\n\n'
            if mode in ("complete", "nul"): wire += 'event: done\ndata: {"stats":"fixture"}\n\n'
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
        # Cancellation must leave an interrupted question and permit retry.
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
        await page.set_viewport_size({"width": 390, "height": 844})
        await page.locator("#collapse").click()
        assert await page.evaluate("document.documentElement.scrollWidth <= innerWidth")
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
            asyncio.run(exercise(port, *tokens, artifacts))
        finally:
            server.terminate(); server.wait(timeout=10); server.stderr.close()
    print("WEB CHAT: PASS (compiled assets, live rendering, UTF-8, history, retry, cancellation, isolated users, no browser persistence, mobile)")


if __name__ == "__main__":
    main()
