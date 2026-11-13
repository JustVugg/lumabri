"""Approve a load from the page's API, on the computer that shares the memory.

`lumabri start` leaves a headless donor with nobody at its "Share resources"
screen. This gate prepares a real reduced checkpoint through the same API the
page uses, and decides the donor's request through /api/v1/donor/approval:
declined once (nothing loads), allowed once through the optional one-hour
repeat permission (the model answers), accepted once, then the permission is
revoked. Loopback only; it is
not a LAN test and not a measurement.
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
        second, plan = prepare()
        shown2 = pending(plan, known={shown["allocation"]})
        assert shown2["instance"] == shown["instance"]
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
        # Inference grants are exact and separate; the next start adds the
        # model this household approved to the local sign-in.
        assert call("/api/v1/models", token)[1]["models"] == []
        assert start() == token
        (granted,) = call("/api/v1/models", token)[1]["models"]
        assert granted["id"] == allocation and granted["name"].endswith("-reduced"), granted
        assert generate(port, token, allocation)

        # ---- accept: the permission covers one allocation at a time, so a
        # second matching load waits for a person; accepting it is one-off --
        third, plan = prepare()
        shown3 = pending(plan, known={shown["allocation"], shown2["allocation"]})
        time.sleep(3)
        assert call(PATH, token)[1]["pending"], "the repeat permission exceeded its one allocation"
        status, accepted = call(PATH, token, {"action": "accept", **shown3})
        assert status == 200 and accepted["result"] == "accepted" and "owner policy" not in accepted["detail"], accepted
        assert call(PATH, token, {"action": "accept", **shown3})[0] == 409, "one approval was usable twice"
        assert finished(third)["state"] == "done"
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
