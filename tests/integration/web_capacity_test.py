"""Compiled browser controls with explicit capacity transport fixtures.
Real OLMoE measurements and cleanup are tested by placement_comparison_test.
"""
import asyncio
import copy
import json
import subprocess
import time
from pathlib import Path

from playwright.async_api import expect

ROOT = Path(__file__).resolve().parents[2]
A, B, M, N = (c * 64 for c in "1234")


async def capacity_workspace(browser, port, token, env, artifacts):
    page = await browser.new_page(viewport={"width": 1440, "height": 1100})
    failures, posts = [], []
    page.on("pageerror", lambda error: failures.append(str(error)))
    data = {"schema": 1, "observation_id": "a" * 64, "state": "measured", "measured_at": int(time.time()), "stable": True,
            "clients": 2, "rounds": 3, "max_tokens": 8, "cost": None,
            "models": [{"allocation": a, "requests": 3, "completed": 3, "rejected": 0, "failed": 0,
                        "ttft_p95_seconds": .05, "gap_p95_of_response_p95_seconds": .01} for a in (A, B)]}
    state = {"records": [{"name": "baseline", "observation": copy.deepcopy(data)}],
             "check": "observed_workload_passed", "hold_list": False, "list_error": False,
             "measure": "normal", "apply_error": False}
    entered, release, measuring, finish = (asyncio.Event() for _ in range(4))
    workspace = {"schema": 1, "captured_at": int(time.time()), "inventory_ok": True, "registry_ok": True,
                 "inventory_ttl_ms": 60000, "nodes": [], "demand": None, "models": [], "allocations": []}
    for i, (a, m) in enumerate(((A, M), (B, N))):
        workspace["allocations"].append({"id": a, "name": f"OLMoE {i + 1} <not markup>", "adapter": "olmoe",
            "context": 128, "session_limit": 2, "preparation_seconds": None, "ranges": [],
            "observation": {"state": "unknown", "decode_tok_s": None}})
        workspace["models"].append({"id": m, "name": f"Managed {i + 1}", "adapter": "olmoe", "revision": 9,
            "state": "saved_route", "replicas": 1, "allocations": [a], "context": 128, "max_tokens": 8})

    async def reply(route):
        action = route.request.url.split("/capacity", 1)[1]
        if not action:
            snapshot, error = copy.deepcopy(state["records"]), state["list_error"]
            if state["hold_list"]:
                state["hold_list"] = False; entered.set(); await asyncio.wait_for(release.wait(), 15)
            await route.fulfill(status=503 if error else 200, json={"error": "capacity_storage_unavailable"} if error else
                                {"scope": "historical_not_revalidated", "records": snapshot})
            return
        body = route.request.post_data_json; posts.append((action, copy.deepcopy(body)))
        if action == "/measure":
            assert body["allocations"] == [B, A], body  # UI must preserve explicitly chosen order.
            assert set(body) == {"record", "clients", "rounds", "max_tokens", "allocations"}
            measured = copy.deepcopy(data)
            measured["models"].reverse()
            if state["measure"] == "hold":
                measuring.set(); await asyncio.wait_for(finish.wait(), 15)
            events = [("progress", {"completed_rounds": n, "rounds": 3, "weights_changed": False}) for n in range(4)]
            events.append(("result", measured))
            if state["measure"] == "invalid": events[1][1]["completed_rounds"] = 3
            else: state["records"].append({"name": body["record"], "observation": measured})
            await route.fulfill(content_type="text/event-stream", body="".join(
                "event: " + event + "\ndata: " + json.dumps(value) + "\n\n" for event, value in events))
        elif action == "/check":
            measured = copy.deepcopy(next(r["observation"] for r in state["records"] if r["name"] == body["record"]))
            measured["state"] = state["check"]
            await route.fulfill(json=measured)
        elif action == "/select":
            assert body["records"] == ["baseline", "candidate"] and body["ttft_ms"] == 1000, body
            await route.fulfill(json={"selected": "candidate", "applied": False, "objective": "operator_order_prices_unknown_or_mixed",
                "candidates": [{"name": n, "state": "observed_workload_passed", "declared_machine_cost": None} for n in body["records"]]})
        elif action == "/apply":
            assert body["observation_id"] == "a" * 64, body
            assert body["record"] == "candidate" and body["models"] == [{"id": N, "revision": 9}, {"id": M, "revision": 9}], body
            await route.fulfill(status=409 if state["apply_error"] else 200, json={"error": "model_revision_or_contract_changed"}
                                if state["apply_error"] else {"applied": True, "weights_released": False})
        elif action == "/remove":
            assert body == {"record": "candidate"}
            state["records"] = [r for r in state["records"] if r["name"] != body["record"]]
            await route.fulfill(json={"removed": True, "weights_changed": False})
        else:
            raise AssertionError(action)

    await page.route("**/api/v1/capacity", reply)
    await page.route("**/api/v1/capacity/*", reply)
    await page.route("**/api/v1/workspace", lambda route: route.fulfill(json=workspace))
    await page.route("**/api/v1/elastic-policy", lambda route: route.fulfill(json={"policies": []}))
    await page.route("**/api/v1/catalogue", lambda route: route.fulfill(json={"models": []}))

    def permission(action):
        subprocess.run([str(ROOT / "lumabri"), "api", action, "bob"], env=env,
                       check=True, capture_output=True, text=True, timeout=10)

    def record(name):
        return page.locator("#capacity-records article").filter(has=page.get_by_role("heading", name=name, exact=True))

    async def check_candidate():
        await record("candidate").get_by_role("button", name="Check limits", exact=True).click()
        await expect(page.locator("#capacity-apply-form")).to_be_visible()

    async def choose_routes():
        await page.locator("#capacity-route-models select").nth(0).select_option(N)
        await page.locator("#capacity-route-models select").nth(1).select_option(M)

    try:
        permission("manage-revoke")
        await page.goto(f"http://127.0.0.1:{port}/")
        await page.locator("#token").fill(token); await page.locator("#login-form button").click()
        await page.locator("#workspace-open").click()
        await expect(record("baseline")).to_contain_text("Historical — not revalidated")
        await expect(page.locator("#capacity-measure-details")).to_be_hidden()
        await expect(record("baseline").get_by_role("button", name="Check limits")).to_be_hidden()
        assert not posts
        permission("manage-grant"); await page.locator("#refresh").click()
        await page.locator("#capacity-measure-details summary").click()
        await page.locator("#capacity-name").fill("candidate")
        await page.locator("#capacity-slots select").first.select_option(B)
        await page.locator("#capacity-add").click()
        await page.locator("#capacity-slots select").nth(1).select_option(A)
        await page.locator("#capacity-form button[type=submit]").click()
        await expect(page.locator("#capacity-status")).to_contain_text("at least one client")
        assert not posts
        await page.locator("#capacity-clients").fill("2")
        page.once("dialog", lambda dialog: dialog.dismiss())
        await page.locator("#capacity-form button[type=submit]").click()
        assert not posts
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#capacity-form button[type=submit]").click()
        await expect(page.locator("#capacity-status")).to_contain_text("Measurement measured")
        await expect(page.locator("#capacity-apply-form")).to_be_hidden()
        assert posts[-1][1] == {"record": "candidate", "clients": 2, "rounds": 3, "max_tokens": 8, "allocations": [B, A]}
        await page.locator("#capacity-refresh").click()
        await expect(record("candidate")).to_be_visible()
        await check_candidate()
        await choose_routes()
        count = len(posts)
        page.once("dialog", lambda dialog: dialog.dismiss())
        await page.locator("#capacity-apply-form button").click()
        assert len(posts) == count
        await page.locator("#capacity-gap").fill("250")
        await expect(page.locator("#capacity-apply-form")).to_be_hidden()
        await page.locator("#capacity-gap").fill("200")
        await page.locator("#capacity-baseline").select_option("baseline")
        await page.locator('#capacity-candidates input[value="candidate"]').check()
        await page.locator("#capacity-compare button").click()
        await expect(page.locator("#capacity-apply-form")).to_be_visible()
        await expect(page.locator("#capacity-result")).to_contain_text("Price unknown")
        assert not any(action == "/apply" for action, _ in posts)
        await choose_routes()
        await page.locator("#capacity").scroll_into_view_if_needed()
        await page.screenshot(path=str(artifacts / "web-capacity-comparison.png"))
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#capacity-apply-form button").click()
        await expect(page.locator("#capacity-status")).to_contain_text("Routing applied atomically")
        await expect(page.locator("#capacity-apply-form")).to_be_hidden()
        # A stale polling error cannot overwrite a later check or its review.
        state.update(hold_list=True, list_error=True)
        await page.evaluate("() => { window.capacityPoll = capacityPanel.refresh(); }")
        await asyncio.wait_for(entered.wait(), 10)
        await check_candidate()
        release.set(); await page.evaluate("window.capacityPoll")
        await expect(page.locator("#capacity-status")).to_contain_text("observed workload passed")
        await expect(page.locator("#capacity-apply-form")).to_be_visible()
        state["list_error"] = False
        await choose_routes(); state["apply_error"] = True
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#capacity-apply-form button").click()
        await expect(page.locator("#capacity-status")).to_contain_text("No automatic retry")
        await expect(page.locator("#capacity-apply-form")).to_be_hidden()
        assert len([p for p in posts if p[0] == "/apply"]) == 2
        state["check"] = "configuration_changed"
        await record("candidate").get_by_role("button", name="Check limits", exact=True).click()
        await expect(page.locator("#capacity-status")).to_contain_text("configuration changed")
        await expect(page.locator("#capacity-apply-form")).to_be_hidden()
        # Protocol failures and user cancellation never become successful evidence.
        state["measure"] = "invalid"
        await page.locator("#capacity-name").fill("invalid")
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#capacity-form button[type=submit]").click()
        await expect(page.locator("#capacity-status")).to_contain_text("Invalid measurement progress")
        state["measure"] = "hold"
        await page.locator("#capacity-name").fill("cancelled")
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#capacity-form button[type=submit]").click()
        await asyncio.wait_for(measuring.wait(), 10)
        await page.locator("#capacity-stop").click()
        await expect(page.locator("#capacity-status")).to_contain_text("Measurement stopped")
        await expect(page.locator("#capacity-stop")).to_be_hidden()
        finish.set()
        page.once("dialog", lambda dialog: dialog.dismiss())
        count = len(posts)
        await record("candidate").get_by_role("button", name="Remove record", exact=True).click()
        assert len(posts) == count
        page.once("dialog", lambda dialog: dialog.accept())
        await record("candidate").get_by_role("button", name="Remove record", exact=True).click()
        await expect(page.locator("#capacity-status")).to_contain_text("Measurement removed")
        await expect(record("candidate")).to_have_count(0)
        await page.set_viewport_size({"width": 390, "height": 844})
        await page.locator("#collapse").click()
        assert await page.evaluate("document.documentElement.scrollWidth <= innerWidth")
        await page.screenshot(path=str(artifacts / "web-capacity-mobile.png"))
        permission("manage-revoke"); await page.locator("#refresh").click()
        await expect(page.locator("#capacity-measure-details")).to_be_hidden()
        await expect(page.locator("#capacity-apply-form")).to_be_hidden()
        # Pending GET success cannot reintroduce private records after navigation.
        entered.clear(); release.clear(); state["hold_list"] = True
        await page.evaluate("() => { window.capacityPoll = capacityPanel.refresh(); }")
        await asyncio.wait_for(entered.wait(), 10)
        await page.locator("#expand").click(); await page.locator("#new-chat").click()
        release.set(); await page.evaluate("window.capacityPoll")
        await expect(page.locator("#capacity-records article")).to_have_count(0)
        assert await page.evaluate("localStorage.length === 0 && sessionStorage.length === 0")
        assert not failures, failures
        print("WEB CAPACITY: PASS (ordered workload, permission, check/compare/apply, revision conflict, cancellation, malformed stream, stale poll, private navigation, mobile)", flush=True)
    finally:
        release.set(); finish.set(); permission("manage-revoke"); await page.close()
