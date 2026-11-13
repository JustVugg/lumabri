"""Add a computer from the page: two households on loopback, two real pages.

Each household is started with `lumabri start` in its own HOME and port range.
In Chromium the first page shows a code under Computers > Add a computer; the
second page types it under "I have a code". A wrong code changes nothing there
and is counted here; the right code moves the second computer into the first
household, restarts its services and leaves the same sign-in working.
Requires Playwright Chromium; never included in native runtime dependencies.
No model is loaded.
"""
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[2]
CODE = re.compile(r"^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$")


def free_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def main():
    with tempfile.TemporaryDirectory(prefix="lmb-pair-web-") as temporary:
        root = Path(temporary)
        bases = {"owner": 36000 + (os.getpid() % 40) * 32, "second": 36016 + (os.getpid() % 40) * 32}
        pages = {name: free_port() for name in bases}

        def environment(name):
            home = root / name
            home.mkdir(exist_ok=True)
            env = {**os.environ, "HOME": str(home), "LUMABRI_PEER_KEY": str(home / "peer.key"),
                   "LUMABRI_KNOWN_HOSTS": str(home / "known"), "LUMABRI_ADVERTISE": "127.0.0.1",
                   "LUMABRI_HOME_PORT_BASE": str(bases[name]), "LUMABRI_NO_DISK_PROBE": "1",
                   "LUMABRI_RAM_RESERVE_MB": "256"}
            for variable in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND", "LUMABRI_PEER_PINS",
                             "LUMABRI_REQUIRE_PIN", "LUMABRI_PAIR_LIFETIME_S"):
                env.pop(variable, None)
            return env

        def run(name, *args):
            return subprocess.run([str(ROOT / "lumabri"), *args], env=environment(name), text=True,
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=120)

        def settings(name):
            text = (root / name / ".lumabri/home.conf").read_text()
            return dict(line.split("=", 1) for line in text.splitlines())

        def call(name, method, path, token=None, body=None):
            conn = http.client.HTTPConnection("127.0.0.1", pages[name], timeout=30)
            headers = {"Authorization": "Bearer " + token} if token else {}
            if body is not None:
                headers["Content-Type"] = "application/json"
            conn.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
            response = conn.getresponse(); data = response.read(); conn.close()
            return response.status, json.loads(data)

        links, playwright, chromium = {}, None, None
        try:
            for name in bases:
                started = run(name, "start", "--no-browser", "--port", str(pages[name]), "--ram-gb", "1")
                assert started.returncode == 0, (name, started.stdout, started.stderr)
                links[name] = next(line.strip() for line in started.stdout.splitlines() if "#token=" in line)
            tokens = {name: link.split("#token=")[1] for name, link in links.items()}
            owner_address = f"127.0.0.1:{bases['owner']}"
            before = settings("second")
            assert settings("owner")["tracker"] == owner_address and before["owner"] == "1"
            household_key = settings("owner")["token"]

            # The routes need a sign-in; reading or creating a code without one is refused.
            assert call("owner", "GET", "/api/v1/household")[0] == 401
            assert call("owner", "POST", "/api/v1/household/pairing", body={"action": "create"})[0] == 401
            assert call("second", "POST", "/api/v1/household/join", body={"code": "7K3M-9QXT"})[0] == 401
            assert call("owner", "GET", "/api/v1/household", tokens["owner"]) == \
                (200, {"schema": 1, "tracker": owner_address, "owner": True})
            assert call("owner", "POST", "/api/v1/household/pairing", tokens["owner"], {"action": "open"})[0] == 400
            assert call("owner", "GET", "/api/v1/household/pairing/" + "0" * 32, tokens["owner"])[1]["state"] == "unknown"
            assert call("second", "POST", "/api/v1/household/join", tokens["second"], {"code": "nope"}) == \
                (400, {"error": "invalid_pairing_code"})
            assert call("second", "POST", "/api/v1/household/join", tokens["second"],
                        {"code": "7K3M-9QXT", "address": f"127.0.0.1:{bases['second']}"}) == \
                (409, {"error": "already_in_this_household"})

            playwright = sync_playwright().start()
            chromium = playwright.chromium.launch(headless=True)
            problems, outside = [], []

            def open_page(name):
                page = chromium.new_page(viewport={"width": 1280, "height": 900})
                page.on("pageerror", lambda error: problems.append(f"{name}: {error}"))
                page.on("request", lambda request: outside.append(request.url)
                        if not request.url.startswith(links[name].split("#")[0]) else None)
                page.goto(links[name])
                expect(page.locator("#app")).to_have_class(re.compile(r"\bconnected\b"))
                assert "#token" not in page.url
                page.locator("#devices-button").click()
                expect(page.locator("#rooms")).to_be_visible()
                expect(page.locator("#room-intro")).to_contain_text("owns the household at")
                return page

            owner, second = open_page("owner"), open_page("second")
            shown, status = owner.locator("#room-code-shown"), owner.locator("#room-status")
            expect(shown).to_be_hidden()
            owner.locator("#room-create").click()
            expect(shown).to_be_visible()
            code = shown.inner_text().strip()
            assert CODE.match(code), "the page did not show a pairing code"
            expect(status).to_contain_text(owner_address)
            expect(owner.locator("#room-cancel")).to_be_visible()

            # The second computer: nothing can be sent before the code and the consent are there.
            second.locator("#mode-join").click()
            join, result = second.locator("#room-join"), second.locator("#room-status")
            expect(second.locator("#room-create")).to_be_hidden()
            expect(join).to_be_disabled()
            wrong = code[:-1] + ("7" if code[-1] != "7" else "8")
            second.locator("#room-code").fill(wrong)
            second.locator("#room-address").fill(owner_address)
            expect(join).to_be_disabled()
            second.locator("#room-consent").check()
            join.click()
            expect(result).to_contain_text("Pairing failed", timeout=20000)
            assert settings("second") == before, "a wrong code changed the settings"
            assert not (root / "second/known").exists() or owner_address not in (root / "second/known").read_text()
            expect(status).to_contain_text("1 of 5 wrong attempts", timeout=10000)
            assert call("second", "GET", "/api/v1/household", tokens["second"])[1]["owner"] is True

            # The right code: this computer joins, restarts, and the same sign-in still works.
            second.locator("#room-code").fill(code.lower())
            second.locator("#room-consent").check()
            join.click()
            expect(result).to_contain_text("restarting", timeout=20000)
            expect(result).to_contain_text(f"now shares with the household at {owner_address}", timeout=150000)
            joined = settings("second")
            assert joined["tracker"] == owner_address and joined["token"] == household_key and joined["owner"] == "0"
            assert joined["ram"] == "4", joined["ram"]       # the pledge chosen on the page
            expect(second.locator("#room-intro")).to_contain_text(f"is in the household at {owner_address}")
            second.locator("#mode-create").click()
            expect(second.locator("#room-intro")).to_contain_text("Codes are created on the computer that owns it")
            expect(second.locator("#room-create")).to_be_disabled()
            expect(status).to_contain_text("A computer joined from", timeout=10000)
            expect(shown).to_be_hidden()
            assert call("second", "GET", "/api/v1/household", tokens["second"]) == \
                (200, {"schema": 1, "tracker": owner_address, "owner": False})
            # A member cannot create codes, and the used code is worth nothing.
            assert call("second", "POST", "/api/v1/household/pairing", tokens["second"], {"action": "create"}) == \
                (409, {"error": "pairing_owner_only"})
            services = json.loads(run("second", "service", "status", "--json").stdout)["services"]
            live = {s["role"] for s in services if s["live"]}
            assert {"donor", "api"} <= live and "tracker" not in live, live

            # A new code can be withdrawn from the page.
            owner.locator("#room-create").click()
            expect(shown).to_be_visible()
            owner.locator("#room-cancel").click()
            expect(status).to_contain_text("withdrawn")
            expect(shown).to_be_hidden()

            assert not problems, problems
            assert not outside, outside
            # Neither the code nor the household key reached a log or any other file.
            for path in root.rglob("*"):
                if path.is_file():
                    data = path.read_bytes()
                    assert code.encode() not in data and code.replace("-", "").encode() not in data, path.name
                    assert household_key.encode() not in data or path.name == "home.conf", path.name
        finally:
            if chromium:
                chromium.close()
            if playwright:
                playwright.stop()
            stopped = [run(name, "service", "stop") for name in bases]
        assert all(s.returncode == 0 for s in stopped), [s.stderr for s in stopped]
    print("pairing from the page: PASS (code shown and counted on one page, typed on the other; "
          "the second computer joined and restarted)")


if __name__ == "__main__":
    main()
