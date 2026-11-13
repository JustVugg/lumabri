"""Approve a load from the page's API, on the computer that shares the memory.

`lumabri start` leaves a headless donor with nobody at its "Share resources"
screen. This gate prepares a real reduced checkpoint through the same API the
page uses, and decides the donor's request through /api/v1/donor/approval:
declined once (nothing loads), allowed once through the optional one-hour
repeat permission (the model answers), accepted once, then the permission is
revoked. The sign-in that prepared a model chats with it at once, with no
grant command; nobody else gains access, and a full grant list is reported.
Loopback only; it is not a LAN test and not a measurement.
"""
import argparse
import base64
import http.client
import json
import os
import resource
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
PATH = "/api/v1/donor/approval"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, default=ROOT / "tiny_olmoe",
                        help="one small planner-supported checkpoint directory (make fixture)")
    parser.add_argument("--screenshots", type=Path,
                        help="also drive the page in Chromium and save the approval prompt here")
    parser.add_argument("--browser", action="store_true", default=os.environ.get("LUMABRI_TEST_BROWSER") == "1",
                        help="prepare and approve the second load by clicks in Chromium, and require the "
                             "chat model picker to gain the model without a reload")
    args = parser.parse_args()
    model = args.model.resolve()
    assert (model / "config.json").is_file(), f"{model} is not a checkpoint; run make fixture"

    temporary = tempfile.mkdtemp(prefix="lmb-approve-")
    home = Path(temporary)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
    env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
           "LUMABRI_KNOWN_HOSTS": temporary + "/known", "LUMABRI_ADVERTISE": "127.0.0.1",
           "LUMABRI_HOME_PORT_BASE": str(12000 + (port % 200) * 160),
           "LUMABRI_NO_DISK_PROBE": "1", "LUMABRI_RAM_RESERVE_MB": "256"}
    for name in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND", "LUMABRI_RESIDENT_REQUIRED"):
        env.pop(name, None)
    # The household default locks resident weights. Where the OS allowance
    # is smaller than this checkpoint, the gate still exercises approval and
    # inference under the explicit diagnostic policy, and says so.
    soft = resource.getrlimit(resource.RLIMIT_MEMLOCK)[0]
    weights = sum(p.stat().st_size for p in model.rglob("*") if p.is_file())
    if "LUMABRI_RESIDENCY_POLICY" not in env and soft != resource.RLIM_INFINITY and soft < 2 * weights:
        env["LUMABRI_RESIDENCY_POLICY"] = "observe"
        print(f"memlock allowance {soft} bytes < 2 x {weights}: using LUMABRI_RESIDENCY_POLICY=observe", flush=True)

    def run(*arguments, timeout=90):
        return subprocess.run([str(ROOT / "lumabri"), *arguments], env=env, text=True,
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)

    def donor():
        answer = run("service", "status", "--json")
        assert answer.returncode == 0, answer.stderr
        return {s["role"]: s for s in json.loads(answer.stdout)["services"]}["donor"]

    def call(path, token=None, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        headers = {"Authorization": "Bearer " + token} if token else {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection.request("POST" if body is not None else "GET", path,
                           None if body is None else json.dumps(body), headers)
        response = connection.getresponse(); data = response.read(); connection.close()
        return response.status, json.loads(data)

    def until(condition, message, seconds=60):
        deadline = time.monotonic() + seconds
        while True:
            value = condition()
            if value:
                return value
            assert time.monotonic() < deadline, message
            time.sleep(.2)

    def start():
        answer = run("start", "--no-browser", "--port", str(port))
        assert answer.returncode == 0, (answer.stdout, answer.stderr)
        links = [line.strip() for line in answer.stdout.splitlines() if "#token=" in line]
        assert len(links) == 1, answer.stdout
        return links[0].split("#token=", 1)[1]

    stopped = None; passed = False
    try:
        token = start()
        # The owner's models folder, as the page's catalogue reads it.
        (home / ".lumabri/models/reduced").symlink_to(model, target_is_directory=True)

        # Reading or deciding a request needs a credential with management.
        fence = {"instance": "1" * 64, "allocation": "2" * 64, "revision": "1"}
        assert call(PATH)[0] == 401
        assert call(PATH, body={"action": "accept", **fence})[0] == 401
        reader = run("api", "user-add", "reader")
        assert reader.returncode == 0 and run("api", "operator-grant", "reader").returncode == 0, reader.stderr
        reader = reader.stdout.strip()
        assert call("/api/v1/workspace", reader)[0] == 200
        for action in (None, "accept", "decline", "allow_repeat", "revoke_repeat"):
            status, answer = call(PATH, reader, None if action is None else {"action": action, **fence})
            assert status == 403 and answer == {"error": "management_permission_required"}, (action, status, answer)

        status, idle = call(PATH, token)
        assert status == 200 and idle["sharing"] and not idle["pending"] and idle["request"] is None, idle
        assert idle["scope"] == "this_computer" and idle["automatic_consent"] is False
        assert idle["instance"] == donor()["instance"]
        assert call(PATH, token, {"action": "accept", **fence})[0] == 409
        assert call(PATH, token, {"action": "approve", **fence})[0] == 400
        assert call(PATH, token, {"action": "accept", **fence, "computer": "other"})[0] == 400

        status, catalogue = call("/api/v1/catalogue", token)
        assert status == 200 and [m["name"] for m in catalogue["models"] if m["source_ready"]] == ["reduced"], catalogue
        nodes = until(lambda: [n["id"] for n in call("/api/v1/workspace", token)[1]["nodes"]
                               if n["workload"] is not None and n["runtime_threads"]],
                      "this computer did not report its capacity")
        assert len(nodes) == 1, nodes
        intent = {"models": ["reduced"], "nodes": nodes, "context": 128, "sessions": 1, "max_new": 8}

        def prepare():
            status, preview = call("/api/v1/preparation", token, {"action": "preview", **intent})
            assert status == 200 and preview["plan"]["requires_approval"], preview
            operation = secrets.token_hex(32)
            status, started = call("/api/v1/preparation", token,
                                   {"action": "start", "operation": operation, "review": preview["review"], **intent})
            assert status == 202 and started["requires_approval"], started
            return operation, preview["plan"]

        def operation_state(operation):
            status, answer = call("/api/v1/preparation", token, {"action": "status", "operation": operation})
            assert status == 200, answer
            return answer

        def pending(plan, known=()):
            def waiting():
                answer = call(PATH, token)[1]
                return answer if answer["pending"] and answer["request"]["allocation"] not in known else None
            answer = until(waiting, "the donor's request did not reach the page API", 120)
            request = answer["request"]; (slice_,) = plan["models"][0]["slices"]
            assert answer["phase"] == "pending" and answer["state"] == "Waiting for your approval", answer
            assert answer["decision_seconds"] == 120 and answer["instance"] == donor()["instance"]
            assert request["name"] == "reduced" and request["model"].endswith("-reduced") and request["adapter"] == "olmoe" and request["runs_edge"], request
            assert (request["begin"], request["end"]) == (slice_["begin"], slice_["end"]) and \
                request["end"] == request["layers"], (request, slice_)
            assert request["ram_bytes"] == slice_["reserved_bytes"] and len(request["requester"]) == 64, request
            assert (request["context"], request["sessions"], request["max_new"]) == (128, 1, 8), request
            assert request["repeat_supported"] is True and not request["expert_accelerator"], request
            state = donor()
            assert state["phase"] == 1 and state["segment_pid"] == 0 and state["reserved_bytes"] == 0, state
            return {"instance": answer["instance"], "allocation": request["allocation"], "revision": answer["revision"]}

        def finished(operation):
            return until(lambda: (lambda s: s if s["state"] in ("done", "failed") else None)(operation_state(operation)),
                         "approved preparation did not finish", 240)

        def requester(fence):
            answer = call(PATH, token)[1]
            assert answer["request"]["allocation"] == fence["allocation"]
            return answer["request"]["requester"]

        def policies():
            listed = run("donor-policy", "list")
            assert listed.returncode == 0, listed.stderr
            return json.loads(listed.stdout)["policies"]

        # ---- decline: the default, and nothing is loaded -----------------
        first, plan = prepare()
        shown = pending(plan)
        for stale in ({**shown, "revision": str(int(shown["revision"]) + 1)}, {**shown, "allocation": "2" * 64},
                      {**shown, "instance": "1" * 64}):
            assert call(PATH, token, {"action": "accept", **stale})[0] == 409, stale
        assert call(PATH, token)[1]["pending"], "a stale decision changed the request"
        if args.screenshots:
            # The same decision, taken by a person's click in the page.
            screenshots(args.screenshots, port, token, shown)
        else:
            status, declined = call(PATH, token, {"action": "decline", **shown})
            assert status == 200 and declined["result"] == "declined" and declined["allocation"] == shown["allocation"], declined
        assert call(PATH, token, {"action": "accept", **shown})[0] == 409, "a declined request was accepted afterwards"
        until(lambda: operation_state(first)["state"] == "failed", "declined preparation did not end", 60)
        state = donor()
        assert state["live"] and state["model_count"] == 0 and state["reserved_total_bytes"] == 0 and \
            state["segment_pid"] == 0 and state["host_pid"] == 0, state
        assert not call(PATH, token)[1]["pending"]
        assert call("/api/v1/models", token)[1] == {"schema": 1, "models": []}
        assert policies() == []

        # ---- allow repeat loads for one hour: the saved permission, not a
        # click, authorizes this request, and the model answers ------------
        browser = None
        if args.browser:
            # A person who only uses the page: prepare there, leave Computers
            # & models, approve in the prompt, and find the model in the picker.
            plan = call("/api/v1/preparation", token, {"action": "preview", **intent})[1]["plan"]
            browser = page_prepare(port, token, nodes[0])
            second = browser["operation"]
        else:
            second, plan = prepare()
        shown2 = pending(plan, known={shown["allocation"]})
        assert shown2["instance"] == shown["instance"]
        if browser:
            page_approve(browser, shown2["allocation"])
        else:
            status, repeated = call(PATH, token, {"action": "allow_repeat", **shown2})
            assert status == 200 and repeated["result"] == "accepted" and "owner policy" in repeated["detail"], repeated
        (policy,) = policies()
        assert policy["enabled"] and policy["max_allocations"] == 1 and policy["expires"] - policy["issued"] == 3600, policy
        assert policy["donor_instance"] == shown2["instance"] and policy["requester"] == requester(shown2), policy
        assert call(PATH, token, {"action": "accept", **shown2})[0] == 409, "one approval was usable twice"
        done = finished(second)
        assert done["state"] == "done" and [a["state"] for a in done["allocations"]] == ["ready_observed"], done
        allocation = done["allocations"][0]["id"]
        after = call(PATH, token)[1]
        assert not after["pending"] and after["phase"] == "ready" and after["request"]["allocation"] == shown2["allocation"], after
        assert after["request"]["repeat_supported"] is False
        reserved = plan["models"][0]["slices"][0]["reserved_bytes"]
        state = donor()
        assert state["model_count"] == 1 and state["reserved_total_bytes"] == reserved, state
        # The sign-in that prepared the model holds its exact inference
        # grant as soon as it is ready: no grant command, no second start.
        assert done["chat_access"] == "granted", done
        (granted,) = call("/api/v1/models", token)[1]["models"]
        assert granted["id"] == allocation and granted["name"].endswith("-reduced"), granted
        assert generate(port, token, allocation)
        # Nobody else gains anything: not a reader, and not another manager
        # who observes the same operation.
        other = run("api", "user-add", "other")
        assert other.returncode == 0 and run("api", "manage-grant", "other").returncode == 0, other.stderr
        other = other.stdout.strip()
        status, watched = call("/api/v1/preparation", other, {"action": "status", "operation": second})
        assert status == 200 and watched["state"] == "done" and \
            watched["chat_access"] == "not_requested_by_this_sign_in", watched
        for outsider in (reader, other):
            assert call("/api/v1/models", outsider)[1]["models"] == []
            assert chat_status(port, outsider, allocation) == (403, {"error": "model_not_authorized"})
        # The declined operation granted nothing, and a later start adds nothing.
        assert operation_state(first)["chat_access"] == "none_ready"
        again = run("start", "--no-browser", "--port", str(port))
        assert again.returncode == 0 and "added to the local sign-in" not in again.stdout, again.stdout
        assert [m["id"] for m in call("/api/v1/models", token)[1]["models"]] == [allocation]

        # ---- accept: the permission covers one allocation at a time, so a
        # second matching load waits for a person; accepting it is one-off --
        third, plan = prepare()
        shown3 = pending(plan, known={shown["allocation"], shown2["allocation"]})
        # Fill the sign-in's grant list before this load is accepted: the
        # model still loads, and the missing grant is reported, not hidden.
        record = home / ".lumabri/api-access/local.user"
        saved = record.read_bytes()
        assert saved[:8] == b"LMBUSER1" and len(saved) == 76 + 32 and saved[76:].hex() == allocation, len(saved)
        order = "little" if saved[72:76] == (1).to_bytes(4, "little") else "big"
        full = saved[:72] + (64).to_bytes(4, order) + saved[76:] + b"".join(secrets.token_bytes(32) for _ in range(63))
        pending_record = record.with_name(".test-local.user")
        pending_record.write_bytes(full); pending_record.chmod(0o600); pending_record.replace(record)
        time.sleep(3)
        assert call(PATH, token)[1]["pending"], "the repeat permission exceeded its one allocation"
        status, accepted = call(PATH, token, {"action": "accept", **shown3})
        assert status == 200 and accepted["result"] == "accepted" and "owner policy" not in accepted["detail"], accepted
        assert call(PATH, token, {"action": "accept", **shown3})[0] == 409, "one approval was usable twice"
        done3 = finished(third)
        assert done3["state"] == "done" and done3["chat_access"] == "grant_list_full" and \
            "most model grants" in done3["chat_access_detail"], done3
        unlisted = done3["allocations"][0]["id"]
        assert unlisted == shown3["allocation"] != allocation
        assert [m["id"] for m in call("/api/v1/models", token)[1]["models"]] == [allocation]
        assert chat_status(port, token, unlisted) == (403, {"error": "model_not_authorized"})
        refused = run("api", "grant", "local", unlisted)
        assert refused.returncode == 1 and "already holds 64 model grants" in refused.stderr, refused.stderr
        assert record.read_bytes() == full, "a full grant list was changed"
        assert policies() == [policy], "a one-off acceptance changed the repeat permissions"
        state = donor()
        assert state["model_count"] == 2 and state["reserved_total_bytes"] == 2 * reserved, state

        # ---- revoking the permission unloads nothing ----------------------
        current = call(PATH, token)[1]
        assert current["request"]["allocation"] == shown3["allocation"] and not current["pending"], current
        status, revoked = call(PATH, token, {"action": "revoke_repeat", **shown3, "revision": current["revision"]})
        assert status == 200 and revoked["result"] == "observed" and "revoked" in revoked["detail"], revoked
        (after_revoke,) = policies()
        assert not after_revoke["enabled"] and after_revoke["revision"] == policy["revision"] + 1, after_revoke
        assert donor()["model_count"] == 2, "revocation unloaded resident weights"
        assert generate(port, token, allocation)
        passed = True
    finally:
        # Releasing resident weights can outlast one stop command's wait.
        for _ in range(4):
            stopped = run("service", "stop")
            if stopped.returncode == 0:
                break
        if stopped.returncode == 0 and passed:
            shutil.rmtree(temporary, ignore_errors=True)
        else:
            print(f"Artifacts kept: {temporary}", file=sys.stderr)
    assert stopped.returncode == 0, stopped.stderr
    print("web approval: PASS")


def page_prepare(port, token, node):
    from playwright.sync_api import sync_playwright, expect
    playwright = sync_playwright().start()
    chromium = playwright.chromium.launch(headless=True)
    page = chromium.new_page(viewport={"width": 1280, "height": 860})
    errors = []; page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(f"http://127.0.0.1:{port}/#token={token}")
    expect(page.locator("#workspace-open")).to_be_visible(timeout=15000)
    expect(page.locator("#model option")).to_have_text(["No approved resident models"])
    page.evaluate("window.loadedOnce = true")
    page.locator("#workspace-open").click()
    page.locator("#prepare-catalogue").click()
    page.locator('#prepare-models input[value="reduced"]').check()
    page.locator(f'#prepare-nodes input[value="{node}"]').check()
    page.locator("#prepare-context").fill("128"); page.locator("#prepare-output").fill("8")
    page.locator("#prepare-preview").click()
    expect(page.locator("#prepare-start")).to_be_visible(timeout=30000)
    page.locator("#prepare-start").click()
    expect(page.locator("#prepare-operation option", has_text="Current ·")).to_have_count(1, timeout=30000)
    operation = page.locator("#prepare-operation").input_value()
    assert len(operation) == 64, operation
    # Leaving Computers & models must not stop the page from following it.
    page.locator('.workspace-switch [data-view="chat"]').click()
    expect(page.locator("#workspace")).to_be_hidden()
    return {"playwright": playwright, "chromium": chromium, "page": page, "errors": errors, "operation": operation}


def page_approve(browser, allocation):
    from playwright.sync_api import expect
    page = browser["page"]
    try:
        expect(page.locator("#approval-title")).to_have_text("Load reduced on this computer?", timeout=15000)
        page.once("dialog", lambda dialog: dialog.accept())
        with page.expect_response(lambda r: r.url.endswith(PATH) and r.request.method == "POST") as sent:
            page.locator("#approval-repeat").click()
        assert sent.value.status == 200 and sent.value.json()["result"] == "accepted", sent.value.text()
        expect(page.locator(f'#model option[value="{allocation}"]')).to_have_count(1, timeout=240000)
        expect(page.locator("#model")).to_have_value(allocation)
        expect(page.locator("#notice")).to_contain_text("is ready to chat")
        assert page.evaluate("window.loadedOnce === true"), "the page was reloaded"
        assert not browser["errors"], browser["errors"]
    finally:
        browser["chromium"].close(); browser["playwright"].stop()


def chat_status(port, token, model):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    try:
        connection.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
            "messages": [{"role": "user", "content": "Count from one to ten."}]}),
            {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


def generate(port, token, model):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
    try:
        connection.request("POST", "/api/v1/chat", json.dumps({"model": model, "max_tokens": 8,
            "messages": [{"role": "user", "content": "Count from one to ten."}]}),
            {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        response = connection.getresponse()
        assert response.status == 200, (response.status, response.read())
        # Random reduced weights may decode to no visible text; the engine's
        # own token notifications are the evidence that inference ran.
        event = None; done = False; text = bytearray()
        while True:
            line = response.readline()
            if not line:
                break
            if line.startswith(b"event: "):
                event = line[7:].strip()
            elif line.startswith(b"data: "):
                data = json.loads(line[6:])
                if event == b"delta":
                    text.extend(base64.b64decode(data["bytes"], validate=True))
                else:
                    assert event == b"done" and data["timing"]["token_notifications"] == 8, (event, data)
                    done = True
        return done
    finally:
        connection.close()


def screenshots(folder, port, token, shown):
    """The prompt as a person sees it, in four renderings; the last one declines."""
    from playwright.sync_api import sync_playwright, expect
    folder.mkdir(parents=True, exist_ok=True)
    views = [(name, width, height, scheme) for name, width, height in (("desktop", 1280, 860), ("phone", 390, 844))
             for scheme in ("light", "dark")]
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        for index, (name, width, height, scheme) in enumerate(views):
            context = browser.new_context(viewport={"width": width, "height": height}, color_scheme=scheme)
            page = context.new_page()
            errors = []; page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(f"http://127.0.0.1:{port}/#token={token}")
            expect(page.locator("#approval")).to_be_visible(timeout=15000)
            expect(page.locator("#approval-title")).to_have_text("Load reduced on this computer?")
            facts = page.locator("#approval-facts").inner_text()
            assert shown["allocation"][:12] in facts and "(olmoe)" in facts and "0–15 of 16" in facts, facts
            for button in ("#approval-decline", "#approval-accept", "#approval-repeat"):
                expect(page.locator(button)).to_be_visible()
            expect(page.locator("#approval-revoke")).to_be_hidden()
            # Nothing is preselected: no control of the prompt holds focus.
            assert page.evaluate("document.activeElement.closest('#approval') === null")
            assert page.evaluate("document.documentElement.scrollWidth <= window.innerWidth"), "the prompt overflows the viewport"
            page.screenshot(path=str(folder / f"approval-{name}-{scheme}.png"))
            if index == len(views) - 1:
                with page.expect_response(lambda r: r.url.endswith(PATH) and r.request.method == "POST") as sent:
                    page.locator("#approval-decline").click()
                assert sent.value.status == 200 and sent.value.json()["result"] == "declined", sent.value.text()
                assert sent.value.request.post_data_json == {"action": "decline", **shown}
                expect(page.locator("#approval-kicker")).to_have_text("declined")
                expect(page.locator("#approval-summary")).to_contain_text("No weights or model state were loaded")
                page.screenshot(path=str(folder / f"approval-{name}-{scheme}-declined.png"))
                page.locator("#approval-dismiss").click()
                expect(page.locator("#approval")).to_be_hidden()
            assert not errors, errors
            context.close()
        browser.close()


if __name__ == "__main__":
    main()
