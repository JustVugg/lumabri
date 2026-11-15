"""The "Get a model" card in the real compiled page, against the real gateway and a hub imitated on loopback.
Requires Playwright Chromium; never included in native runtime dependencies.
"""
import asyncio
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
from playwright.async_api import async_playwright, expect
from model_download_test import Hub, Repo, lock_free, standard_files, tree

ROOT = Path(__file__).resolve().parents[2]


async def scenario(link, hub, home):
    models, catalogue = home / ".lumabri/models", home / ".lumabri/download-catalogue.json"
    async with async_playwright() as playwright:
        browser = await playwright.chromium.launch()
        page = await browser.new_page(viewport={"width": 1280, "height": 900})
        problems = []
        page.on("pageerror", lambda error: problems.append(str(error)))
        outside = []
        page.on("request", lambda request: outside.append(request.url)
                if not request.url.startswith(link.split("#")[0]) else None)
        card, status = page.locator("#get-model"), page.locator("#get-model-status")
        download, cancel = page.locator("#get-model-download"), page.locator("#get-model-cancel")

        # A model that fits: its facts, and nothing started by opening the page.
        await page.goto(link)
        await expect(card).to_be_visible()
        await expect(card).to_contain_text("Tiny fixture")
        await expect(card).to_contain_text("Test licence")
        await expect(card).to_contain_text("an estimate until the model is on disk")
        await expect(card).to_contain_text(str(models))
        await expect(download).to_be_enabled()
        await expect(download).to_have_text("Download")
        # The larger model is listed, greyed, with the plain sentence and no way to start it.
        other = page.locator(".get-model-other")
        await expect(other).to_have_count(1)
        await expect(other).to_contain_text("Giant fixture")
        await expect(other).to_contain_text("Add a computer to use it.")
        assert await other.locator("button").count() == 0
        assert not any(models.iterdir()) and not any("/resolve/" in entry[0] for entry in hub.log)

        # Download, progress, cancel.
        hub.release.clear()
        await download.click()
        await expect(page.locator("#get-model-progress")).to_be_visible()
        await expect(status).to_contain_text("%", timeout=15000)
        await expect(cancel).to_be_visible()
        await expect(download).to_be_hidden()
        await cancel.click()
        await expect(status).to_contain_text("Download cancelled", timeout=15000)
        await expect(download).to_be_visible()
        assert not any(models.iterdir())
        hub.release.set()

        # Download to the end: the card says what happens next and loads nothing.
        await download.click()
        await expect(status).to_contain_text("Downloaded and checked. It now appears under Prepare models.", timeout=30000)
        assert tree(models / "tiny") == standard_files()
        await expect(page.locator("#model")).to_have_value("")
        await page.locator("#get-model-open").click()
        await expect(page.locator("#workspace")).to_be_visible()
        await expect(card).to_be_hidden()

        # Only a model that is too large: greyed, and the Download button cannot be pressed.
        entries = json.loads(catalogue.read_text())
        entries["models"] = [entry for entry in entries["models"] if entry["id"] == "giant"]
        catalogue.write_text(json.dumps(entries))
        for path in sorted(models.rglob("*"), reverse=True):
            path.rmdir() if path.is_dir() else path.unlink()
        await page.goto("about:blank")  # a link that differs only in its fragment would not load the page again
        await page.goto(link)
        await expect(card).to_be_visible()
        await expect(status).to_contain_text("of memory across your computers. Add a computer to use it.")
        await expect(download).to_be_disabled()

        # No downloadable model at all.
        entries["models"] = []
        catalogue.write_text(json.dumps(entries))
        await page.goto("about:blank")
        await page.goto(link)
        await expect(card).to_contain_text("No model is available for download yet.")
        await expect(download).to_be_hidden()

        # Signing out leaves nothing of the card behind.
        await page.locator("#sign-out").click()
        await expect(card).to_be_hidden()
        assert problems == [], problems
        assert outside == [], outside
        await browser.close()


def main():
    hub = Hub()
    threading.Thread(target=hub.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix="lmb-web-download-") as temporary:
        home = Path(temporary)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
               "LUMABRI_KNOWN_HOSTS": temporary + "/known", "LUMABRI_ADVERTISE": "127.0.0.1",
               "LUMABRI_HOME_PORT_BASE": str(12000 + (port % 200) * 160),
               "LUMABRI_NO_DISK_PROBE": "1", "LUMABRI_RAM_RESERVE_MB": "256"}
        for name in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND"):
            env.pop(name, None)

        def run(*args):
            return subprocess.run([str(ROOT / "lumabri"), *args], env=env, text=True, stdin=subprocess.DEVNULL,
                                  capture_output=True, timeout=90)

        hub.add("owner/tiny", Repo(standard_files(), stall=True))
        try:
            first = run("start", "--no-browser", "--port", str(port))
            assert first.returncode == 0, (first.stdout, first.stderr)
            link = next(line.strip() for line in first.stdout.splitlines() if "#token=" in line)
            (home / ".lumabri/download-catalogue.json").write_text(json.dumps({
                "schema": 1, "endpoint": hub.base, "models": [
                    {"id": "tiny", "family": "fixture", "name": "Tiny fixture", "repo": "owner/tiny", "disk_gb": 0.001,
                     "ram_min_gb": 0.001, "ram_good_gb": 0.002, "summary": "test files", "licence": "Test licence"},
                    {"id": "giant", "family": "fixture", "name": "Giant fixture", "repo": "owner/tiny",
                     "disk_gb": 0.001, "resident_gb": 900000, "ram_min_gb": 1, "ram_good_gb": 2}]}))
            asyncio.run(scenario(link, hub, home))
        finally:
            hub.release.set()
            stopped = run("service", "stop")
            hub.shutdown()
            hub.server_close()
        assert stopped.returncode == 0, stopped.stderr
        state = home / ".lumabri/downloads"
        assert not state.exists() or lock_free(state)
    print("WEB DOWNLOAD: PASS (fits, progress, cancel, completion, too large, none available, sign-out)")


if __name__ == "__main__":
    main()
