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
from playwright.async_api import Error as PlaywrightError, async_playwright, expect
from web_elastic_test import elastic_workspace
from web_capacity_test import capacity_workspace

ROOT = Path(__file__).resolve().parents[2]
MODEL = "01" * 32


async def history_races(page, port, token):
    """Delay real history replies; assert persisted documents as well as the UI."""
    base = f"http://127.0.0.1:{port}"
    headers = {"Authorization": "Bearer " + token}
    history = base + "/api/v1/conversations"

    class PausedReply:
        def __init__(self, method, failure=False, engine=False):
            self.method, self.failure, self.engine = method, failure, engine
            self.entered, self.release = asyncio.Event(), asyncio.Event()
            self.cancelled = False

        async def handle(self, route):
            if route.request.method != self.method:
                await route.fallback()
                return
            response = None if self.failure or self.engine else await route.fetch()
            self.entered.set()
            await asyncio.wait_for(self.release.wait(), 30)
            if self.failure:
                await route.fulfill(status=503, json={"error": "delayed_history_failure"})
            elif self.engine:
                try:
                    await route.fulfill(content_type="text/event-stream", body=
                        'event: delta\ndata: {"bytes":"UmFjZSByZXBseQ=="}\n\nevent: done\ndata: {}\n\n')
                except PlaywrightError:
                    if not self.cancelled:
                        raise
            else:
                await route.fulfill(response=response)

    async def read(id):
        response = await page.request.get(history + "/" + id, headers=headers)
        assert response.status == 200
        return await response.json()

    async def seed(label):
        records = []
        for suffix in ("A", "B"):
            title = f"{label} {suffix}"
            conversation = {"model": MODEL, "title": title, "state": "complete",
                "messages": [{"role": "user", "content": title}, {"role": "assistant", "content": "Original reply"}]}
            response = await page.request.post(history, headers=headers,
                                               data={"revision": 0, "conversation": conversation})
            assert response.status == 200
            records.append(await response.json())
        await page.locator("#refresh").click()
        for record in records:
            await expect(page.get_by_role("button", name=record["conversation"]["title"], exact=True)).to_be_visible()
        return records

    async def open_record(record):
        await page.get_by_role("button", name=record["conversation"]["title"], exact=True).click()
        await page.evaluate("window.raceHistoryLoad")

    # Observe completion of the actual event handlers, without timing sleeps
    # or adding synchronization hooks to production code.
    await page.evaluate("""() => {
        window.raceOriginalLoad = openConversation;
        openConversation = id => window.raceHistoryLoad = window.raceOriginalLoad(id);
        window.raceOriginalDelete = document.getElementById('delete').onclick;
        document.getElementById('delete').onclick = () =>
            window.raceHistoryDelete = window.raceOriginalDelete();
    }""")
    try:
        for case in ("existing", "new", "stop-retry", "load-error", "save-conflict"):
            a, b = await seed("Load " + case)
            await open_record(a)
            if case == "new":
                await page.locator("#new-chat").click()
            prompt = "Question for " + case
            await page.locator("#prompt").fill(prompt)
            load = PausedReply("GET", failure=case == "load-error")
            engine = PausedReply("POST", engine=True)
            load_url = history + "/" + b["id"]
            save_url = history + "/" + a["id"]

            async def reject_completed_save(route):
                if route.request.method == "POST" and route.request.post_data_json["conversation"]["state"] == "complete":
                    await route.fulfill(status=409, json={"error": "conversation_changed_reload_before_editing"})
                else:
                    await route.fallback()

            await page.route(load_url, load.handle)
            await page.route("**/api/v1/chat", engine.handle)
            if case == "save-conflict":
                await page.route(save_url, reject_completed_save)
            try:
                await page.get_by_role("button", name=b["conversation"]["title"], exact=True).click()
                await asyncio.wait_for(load.entered.wait(), 10)
                await page.locator("#send").click()
                await asyncio.wait_for(engine.entered.wait(), 10)
                load.release.set()
                await page.evaluate("window.raceHistoryLoad")
                await expect(page.locator("#notice")).to_have_text("")
                if case == "stop-retry":
                    engine.cancelled = True
                    await page.locator("#stop").click()
                    await expect(page.locator("#stop")).to_be_hidden()
                    await expect(page.locator("#retry")).to_be_visible()
                    assert (await read(a["id"]))["conversation"]["state"] == "interrupted"
                engine.release.set()
                if case == "stop-retry":
                    await page.locator("#retry").click()
                await expect(page.locator("#stop")).to_be_hidden()
                await expect(page.locator(".assistant .content").last).to_have_text("Race reply")
                assert await read(b["id"]) == b, "a delayed history load redirected the generation into B"
                if case == "save-conflict":
                    await expect(page.locator("#notice")).to_contain_text("NOT saved")
                    assert (await read(a["id"]))["conversation"]["state"] == "pending"
                    async with page.expect_download() as download:
                        await page.locator("#export").click()
                    exported = await download.value
                    saved = json.loads(Path(await exported.path()).read_text())
                    assert saved["id"] == a["id"] and saved["revision"] == a["revision"] + 1
                    page.once("dialog", lambda dialog: dialog.accept())
                    await page.locator("#new-chat").click()
                elif case == "new":
                    listing = await (await page.request.get(history, headers=headers)).json()
                    created = [item for item in listing["conversations"] if item["title"] == prompt]
                    assert len(created) == 1
                    saved = await read(created[0]["id"])
                else:
                    saved = await read(a["id"])
                assert saved["conversation"]["state"] == "complete"
                assert saved["conversation"]["messages"][-2:] == [
                    {"role": "user", "content": prompt}, {"role": "assistant", "content": "Race reply"}]
                assert len(saved["conversation"]["messages"]) == (2 if case == "new" else 4)
            finally:
                load.release.set(); engine.release.set()
                await page.unroute(load_url, load.handle)
                await page.unroute("**/api/v1/chat", engine.handle)
                if case == "save-conflict":
                    await page.unroute(save_url, reject_completed_save)

        for case in ("switch", "pending-switch", "pending-self", "new", "logout", "failure"):
            a, b = await seed("Delete " + case)
            await open_record(a)
            deletion = PausedReply("DELETE", failure=case == "failure")
            delete_url = history + "/" + a["id"]
            pending_load = None
            if case in ("pending-switch", "pending-self"):
                pending_load = PausedReply("GET")
                pending_record = a if case == "pending-self" else b
                pending_url = history + "/" + pending_record["id"]
                await page.route(pending_url, pending_load.handle)
                if case == "pending-self":
                    await page.get_by_role("button", name=a["conversation"]["title"], exact=True).click()
                    await asyncio.wait_for(pending_load.entered.wait(), 10)
            await page.route(delete_url, deletion.handle)
            try:
                page.once("dialog", lambda dialog: dialog.accept())
                await page.locator("#delete").click()
                await asyncio.wait_for(deletion.entered.wait(), 10)
                await expect(page.locator("#send")).to_be_disabled()
                await expect(page.locator("#delete")).to_be_disabled()
                if case == "switch":
                    await open_record(b)
                    await expect(page.locator("#send")).to_be_enabled()
                elif case == "pending-switch":
                    await page.get_by_role("button", name=b["conversation"]["title"], exact=True).click()
                    await asyncio.wait_for(pending_load.entered.wait(), 10)
                elif case == "new":
                    await page.locator("#new-chat").click()
                    await page.locator("#prompt").fill("Keep this draft")
                elif case == "logout":
                    await page.locator("#sign-out").click()
                deletion.release.set()
                await page.evaluate("window.raceHistoryDelete")
                if pending_load:
                    pending_load.release.set()
                    await page.evaluate("window.raceHistoryLoad")
                if case == "failure":
                    assert await read(a["id"]) == a
                    await expect(page.locator("#notice")).to_contain_text("delayed history failure")
                    await expect(page.locator("#send")).to_be_enabled()
                    await expect(page.locator("#delete")).to_be_enabled()
                elif case == "logout":
                    await expect(page.locator("#login")).to_be_visible()
                    await expect(page.locator("#history button")).to_have_count(0)
                    await expect(page.locator("#notice")).to_have_text("")
                    await page.locator("#token").fill(token)
                    await page.locator("#login-form button").click()
                    await expect(page.locator("#composer-area")).to_be_visible()
                else:
                    assert (await page.request.get(delete_url, headers=headers)).status == 404
                    await expect(page.get_by_role("button", name=a["conversation"]["title"], exact=True)).to_have_count(0)
                    await expect(page.get_by_role("button", name=b["conversation"]["title"], exact=True)).to_be_visible()
                    assert await read(b["id"]) == b
                    if case in ("switch", "pending-switch"):
                        await expect(page.locator("#history button.active")).to_have_text(b["conversation"]["title"])
                        await expect(page.locator(".user .content")).to_have_text(b["conversation"]["title"])
                    else:
                        await expect(page.locator("#prompt")).to_have_value("Keep this draft" if case == "new" else "")
                        await expect(page.locator(".message")).to_have_count(0)
            finally:
                deletion.release.set()
                await page.unroute(delete_url, deletion.handle)
                if pending_load:
                    pending_load.release.set()
                    await page.unroute(pending_url, pending_load.handle)
    finally:
        await page.evaluate("""() => {
            openConversation = window.raceOriginalLoad;
            document.getElementById('delete').onclick = window.raceOriginalDelete;
        }""")
    print("WEB HISTORY RACES: PASS (existing/new turns, stale success/error, stop/retry, unsaved export, delete/navigation/logout/failure)", flush=True)


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
        await history_races(page, port, alice)
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
                "demand": {"snapshot_age_ms": 0, "window_complete": False, "models": [{"id": MODEL,
                    "admitted": 1, "opening": 2, "completed": 3, "capacity_rejected": 4,
                    "failed": 1, "interrupted": 0, "cancelled": 2, "latency": {"ttft_ms": {"p95": 120},
                    "completion_ms": {"p95": 900}, "completion_samples": 3}}]},
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
        await expect(page.locator("#workspace-summary")).to_contain_text("Partial window")
        await expect(page.locator("#workspace-allocations")).to_contain_text("4 capacity rejected")
        await expect(page.locator("#workspace-allocations")).to_contain_text("120 ms / 900 ms")
        assert await page.locator("#workspace script").count() == 0
        assert await page.locator(".allocation-actions button:visible").count() == 0, "read-only operator got mutation controls"
        await page.screenshot(path=str(artifacts / "web-workspace.png"))
        # Real credential-bound management grant; explicit transport fixture
        # below exercises browser confirmation and exact uint64 fences. The
        # native resident flow separately invokes actual host/keeper handlers.
        subprocess.run([str(ROOT / "lumabri"), "api", "manage-grant", "bob"], env=env,
                       capture_output=True, text=True, check=True, timeout=10)
        await page.locator("#refresh").click()
        await page.locator("#workspace-refresh").click()
        await expect(page.locator("#workspace-allocations .allocation-actions button")).to_have_count(4)
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
        await expect(page.locator(".allocation-actions button:visible")).to_have_count(0)
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
        await elastic_workspace(browser, port, bob, env, artifacts)
        await capacity_workspace(browser, port, bob, env, artifacts)
        # Documentation image: real compiled UI with explicitly labelled
        # example conversation, not a claim about model output quality.
        preview = await browser.new_page(viewport={"width": 1365, "height": 900})
        await preview.route("**/api/v1/models", lambda route: route.fulfill(json={"models": [
            {"id": MODEL, "name": "OLMoE · interface example", "adapter": "olmoe", "max_tokens": 256}]}))
        response = await preview.request.post(f"http://127.0.0.1:{port}/api/v1/conversations",
            headers={"Authorization": "Bearer " + bob}, data={"revision": 0, "conversation": {
                "model": MODEL, "title": "A place for your ideas", "state": "complete", "messages": [
                    {"role": "user", "content": "Help me plan a quiet weekend in the garden."},
                    {"role": "assistant", "content": "Keep it simple: one useful task, something to enjoy, and time to rest.\n\nStart with an hour of planting or tidying while it is cool. Make lunch outside, then leave the afternoon free for a book or a walk.\n\nWhat would you most like to grow?"}]}})
        assert response.ok, await response.text()
        await preview.goto(f"http://127.0.0.1:{port}/")
        await preview.locator("#token").fill(bob); await preview.locator("#login-form button").click()
        await preview.get_by_role("button", name="A place for your ideas", exact=True).click()
        await expect(preview.locator("#messages .assistant .content")).to_contain_text("Keep it simple")
        await preview.screenshot(path=str(artifacts / "web-chat-preview.png"))
        await preview.close()
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
