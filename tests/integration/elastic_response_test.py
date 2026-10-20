"""Same-turn recovery on actual OLMoE output captured by the test-only proxy."""
import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import socket
import threading
import time
import uuid


class WaitingTurn:
    def __init__(self, api, token, model_id, source_name, seconds=120, hold=False):
        self.control = Path(api.env["LUMABRI_TEST_RECOVERY_CONTROL"])
        self.nonce = uuid.uuid4().hex
        self.oracle_path = self.control.parent / (self.nonce + "-" + source_name + ".json")
        pending = self.control.with_suffix(".pending")
        pending.write_text(json.dumps({"nonce": self.nonce, "models": {
            source_name: {"mode": "typed", "wait": hold}}}))
        pending.replace(self.control)
        self.connection = http.client.HTTPConnection("127.0.0.1", api.port, timeout=150)
        self.first = threading.Event(); self.waiting = threading.Event()
        self.frames = []; self.response = None; self.cancelled = False
        self.pool = ThreadPoolExecutor(max_workers=1)
        payload = {"model": model_id, "max_tokens": 8, "recovery_wait_seconds": seconds,
                   "messages": [{"role": "user", "content": "hello elastic service"}]}
        self.future = self.pool.submit(self.run, token, payload)

    def run(self, token, payload):
        try:
            self.connection.request("POST", "/api/v1/chat", json.dumps(payload),
                {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            self.response = self.connection.getresponse()
            assert self.response.status == 200, (self.response.status, self.response.read())
            while True:
                line = self.response.readline()
                if not line:
                    break
                assert line.startswith(b"event: "), line
                data = self.response.readline(); assert data.startswith(b"data: "), data
                assert self.response.readline() == b"\n"
                kind, value = line[7:].strip().decode(), json.loads(data[6:])
                self.frames.append((kind, value))
                if kind == "delta":
                    self.first.set()
                if kind == "recovering" and value.get("phase") == "waiting":
                    self.waiting.set()
            return self.frames
        except (OSError, ValueError):
            if not self.cancelled:
                raise
            return self.frames
        finally:
            if self.response:
                self.response.close()
            self.connection.close()

    def ready(self, waiting=True):
        assert self.first.wait(15), ("no real visible prefix", self.frames)
        until = time.monotonic() + 10
        while not self.oracle_path.exists():
            assert time.monotonic() < until, "real oracle was not captured"
            time.sleep(.02)
        if waiting:
            assert self.waiting.wait(15), ("no visible replacement wait", self.frames)
        return json.loads(self.oracle_path.read_text())

    def release(self):
        (self.control.parent / (self.nonce + ".release")).touch()

    def cancel(self):
        self.cancelled = True
        if self.response and self.response.fp:
            self.response.fp.raw._sock.shutdown(socket.SHUT_RDWR)

    def finish(self, success=False, contains=None):
        frames = self.future.result(timeout=130)
        assert frames and frames[-1][0] == ("done" if success else "error"), frames
        oracle = json.loads(self.oracle_path.read_text())
        emitted = b"".join(base64.b64decode(value["bytes"]) for kind, value in frames if kind == "delta")
        assert emitted == base64.b64decode(oracle["oracle" if success else "visible"]), (frames, oracle)
        if contains:
            assert contains in frames[-1][1]["message"], frames
        if success:
            done = frames[-1][1]
            assert done["recovery_attempts"] == 1 and done["observation_saved"] is False, done
            assert done["stats_scope"] == "final_attempt_only" and done["timing"]["token_notifications"] is None
        return frames

    def close(self):
        self.release()
        if not self.future.done():
            self.cancel()
        try:
            self.future.result(timeout=15)
        finally:
            self.pool.shutdown(wait=True)
            self.control.unlink(missing_ok=True)


def verify_wait_boundaries(api, token, route, source, policy):
    before = policy()
    turn = WaitingTurn(api, token, route, source, seconds=0)
    try:
        turn.ready(waiting=False); frames = turn.finish()
        assert not any(kind == "recovering" for kind, _ in frames), frames
    finally:
        turn.close()
    turn = WaitingTurn(api, token, route, source, seconds=1)
    try:
        turn.ready(); turn.finish(contains="expired")
    finally:
        turn.close()
    turn = WaitingTurn(api, token, route, source)
    try:
        turn.ready(); turn.cancel(); frames = turn.future.result(timeout=15)
        assert not any(kind == "done" for kind, _ in frames), frames
    finally:
        turn.close()
    turn = WaitingTurn(api, token, route, source)
    try:
        turn.ready()
        code, disabled = api.http("/api/v1/elastic-policy", {
            "action": "disable", "id": route, "revision": policy()["revision"]})
        assert code == 200, disabled
        turn.finish(contains="policy")
    finally:
        turn.close()
    code, resumed = api.http("/api/v1/elastic-policy", {
        "action": "resume", "id": route, "revision": policy()["revision"]})
    assert code == 200, resumed
    revoked = api.cli("user-add", "waiting-revoked")
    api.cli("grant", "waiting-revoked", route)
    turn = WaitingTurn(api, revoked, route, source)
    try:
        turn.ready(); api.cli("revoke", "waiting-revoked")
        replacement = api.cli("user-add", "waiting-revoked")
        assert replacement != revoked
        api.cli("grant", "waiting-revoked", route)
        turn.finish(contains="revoked")
    finally:
        turn.close()
    assert policy()["reserved_bytes"] == before["reserved_bytes"]
    print("RECOVERY WAIT BOUNDARIES: PASS (real prefix, explicit opt-in, deadline, disconnect, policy/credential revocation, no new allocations)", flush=True)
