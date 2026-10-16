"""Actual browser -> capacity API -> resident reduced-OLMoE engines.
No request interception, artificial timing, or fabricated inference results.
The surrounding fixture owns, approves and cleans up every allocation.
"""
import json

from playwright.sync_api import sync_playwright, expect


def verify_browser_capacity(api, managed, static, artifacts):
    routes = [json.loads(api.cli("model-add", f"browser-capacity-{i}", allocation["id"], "--tracker", api.tracker))
              for i, allocation in enumerate(static)]
    for route in routes:
        api.cli("grant", "preparer", route["id"])
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        try:
            page = browser.new_page(viewport={"width": 1440, "height": 1100})
            failures = []
            page.on("pageerror", lambda e: failures.append(str(e)))
            page.goto(f"http://127.0.0.1:{api.port}/")
            page.locator("#token").fill(api.token); page.locator("#login-form button").click()
            page.locator("#workspace-open").click()
            expect(page.locator("#capacity-records")).to_contain_text("placement-static")
            page.locator("#capacity-measure-details summary").click()
            page.locator("#capacity-name").fill("browser-measured")
            page.locator("#capacity-slots select").first.select_option(managed[0]["id"])
            page.locator("#capacity-add").click()
            page.locator("#capacity-slots select").nth(1).select_option(managed[1]["id"])
            page.locator("#capacity-clients").fill("2")
            page.once("dialog", lambda dialog: dialog.accept())
            page.locator("#capacity-form button[type=submit]").click()
            expect(page.locator("#capacity-status")).to_contain_text("Measurement measured", timeout=180000)
            page.locator("#capacity-refresh").click()
            expect(page.locator("#capacity-records")).to_contain_text("browser-measured")
            page.locator("#capacity-ttft").fill("30000"); page.locator("#capacity-gap").fill("30000")
            page.locator("#capacity-age").fill("3600")
            page.locator("#capacity-baseline").select_option("placement-static")
            page.locator('#capacity-candidates input[value="browser-measured"]').check()
            page.locator("#capacity-compare button").click()
            expect(page.locator("#capacity-apply-form")).to_be_visible(timeout=30000)
            expect(page.locator("#capacity-status")).to_contain_text("Selected browser-measured")
            # Replacing a record under its old name cannot apply a different
            # observation to the user's reviewed model revisions.
            unchanged = {"record": "browser-measured", "observation_id": "00" * 32, "ttft_ms": 30000,
                         "gap_ms": 30000, "max_age_seconds": 3600,
                         "models": [{"id": r["id"], "revision": r["revision"]} for r in routes]}
            code, refused = api.http("/api/v1/capacity/apply", unchanged)
            assert code == 409, refused
            code, before_apply = api.http("/api/v1/workspace")
            assert code == 200
            original = {m["id"]: m for m in before_apply["models"]}
            assert all(original[r["id"]]["revision"] == r["revision"] and
                       original[r["id"]]["allocations"] == [a["id"]] for r, a in zip(routes, static)), original
            for i, route in enumerate(routes):
                page.locator("#capacity-route-models select").nth(i).select_option(route["id"])
            page.locator("#capacity-apply-form").scroll_into_view_if_needed()
            page.screenshot(path=str(artifacts / "browser-capacity-real-olmoe.png"))
            page.once("dialog", lambda dialog: dialog.accept())
            page.locator("#capacity-apply-form button").click()
            expect(page.locator("#capacity-status")).to_contain_text("Routing applied atomically", timeout=30000)
            code, workspace = api.http("/api/v1/workspace")
            assert code == 200
            saved = {m["id"]: m for m in workspace["models"]}
            assert all(saved[r["id"]]["revision"] == r["revision"] + 1 and
                       saved[r["id"]]["allocations"] == [a["id"]] for r, a in zip(routes, managed)), saved
            # Both model routes actually answer through the same web chat.
            for route in routes:
                page.locator("#new-chat").click()
                page.locator("#model").select_option(route["id"])
                page.locator("#prompt").fill("Hello")
                page.locator("#send").click()
                expect(page.locator("#save-status")).to_have_text("Saved privately on this computer", timeout=30000)
                expect(page.locator("#messages .assistant .content")).not_to_be_empty()
            assert not failures, failures
            code, record = api.http("/api/v1/capacity/check", {"record": "browser-measured", "ttft_ms": 30000,
                "gap_ms": 30000, "max_age_seconds": 3600})
            assert code == 200 and record["state"] == "observed_workload_passed", record
            assert all(m["completed"] == 3 and not m["rejected"] and not m["failed"] for m in record["models"]), record
            (artifacts / "browser-capacity-real-olmoe.json").write_text(json.dumps(
                {"scope": "loopback_reduced_olmoe_browser", "record": record,
                 "routes": [saved[r["id"]] for r in routes]}, indent=2))
        finally:
            browser.close()
            for route in routes:
                api.cli("model-remove", route["id"])
    print("BROWSER CAPACITY OLMOE: PASS (real two-model measurement, comparison, atomic routing, private chats; no HTTP fixtures)", flush=True)
