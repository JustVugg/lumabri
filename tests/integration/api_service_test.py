"""Permanent API keeper: no model or paid infrastructure is involved."""
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="lmb-api-service-") as temporary:
        env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
               "LUMABRI_KNOWN_HOSTS": temporary + "/known", "LUMABRI_ENCRYPT": "1"}
        env.pop("LUMABRI_HOME_FOREGROUND", None)
        def run(*args, good=True):
            result = subprocess.run([str(ROOT / "lumabri"), *args], env=env, text=True,
                                    capture_output=True, timeout=20)
            if good:
                assert result.returncode == 0, (args, result.stderr)
            return result
        def states():
            return {s["role"]: s for s in json.loads(run("service", "status", "--json").stdout)["services"]}
        token = run("api", "user-add", "alice").stdout.strip()
        with socket.socket() as reserving:
            reserving.bind(("127.0.0.1", 0)); port = reserving.getsockname()[1]
        start = ("api", "start", "--tracker", "127.0.0.1:1", "--port", str(port))
        def models():
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/api/v1/models", headers={"Authorization": "Bearer " + token})
            reply = conn.getresponse(); body = reply.read(); conn.close()
            assert reply.status == 200 and json.loads(body)["models"] == []
        pending = None
        try:
            run(*start); initial = states()
            assert initial["api"]["live"] and initial["api"]["phase"] == 2
            assert initial["manager"]["live"]
            models()
            run(*start)
            assert states()["api"]["instance"] == initial["api"]["instance"]
            wrong = (*start[:-1], str(port + 1 if port < 65535 else port - 1))
            assert run(*wrong, good=False).returncode != 0
            assert states()["api"]["instance"] == initial["api"]["instance"]
            run("service", "restart")
            restarted = states()
            assert restarted["manager"]["instance"] != initial["manager"]["instance"]
            assert restarted["api"]["instance"] == initial["api"]["instance"]
            assert restarted["api"]["pid"] == initial["api"]["pid"]
            models()
            # An accepted request must not inherit the keeper singleton lock.
            pending = socket.create_connection(("127.0.0.1", port), timeout=5)
            pending.sendall(b"GET "); time.sleep(.2)
            live = states()["api"]; assert live["live"]
            os.kill(live["pid"], signal.SIGKILL)  # only this test's live authenticated keeper
            deadline = time.monotonic() + 5
            while states()["api"]["live"]:
                assert time.monotonic() < deadline
                time.sleep(.05)
            assert states()["api"]["state"] == "interrupted"
            run(*start)
            assert states()["api"]["instance"] != live["instance"]
            pending.close(); pending = None
            models()
            run("api", "stop")
            assert states()["api"]["state"] == "stopped" and states()["manager"]["live"]
            with socket.socket() as occupied:
                occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                occupied.bind(("127.0.0.1", port)); occupied.listen(1)
                assert run(*start, good=False).returncode != 0
                assert not states()["api"]["live"]
            run(*start); models()
            run("service", "stop")
            assert all(not s["live"] for s in states().values())
        finally:
            if pending:
                pending.close()
            run("service", "stop", good=False)
    print("API SERVICE: PASS (independent lifetime, idempotent start, manager restart, crashed parent, occupied port and isolated stop)")


if __name__ == "__main__":
    main()
