"""Standing consent: a donor that accepts its own household's loads without a click.

Real loads of one very small generated checkpoint, each household in its own
home directory, loopback only:

* a fresh `lumabri start` says that this computer accepts its household's
  requests automatically and how to turn that off; a preparation then
  completes and answers with no approval, and the acceptance is recorded with
  who asked, which model and how much memory, in the page API, the command
  line and the donor log;
* nobody else can flip the switch: not an anonymous caller, not a sign-in
  without the management permission, and not a manager who arrives through
  the HTTPS proxy from another computer;
* loads are accepted automatically only while they fit the memory this
  computer shares: the one that no longer fits is refused with consent on and
  nothing more is loaded. Through the page API that refusal comes from the
  household planner, which reads the donor's own budget; the donor's check
  behind it is the unchanged code path of a manual acceptance;
* switched off from the page, the next request waits for a person, and the
  model that is already loaded stays loaded;
* an installation that already had a household and no record stays off.
"""
import http.client
import json
import os
from pathlib import Path
import secrets
import socket
import stat
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from residency_auto_test import APPROVAL, Household, checkpoint, until

CONSENT = "/api/v1/donor/consent"
PREPARATION = "/api/v1/preparation"


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=30)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        self.sock.connect(self.path)


class House(Household):
    def __init__(self, model, share=None):
        super().__init__(model, 1 << 20, None)
        self.share = share

    def begin(self):
        arguments = ["start", "--no-browser", "--port", str(self.port)]
        if self.share:
            arguments += ["--ram-gb", str(self.share)]
        answer = self.run(*arguments)
        assert answer.returncode == 0, (answer.stdout, answer.stderr)
        links = [line.strip() for line in answer.stdout.splitlines() if "#token=" in line]
        if links:
            self.token = links[0].split("#token=", 1)[1]
        link = self.home / ".lumabri/models/reduced"
        if not link.exists():
            link.symlink_to(self.model, target_is_directory=True)
        return answer.stdout

    def raw(self, path, body=None, token=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        headers = {"Authorization": "Bearer " + token} if token else {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection.request("POST" if body is not None else "GET", path,
                           None if body is None else json.dumps(body), headers)
        response = connection.getresponse(); data = response.read(); connection.close()
        return response.status, json.loads(data)

    def status(self):
        answer = self.run("consent", "status", "--json")
        assert answer.returncode == 0, answer.stderr
        return json.loads(answer.stdout)

    def intent(self, context=128):
        nodes = until(lambda: [n["id"] for n in self.json("/api/v1/workspace")["nodes"]
                               if n["workload"] is not None and n["runtime_threads"]],
                      "this computer did not report its capacity")
        return {"models": ["reduced"], "nodes": nodes, "context": context, "sessions": 1, "max_new": 8}

    def request(self, intent, review):
        operation = secrets.token_hex(32)
        status, answer = self.raw(PREPARATION, {"action": "start", "operation": operation, "review": review, **intent},
                                  self.token)
        return operation, status, answer

    def finished(self, operation, seconds=240):
        state = lambda: self.json(PREPARATION, {"action": "status", "operation": operation})
        return until(lambda: (lambda s: s if s["state"] in ("done", "failed") else None)(state()),
                     "the preparation did not end", seconds)


def page_switch(house, entry):
    """The switch on this computer's own page: shown on, turned off, on again and off. False without a browser."""
    try:
        from playwright.sync_api import sync_playwright, expect
    except ImportError:
        return False
    with sync_playwright() as playwright:
        chromium = playwright.chromium.launch(headless=True)
        page = chromium.new_page(viewport={"width": 1280, "height": 860})
        errors = []; page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(f"http://127.0.0.1:{house.port}/#token={house.token}")
        expect(page.locator("#workspace-open")).to_be_visible(timeout=15000)
        expect(page.locator("#consent")).to_be_hidden()
        page.locator("#workspace-open").click()
        switch = page.locator("#consent-switch")
        expect(switch).to_be_checked(timeout=15000)
        expect(page.locator("#consent-note")).to_contain_text("without a click")
        expect(page.locator("#consent-accepted li")).to_have_count(1)
        expect(page.locator("#consent-accepted li")).to_contain_text(entry["requester"][:12])
        expect(page.locator("#consent-accepted li")).to_contain_text("reduced")
        switch.uncheck()
        expect(page.locator("#consent-status")).to_contain_text("next request will wait", timeout=15000)
        assert house.status()["enabled"] is False and house.status()["origin"] == "owner"
        # Turning it on asks once; refusing the question changes nothing.
        page.once("dialog", lambda dialog: dialog.dismiss())
        switch.click()
        expect(switch).not_to_be_checked()
        assert house.status()["enabled"] is False
        page.once("dialog", lambda dialog: dialog.accept())
        switch.check()
        expect(page.locator("#consent-status")).to_contain_text("accepted automatically", timeout=15000)
        assert house.status()["enabled"] is True
        switch.uncheck()
        expect(page.locator("#consent-status")).to_contain_text("next request will wait", timeout=15000)
        expect(page.locator("#consent-note")).to_contain_text("waits for a person")
        assert not errors, errors
        chromium.close()
    assert house.status()["enabled"] is False
    return True


def fresh_start_needs_no_click(model):
    house = House(model); by_page = False
    try:
        output = house.begin()
        assert "this computer accepts its household's requests to load a model automatically" in output and \
            "lumabri consent off" in output, output
        record = house.home / ".lumabri/service/standing-consent"
        assert stat.S_IMODE(record.stat().st_mode) == 0o600, "the consent record is not owner-only"
        state = house.status()
        assert state["enabled"] and state["chosen"] and state["origin"] == "start" and state["accepted"] == [], state
        page = house.json(CONSENT)
        assert page["enabled"] and page["scope"] == "this_computer" and page["accepted"] == [], page
        assert house.json(APPROVAL)["automatic_consent"] is True

        # ---- a load with no approval at all -------------------------------
        intent = house.intent()
        preview = house.json(PREPARATION, {"action": "preview", **intent})
        reserved = preview["plan"]["models"][0]["slices"][0]["reserved_bytes"]
        operation, status, answer = house.request(intent, preview["review"])
        assert status in (200, 202), answer
        final = house.finished(operation)
        assert final["state"] == "done" and final["chat_access"] == "granted", final
        allocation = final["allocations"][0]["id"]
        house.chat(allocation)
        logs = house.logs()
        assert "[donor-approval] action=accept" not in logs, "a decision was sent for an automatic acceptance"
        assert f"[standing-consent] accepted allocation={allocation}" in logs, logs[-3000:]
        (entry,) = house.json(CONSENT)["accepted"]
        assert entry["allocation"] == allocation and entry["ram_bytes"] == reserved and \
            entry["model"].endswith("-reduced") and len(entry["requester"]) == 64 and \
            abs(entry["time"] - time.time()) < 600, entry
        assert house.status()["accepted"] == [entry]
        listed = house.run("consent", "status").stdout
        assert "Automatic acceptance: ON" in listed and entry["requester"][:12] in listed and "reduced" in listed, listed
        shown = house.json(APPROVAL)
        assert shown["request"]["allocation"] == allocation and not shown["pending"], shown
        donor = house.donor()
        assert donor["model_count"] == 1 and donor["reserved_total_bytes"] == reserved, donor

        # ---- only the person at this computer holds the switch -------------
        off = {"enabled": False}
        assert house.raw(CONSENT)[0] == 401 and house.raw(CONSENT, off)[0] == 401
        reader = house.run("api", "user-add", "reader")
        assert reader.returncode == 0 and house.run("api", "operator-grant", "reader").returncode == 0, reader.stderr
        reader = reader.stdout.strip()
        for body in (None, off):
            assert house.raw(CONSENT, body, reader) == (403, {"error": "management_permission_required"})
        for bad in ({}, {"enabled": "false"}, {"enabled": 0}, {"enabled": False, "household": "elsewhere"}):
            assert house.raw(CONSENT, bad, house.token) == (400, {"error": "invalid_donor_consent_request"}), bad
        assert house.status()["enabled"] and house.status()["origin"] == "start"
        print("fresh start: loaded and answered with no approval; recorded; outsiders refused")

        # ---- off from the page: the next request waits, nothing unloads ----
        by_page = page_switch(house, entry)
        changed = house.json(CONSENT, off)
        assert changed["enabled"] is False and changed["origin"] == "owner" and changed["accepted"] == [entry], changed
        assert house.status()["enabled"] is False and house.json(APPROVAL)["automatic_consent"] is False
        assert house.donor()["model_count"] == 1, "turning consent off unloaded a model"
        house.chat(allocation)
        preview = house.json(PREPARATION, {"action": "preview", **intent})
        second, status, answer = house.request(intent, preview["review"])
        assert status in (200, 202), answer
        waiting = until(lambda: (lambda a: a if a["pending"] else None)(house.json(APPROVAL)),
                        "with consent off the request did not wait for a person", 120)
        time.sleep(4)
        still = house.json(APPROVAL)
        assert still["pending"] and still["request"]["allocation"] == waiting["request"]["allocation"] != allocation, still
        # A waiting request is counted as a model, and holds no memory.
        assert house.donor()["reserved_total_bytes"] == reserved and house.json(CONSENT)["accepted"] == [entry]
        fence = {"instance": still["instance"], "allocation": still["request"]["allocation"], "revision": still["revision"]}
        assert house.json(APPROVAL, {"action": "decline", **fence})["result"] == "declined"
        assert house.finished(second)["state"] == "failed"
        assert house.donor()["model_count"] == 1 and house.json(CONSENT)["accepted"] == [entry]
        print("consent off: the next request waited for a person; the loaded model stayed")

        # ---- a manager from another computer, through the HTTPS proxy ------
        assert house.json(CONSENT, {"enabled": True})["enabled"] is True
        assert house.run("api", "stop").returncode == 0
        proxied = house.run("api", "start", "--https-origin", "https://lumabri.invalid")
        assert proxied.returncode == 0, proxied.stderr
        upstream = house.home / ".lumabri/api-access/proxy.sock"
        assert upstream.is_socket(), "the proxied gateway has no private upstream socket"

        def through_proxy(path, body=None):
            connection = UnixConnection(str(upstream))
            headers = {"Authorization": "Bearer " + house.token, "Host": "lumabri.invalid"}
            if body is not None:
                headers.update({"Content-Type": "application/json", "Origin": "https://lumabri.invalid"})
            connection.request("POST" if body is not None else "GET", path,
                               None if body is None else json.dumps(body), headers)
            response = connection.getresponse(); data = response.read(); connection.close()
            return response.status, json.loads(data)

        # The same manager is served there, only not for this computer's consent.
        assert through_proxy("/api/v1/session") == (200, {"schema": 1, "operator": True, "management": True})
        for body in (None, off, {"enabled": True}):
            assert through_proxy(CONSENT, body) == (403, {"error": "donor_approval_local_page_only"}), body
        assert house.status()["enabled"] is True, "a proxied manager changed this computer's consent"
        house.passed = True
    finally:
        house.stop()
    print("proxied manager: refused; the switch stayed as its owner left it")
    print("page switch: shown, off, on after confirmation, off" if by_page else
          "page switch: NOT exercised, Playwright is not installed; the route was exercised directly")


def over_the_share_is_refused(model):
    """Loads reviewed while each still fits, started one after another until the share is full."""
    house = House(model, share=1)
    try:
        assert "automatically" in house.begin()
        intent = house.intent()
        reviews = [house.json(PREPARATION, {"action": "preview", **intent}) for _ in range(6)]
        reserved = reviews[0]["plan"]["models"][0]["slices"][0]["reserved_bytes"]
        assert 6 * reserved > (1 << 30) + reserved and reserved <= 1 << 30, reserved
        loaded = []; where = None
        for review in reviews:
            operation, status, answer = house.request(intent, review["review"])
            if status not in (200, 202):
                assert status in (400, 409, 422, 503), (status, answer)
                where = f"the household planner ({answer.get('error')})"
                break
            final = house.finished(operation)
            if final["state"] == "failed":
                where = "this computer's own admission check" if "[standing-consent] refused" in house.logs() \
                    else "the household planner"
                break
            loaded.append(final["allocations"][0]["id"])
        assert where and loaded, "six loads fitted a 1 GB share"
        time.sleep(2)
        donor = house.donor()
        assert donor["reserved_total_bytes"] == len(loaded) * reserved <= 1 << 30, donor
        assert sorted(entry["allocation"] for entry in house.json(CONSENT)["accepted"]) == sorted(loaded)
        assert house.status()["enabled"] is True
        house.passed = True
        print(f"over the share: {len(loaded)} loads of {reserved} bytes accepted automatically, the next refused by "
              f"{where} with consent on; nothing more was loaded")
    finally:
        house.stop()


def existing_installation_stays_off(model):
    house = House(model)
    try:
        house.begin()
        for _ in range(4):
            if house.run("service", "stop").returncode == 0:
                break
        # An installation from before this setting: a household, and no record.
        (house.home / ".lumabri/service/standing-consent").unlink()
        output = house.begin()
        assert "each request to load a model here waits for your approval" in output and \
            "lumabri consent on" in output and "automatically, " not in output.replace("automatically: lumabri", ""), output
        state = house.status()
        assert state["enabled"] is False and state["chosen"] is False and state["origin"] == "none", state
        assert not (house.home / ".lumabri/service/standing-consent").exists(), "start wrote consent for an existing household"
        intent = house.intent()
        preview = house.json(PREPARATION, {"action": "preview", **intent})
        operation, status, answer = house.request(intent, preview["review"])
        assert status in (200, 202), answer
        until(lambda: house.json(APPROVAL)["pending"], "an existing installation did not ask its owner", 120)
        time.sleep(3)
        assert house.json(APPROVAL)["pending"] and house.donor()["reserved_total_bytes"] == 0
        # The owner's command turns it on, and the waiting request is then accepted.
        on = house.run("consent", "on")
        assert on.returncode == 0 and "Automatic acceptance: ON" in on.stdout, (on.stdout, on.stderr)
        final = house.finished(operation)
        assert final["state"] == "done", final
        assert house.json(CONSENT)["origin"] == "owner" and len(house.json(CONSENT)["accepted"]) == 1
        off = house.run("consent", "off")
        assert off.returncode == 0 and "Automatic acceptance: OFF" in off.stdout and house.donor()["model_count"] == 1
        house.passed = True
    finally:
        house.stop()
    print("existing installation: off until its owner turns it on")


def main():
    if sys.platform != "linux":
        print("standing consent: exercised on Linux only; not claimed on this kernel")
        return
    with tempfile.TemporaryDirectory(prefix="lmb-consent-model-") as folder:
        model = checkpoint(Path(folder) / "reduced")
        fresh_start_needs_no_click(model)
        over_the_share_is_refused(model)
        existing_installation_stays_off(model)
    print("standing consent: PASS")


if __name__ == "__main__":
    main()
