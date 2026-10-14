"""Browser contract tests. Real login/permissions and compiled assets; explicit
policy/registry transport fixtures, not a substitute for real OLMoE lifecycle.
"""
import asyncio
import copy
import subprocess
import time
from pathlib import Path
from playwright.async_api import expect

MODEL, NEW_MODEL, ALLOCATION = "01" * 32, "04" * 32, "05" * 32
NODE, OTHER_NODE = "02" * 32, "03" * 32
ROOT = Path(__file__).resolve().parents[2]


async def elastic_workspace(browser, port, token, env, artifacts):
    page = await browser.new_page(viewport={"width": 1440, "height": 1000})
    failures = []
    page.on("pageerror", lambda error: failures.append(str(error)))
    posts = []
    huge = "9007199254740993"
    policy = {"id": MODEL, "revision": 7, "route_revision": 9, "enabled": True, "faulted": False,
              "phase": "idle", "replicas": 1, "min_replicas": 1, "max_replicas": 2,
              "reserved_bytes": "1000000000", "max_reserved_bytes": huge, "cooldown_seconds": 60,
              "idle_seconds": 300, "up_rejections": 3, "ttft_ms": 0, "source_model": "olmoe",
              "context": 512, "sessions": 2, "max_new": 128, "nodes": [NODE],
              "adopt_allocations": [ALLOCATION], "reason": "within_limits", "last_check": int(time.time()),
              "pending_operation": "", "pending_allocation": ""}
    state = {"policies": [policy], "offline": False, "conflict": False, "pause_load": False, "paused_error": False}
    entered, release = asyncio.Event(), asyncio.Event()
    workspace = {"schema": 1, "inventory_ok": True, "registry_ok": True, "captured_at": int(time.time()),
                 "inventory_ttl_ms": 60000, "nodes": [], "models": [], "allocations": [], "demand": None}
    for id, name in ((NODE, "Approved PC"), (OTHER_NODE, "Unselected PC")):
        workspace["nodes"].append({"id": id, "name": name, "cpu": "Test CPU", "os": "Linux", "arch": "x86_64",
                                  "age_ms": 0, "runtime_threads": 2, "threads": 2, "ram_total_bytes": 8 << 30,
                                  "ram_available_bytes": 6 << 30, "ram_offered_bytes": 4 << 30, "gpu_detected": 0,
                                  "vram_inventory_bytes": 0, "workload": None, "machine_cost": None, "power": None})
    for id, name in ((MODEL, "OLMoE"), (NEW_MODEL, "Second model")):
        workspace["models"].append({"id": id, "name": name, "adapter": "olmoe", "replicas": 1,
                                    "context": 512, "max_tokens": 128, "revision": 9,
                                    "state": "saved_route", "allocations": [ALLOCATION]})
    workspace["allocations"].append({"id": ALLOCATION, "name": "Owned replica", "adapter": "olmoe",
        "context": 512, "session_limit": 2, "preparation_seconds": None, "ranges": [],
        "observation": {"state": "unknown", "decode_tok_s": None}})

    async def workspace_reply(route):
        workspace["captured_at"] = int(time.time())
        await route.fulfill(json=workspace)

    async def policy_reply(route):
        if route.request.method == "GET":
            snapshot = copy.deepcopy(state["policies"])
            offline = state["offline"]
            if state["pause_load"]:
                state["pause_load"] = False  # Only this GET, not the action's fresh inspection.
                offline = state["paused_error"]
                entered.set(); await asyncio.wait_for(release.wait(), 15)
            if offline:
                await route.fulfill(status=503, json={"error": "elastic_state_unavailable"})
            else:
                await route.fulfill(json={"schema": 1, "policies": snapshot})
            return
        body = route.request.post_data_json; posts.append(copy.deepcopy(body))
        current = next((p for p in state["policies"] if p["id"] == body["id"]), None)
        if state["conflict"]:
            state["conflict"] = False
            await route.fulfill(status=409, json={"error": "elastic_revision_or_ownership_conflict"}); return
        assert body["revision"] == (current["revision"] if current else 0), body
        if body["action"] == "configure":
            expected = {"action", "id", "revision", "route_revision", "enabled", "min_replicas", "max_replicas",
                        "cooldown_seconds", "idle_seconds", "up_rejections", "ttft_ms", "max_reserved_bytes",
                        "source_model", "nodes", "context", "sessions", "max_new", "adopt_allocations"}
            assert set(body) == expected and body["nodes"] == [NODE] and body["adopt_allocations"] == [ALLOCATION], body
            if current is None:
                current = copy.deepcopy(policy); current["id"] = body["id"]; state["policies"].append(current)
            current.update({k: v for k, v in body.items() if k != "action"})
        else:
            assert set(body) == {"action", "id", "revision"}, body
            if body["action"] in ("disable", "resume"):
                current["enabled"] = body["action"] == "resume"
        current["revision"] = body["revision"] + 1
        current["reason"] = "operator_change_recorded"
        await route.fulfill(json=current)

    await page.route("**/api/v1/workspace", workspace_reply)
    await page.route("**/api/v1/elastic-policy", policy_reply)
    await page.route("**/api/v1/catalogue", lambda route: route.fulfill(json={"models": [
        {"name": "olmoe", "adapter": "olmoe", "checkpoint_bytes": 1000000000, "source_ready": True}]}))

    def permission(action):
        subprocess.run([str(ROOT / "lumabri"), "api", action, "bob"], env=env,
                       text=True, capture_output=True, check=True, timeout=10)

    async def refresh():
        await page.locator("#workspace-refresh").click()
        await expect(page.locator("#elastic-status")).to_contain_text("Saved policies")

    async def editor(name):
        card = page.locator("#workspace-models article").filter(has=page.get_by_role("heading", name=name, exact=True))
        await card.get_by_role("button", name="Manage replicas", exact=True).click()
        await expect(page.locator("#elastic-form")).to_be_visible()

    async def review():
        await page.get_by_role("button", name="Review policy", exact=True).click()
        await expect(page.locator("#elastic-apply")).to_be_visible()

    try:
        await page.goto(f"http://127.0.0.1:{port}/")
        await page.locator("#token").fill(token); await page.locator("#login-form button").click()
        await expect(page.locator("#workspace-open")).to_be_visible()
        await page.locator("#workspace-open").click()
        await expect(page.locator("#elastic-policies")).to_contain_text("Registered replicas / limits")
        assert await page.locator("#elastic-policies").get_attribute("inert") is None
        assert await page.locator("#elastic-policies button").count() == 0
        assert await page.get_by_role("button", name="Manage replicas", exact=True).count() == 0
        permission("manage-grant")
        await page.locator("#refresh").click(); await refresh()
        await editor("OLMoE")
        assert await page.locator("#elastic-nodes input:checked").count() == 1
        assert await page.locator("#elastic-nodes input:checked").input_value() == NODE
        exact_budget = await page.locator("#elastic-budget").input_value()
        await review()
        await expect(page.locator("#elastic-review")).to_contain_text(huge + " bytes")
        page.once("dialog", lambda dialog: dialog.dismiss())
        await page.locator("#elastic-apply").click()
        assert not posts, "cancelled review mutated the policy"
        await page.locator("#elastic-budget").fill("8.25")
        await expect(page.locator("#elastic-apply")).to_be_hidden()
        await page.locator("#elastic-budget").fill(exact_budget)
        await review()
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#elastic-apply").click()
        await expect(page.locator("#elastic-status")).to_contain_text("Policy saved at revision 8")
        assert len(posts) == 1 and posts[-1]["max_reserved_bytes"] == huge
        await refresh()
        # An old polling success must not restore Enabled after a pause;
        # an old polling failure must not erase the later resume outcome.
        state["pause_load"] = True
        await page.evaluate("() => { window.elasticPoll = refreshElastic(); }")
        await asyncio.wait_for(entered.wait(), 10)
        page.once("dialog", lambda dialog: dialog.accept())
        await page.get_by_role("button", name="Pause policy", exact=True).click()
        await expect(page.locator("#elastic-status")).to_contain_text("Policy revision 9")
        assert posts[-1] == {"action": "disable", "id": MODEL, "revision": 8}
        release.set(); await page.evaluate("window.elasticPoll")
        await expect(page.locator("#elastic-status")).to_contain_text("Policy revision 9")
        await expect(page.locator("#elastic-policies article")).to_have_count(0)
        entered.clear(); release.clear()
        await refresh()
        state.update(pause_load=True, paused_error=True)
        await page.evaluate("() => { window.elasticPoll = refreshElastic(); }")
        await asyncio.wait_for(entered.wait(), 10)
        page.once("dialog", lambda dialog: dialog.accept())
        await page.get_by_role("button", name="Resume policy", exact=True).click()
        await expect(page.locator("#elastic-status")).to_contain_text("Policy revision 10")
        release.set(); await page.evaluate("window.elasticPoll")
        await expect(page.locator("#elastic-status")).to_contain_text("Policy revision 10")
        await expect(page.locator("#elastic-policies article")).to_have_count(0)
        entered.clear(); release.clear(); state["paused_error"] = False
        await refresh()
        await editor("Second model")
        await expect(page.locator("#elastic-nodes input:checked")).to_have_count(0)
        await expect(page.locator("#elastic-budget")).to_have_value("")
        await expect(page.locator("#elastic-enabled")).not_to_be_checked()
        await page.locator("#elastic-source").select_option("olmoe")
        await page.locator("#elastic-budget").fill("1.5")
        await page.get_by_role("button", name="Review policy", exact=True).click()
        await expect(page.locator("#elastic-status")).to_contain_text("Choose at least one authorized computer")
        await page.locator(f'#elastic-nodes input[value="{NODE}"]').check()
        await review()
        await page.screenshot(path=str(artifacts / "web-elastic-policy.png"))
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#elastic-apply").click()
        await expect(page.locator("#elastic-status")).to_contain_text("Policy saved at revision 1")
        assert posts[-1]["id"] == NEW_MODEL and posts[-1]["max_reserved_bytes"] == str(3 << 29)
        assert not posts[-1]["enabled"]
        await refresh(); await editor("OLMoE"); await review()
        count = len(posts); state["conflict"] = True
        page.once("dialog", lambda dialog: dialog.accept())
        await page.locator("#elastic-apply").click()
        await expect(page.locator("#elastic-status")).to_contain_text("Reload and review")
        await expect(page.locator("#elastic-apply")).to_be_hidden()
        assert len(posts) == count + 1, "revision conflict caused an automatic retry"
        await page.locator("#elastic-close").click()
        policy.update(phase="preparing", faulted=True, pending_operation="a" * 64, pending_allocation="b" * 64,
                      reason="exact_receipt_uncertain", enabled=False)
        await refresh()
        first_card = page.locator("#elastic-policies article").first
        await expect(first_card.get_by_role("button", name="Edit policy", exact=True)).to_be_disabled()
        await expect(first_card.get_by_role("button", name="Resume policy", exact=True)).to_be_disabled()
        page.once("dialog", lambda dialog: dialog.accept())
        await first_card.get_by_role("button", name="Reconcile preparation", exact=True).click()
        await expect(page.locator("#elastic-status")).to_contain_text("Policy revision")
        assert posts[-1] == {"action": "reconcile", "id": MODEL, "revision": 10}
        assert policy["reserved_bytes"] == "1000000000" and policy["pending_operation"] == "a" * 64
        await refresh(); await editor("Second model")
        await page.set_viewport_size({"width": 390, "height": 844})
        await page.locator("#collapse").click()
        assert await page.evaluate("document.documentElement.scrollWidth <= innerWidth")
        await page.screenshot(path=str(artifacts / "web-elastic-mobile.png"))
        permission("manage-revoke")
        await page.locator("#refresh").click()
        await expect(page.locator("#elastic-form")).to_be_hidden()
        await refresh()
        await expect(page.locator("#elastic-policies button")).to_have_count(0)
        state["offline"] = True
        await page.locator("#workspace-refresh").click()
        await expect(page.locator("#elastic-status")).to_contain_text("Replica policies unavailable")
        await expect(page.locator("#elastic-policies article")).to_have_count(0)
        state["offline"] = False
        permission("manage-grant")
        await page.locator("#refresh").click(); await refresh()
        state["pause_load"] = True
        await page.evaluate("""() => {
            const open = openElasticEditor;
            openElasticEditor = id => window.elasticEditTask = open(id);
        }""")
        await page.locator("#workspace-models article").last.get_by_role("button", name="Manage replicas", exact=True).click()
        await asyncio.wait_for(entered.wait(), 10)
        await page.locator("#expand").click(); await page.locator("#new-chat").click()
        release.set(); state["pause_load"] = False
        # Await the actual handler promise. wait_for_function installs an
        # in-page eval poller rejected by newer Chromium under our CSP.
        # Do not weaken script-src or bypass CSP just for synchronization.
        await page.evaluate("window.elasticEditTask")
        await expect(page.locator("#elastic-form")).to_be_hidden()
        await expect(page.locator("#workspace")).to_be_hidden()
        assert not failures, failures
    finally:
        release.set(); permission("manage-revoke"); await page.close()
    print("WEB ELASTIC: PASS (read-only operator, exact budget, explicit computers, review/cancel, fenced edits, pause/resume/reconcile, stale reply, revocation, mobile)", flush=True)
