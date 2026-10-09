"""Faults at real OLMoE output boundaries; requires the disposable codec proxy."""
import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import time
import uuid


def verify_recovery(env, first, third, route, chat, request, cli, command, port, token):
    control = Path(env["LUMABRI_TEST_RECOVERY_CONTROL"])

    def arm(mode, secondary=None, wait=False):
        nonce = uuid.uuid4().hex
        models = {first["name"]: {"mode": mode, "wait": wait}}
        if secondary:
            models[third["name"]] = {"mode": secondary}
        elif mode in ("typed", "diverge", "truncate"):
            models[third["name"]] = {"mode": "observe"}
        pending = control.with_suffix(".pending")
        pending.write_text(json.dumps({"nonce": nonce, "models": models}))
        pending.replace(control)
        return nonce

    def record(nonce, model):
        path = control.parent / (nonce + "-" + model["name"] + ".json")
        deadline = time.monotonic() + 10
        while not path.exists():
            assert time.monotonic() < deadline, ("fault did not activate", path)
            time.sleep(.02)
        return json.loads(path.read_text())

    def events(raw):
        result = []
        for block in raw.split(b"\n\n"):
            if not block:
                continue
            lines = block.splitlines()
            assert len(lines) == 2 and lines[0].startswith(b"event: ") and lines[1].startswith(b"data: "), block
            result.append((lines[0][7:].decode(), json.loads(lines[1][6:])))
        return result

    def deltas(parsed):
        return b"".join(base64.b64decode(value["bytes"]) for kind, value in parsed if kind == "delta")

    def calibrations():
        directory = Path(env["HOME"]) / ".lumabri/calibrations"
        return {str(p): p.read_bytes() for p in directory.rglob("*.cal")}

    for mode, secondary in (("nonretryable", None), ("typed", None), ("diverge", None),
                            ("typed", "short"), ("typed", "typed")):
        before = calibrations()
        nonce = arm(mode, secondary)
        status, data = chat(route["id"])
        parsed = events(data); kinds = [kind for kind, _ in parsed]
        original = record(nonce, first)
        print("RECOVERY CASE:", mode, secondary, flush=True)
        assert status == 200, (status, data)
        assert calibrations() == before, "failed/recovered turn replaced a no-replay timing"
        if mode == "typed" and secondary is None:
            assert "recovering" in kinds and kinds[-1] == "done" and "error" not in kinds, data
            assert deltas(parsed) == base64.b64decode(original["oracle"]), (parsed, original)
            done = parsed[-1][1]
            assert done["recovery_attempts"] == 1 and done["stats_scope"] == "final_attempt_only"
            assert done["observation_saved"] is False
            assert done["timing"]["token_notifications"] is None
            assert done["timing"]["token_notification_gaps"] is None, "replayed prefix became visible-token latency"
            assert done["timing"]["completion_seconds"] >= done["timing"]["ttft_seconds"] >= 0
        else:
            diagnostic = None
            if kinds[-1] != "error" or "done" in kinds:
                path = control.parent / (nonce + "-" + third["name"] + ".json")
                diagnostic = json.loads(path.read_text()) if path.exists() else "secondary fault never activated"
            assert kinds[-1] == "error" and "done" not in kinds, (mode, secondary, original, diagnostic, data)
            assert deltas(parsed) == base64.b64decode(original["visible"]), (parsed, original)
            assert ("recovering" in kinds) == (mode != "nonretryable"), data
            if mode == "diverge" or secondary == "short":
                assert "prefix" in parsed[-1][1]["message"], data
        if mode == "nonretryable":
            # This is a single-slot host in the native recovery gate. Do not
            # wait/retry: the completed HTTP error must already have observed
            # remote cleanup, including the proxy's delayed RESET_DONE.
            idle = json.loads(command("replica", first["id"], "status"))
            assert idle["connections"] == idle["admitted_requests"] == 0, idle
        control.unlink()

    # A disconnected browser must not resubmit its text on another replica.
    nonce = arm("typed", "typed", wait=True)
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    payload = {"model": route["id"], "max_tokens": 8, "messages": [{"role": "user", "content": "hi"}]}
    conn.request("POST", "/api/v1/chat", json.dumps(payload),
                 {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    response = conn.getresponse(); assert response.status == 200
    while True:
        line = response.readline()
        assert line, "stream closed before cancellation point"
        if line == b"event: delta\n":
            break
    record(nonce, first)
    response.close(); conn.close()
    (control.parent / (nonce + ".release")).touch()
    time.sleep(.5)
    assert not (control.parent / (nonce + "-" + third["name"] + ".json")).exists(), "cancellation replayed text"
    control.unlink()

    # Revocation during an interrupted request forbids the next disclosure.
    revoked = cli("user-add", "recovery-revoked")
    command("grant", "recovery-revoked", route["id"])
    nonce = arm("typed", "typed", wait=True)
    with ThreadPoolExecutor(max_workers=1) as pool:
        future = pool.submit(request, "/api/v1/chat", revoked, payload)
        record(nonce, first)
        cli("revoke", "recovery-revoked")
        replacement = cli("user-add", "recovery-revoked")
        assert replacement != revoked
        command("grant", "recovery-revoked", route["id"])
        (control.parent / (nonce + ".release")).touch()
        status, data = future.result(timeout=30)
    parsed = events(data)
    assert status == 200 and parsed[-1][0] == "error" and "revoked" in parsed[-1][1]["message"], data
    assert not any(kind in ("done", "recovering") for kind, _ in parsed), data
    assert not (control.parent / (nonce + "-" + third["name"] + ".json")).exists(), "revoked text reached another replica"
    control.unlink()

    # Finally kill the preferred codec after a visible byte, without a terminal
    # frame. Oracle comparison proves both replay determinism (nonzero sampling)
    # and no lost/duplicated bytes at the real gateway's transport failure.
    before = calibrations(); nonce = arm("truncate")
    status, data = chat(route["id"])
    parsed = events(data); original = record(nonce, first)
    assert status == 200 and parsed[-1][0] == "done" and any(kind == "recovering" for kind, _ in parsed), data
    assert deltas(parsed) == base64.b64decode(original["oracle"]), (parsed, original)
    assert parsed[-1][1]["recovery_attempts"] == 1 and calibrations() == before
    assert parsed[-1][1]["timing"]["token_notifications"] is None
    assert parsed[-1][1]["timing"]["token_notification_gaps"] is None
    control.unlink()
    print("MANAGED RECOVERY: PASS (real OLMoE sampling replay, typed failure, codec EOF, exact output oracle, divergent/short prefix, no capacity, cancellation, revocation, no replay-cost calibration)", flush=True)
