"""Optional browser gate over the real resident Colibri inference service."""
from playwright.sync_api import sync_playwright, expect


def verify_resident_browser(port, alice, bob, model, artifacts):
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1365, "height": 900})
        failures = []
        page.on("pageerror", lambda error: failures.append(str(error)))
        try:
            page.goto(f"http://127.0.0.1:{port}/")
            page.locator("#token").fill(alice)
            page.locator("#login-form button").click()
            expect(page.locator("#prompt")).to_be_enabled()
            assert page.locator("#model").input_value() == model
            page.locator("#prompt").fill("hi"); page.locator("#send").click()
            expect(page.locator("#stop")).to_be_hidden(timeout=30000)
            assert page.locator("#save-status").inner_text().startswith("Saved"), page.locator("#notice").inner_text()
            answer = page.locator(".assistant .content").text_content()
            assert answer, page.locator("#notice").inner_text()
            assert page.locator("#notice").inner_text() == ""
            page.screenshot(path=str(artifacts / "real-olmoe-web.png"))
            headers = {"Authorization": "Bearer " + alice}
            listing = page.request.get(f"http://127.0.0.1:{port}/api/v1/conversations", headers=headers).json()
            chat = next(item for item in listing["conversations"] if item["title"] == "hi")
            saved = page.request.get(f"http://127.0.0.1:{port}/api/v1/conversations/{chat['id']}", headers=headers).json()
            assert saved["conversation"]["state"] == "complete"
            assert saved["conversation"]["messages"][-1]["content"] == answer
            page.reload(); expect(page.locator("#login")).to_be_visible()
            page.locator("#token").fill(alice); page.locator("#login-form button").click()
            page.locator("#history button", has_text="hi").click()
            expect(page.locator(".assistant .content")).to_have_count(1)
            assert page.locator(".assistant .content").text_content() == answer
            page.locator("#sign-out").click()
            page.locator("#token").fill(bob); page.locator("#login-form button").click()
            expect(page.locator("#composer-area")).to_be_visible()
            assert page.locator("#history button").count() == 0
            assert not failures, failures
        except Exception:
            page.screenshot(path=str(artifacts / "real-olmoe-web-failed.png"))
            (artifacts / "real-olmoe-web-failed.txt").write_text(page.locator("body").inner_text())
            raise
        finally:
            browser.close()
    print("RESIDENT WEB: PASS (real Colibri OLMoE generation, private saved reply, reload, isolated second user)", flush=True)
