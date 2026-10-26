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
               "LUMABRI_KNOWN_HOSTS": temporary + "/known"}
        env.pop("LUMABRI_ENCRYPT", None)
        env.pop("LUMABRI_TOKEN", None)
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
        run("api", "operator-grant", "alice")
        with socket.socket() as reserving:
            reserving.bind(("127.0.0.1", 0)); port = reserving.getsockname()[1]
        with socket.socket() as reserving:
            reserving.bind(("127.0.0.1", 0)); tracker_port = reserving.getsockname()[1]
        tracker_address = f"127.0.0.1:{tracker_port}"
        # The tracker is encrypted and requires the key saved by the TUI. API
        # commands below run from a fresh terminal without either env override.
        household = Path(temporary) / ".lumabri" / "home.conf"
        household.write_text(f"tracker={tracker_address}\ntoken=api-service-test-key\n")
        household.chmod(0o600)
        tracker = subprocess.Popen([str(ROOT / "tracker"), "--port", str(tracker_port),
                                    "--token", "api-service-test-key"],
                                   env={**env, "LUMABRI_ENCRYPT": "1"},
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        start = ("api", "start", "--port", str(port))
        def models():
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/api/v1/models", headers={"Authorization": "Bearer " + token})
            reply = conn.getresponse(); body = reply.read(); conn.close()
            assert reply.status == 200 and json.loads(body)["models"] == []
        def demand():
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/api/v1/demand", headers={"Authorization": "Bearer " + token})
            reply = conn.getresponse(); body = reply.read(); conn.close()
            assert reply.status == 200
            return json.loads(body)
        def workspace():
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/api/v1/workspace", headers={"Authorization": "Bearer " + token})
            reply = conn.getresponse(); body = reply.read(); conn.close()
            assert reply.status == 200 and json.loads(body)["inventory_ok"], body
        pending = None
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", tracker_port), timeout=.2):
                        break
                except OSError:
                    assert tracker.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            assert json.loads(run("api", "workspace").stdout)["inventory_ok"], \
                "fresh-terminal CLI did not authenticate to the encrypted household"
            for mode in ("start", "serve"):
                rejected = subprocess.run([str(ROOT / "lumabri"), "api", mode, "--port", str(port)],
                    env={**env, "LUMABRI_ENCRYPT": "0"}, text=True, capture_output=True, timeout=10)
                assert rejected.returncode != 0 and "require authenticated encryption" in rejected.stderr
                assert not states()["api"]["live"]
            run(*start); initial = states()
            assert initial["api"]["live"] and initial["api"]["phase"] == 2
            assert initial["manager"]["live"]
            models()
            workspace()
            initial_demand = demand()
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
            workspace()
            assert demand()["instance"] == initial_demand["instance"], "manager restart reset live gateway evidence"
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
            restarted_demand = demand()
            assert restarted_demand["instance"] != initial_demand["instance"] and not restarted_demand["window_complete"]
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
            tracker.terminate()
            try:
                tracker.wait(timeout=5)
            except subprocess.TimeoutExpired:
                tracker.kill(); tracker.wait(timeout=5)
    print("API SERVICE: PASS (saved household key, default encrypted peer transport, plaintext rejection, independent lifetime, idempotent start, manager restart, crashed parent, occupied port and isolated stop)")


if __name__ == "__main__":
    main()
