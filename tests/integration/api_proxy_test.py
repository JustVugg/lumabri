"""Private HTTPS upstream contract. No TLS claims, engines or real household."""
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
ORIGIN = "https://chat.example.test:8443"


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__(ORIGIN[8:], timeout=5)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def main():
    # Darwin's default TMPDIR can exceed sockaddr_un.sun_path. Use a short,
    # private test home and resolve /tmp -> /private/tmp as the service does.
    with tempfile.TemporaryDirectory(prefix="lmb-proxy-", dir="/tmp") as temporary:
        home = Path(temporary).resolve()
        env = {**os.environ, "HOME": str(home), "LUMABRI_PEER_KEY": str(home / "peer.key"),
               "LUMABRI_KNOWN_HOSTS": str(home / "known"), "LUMABRI_ENCRYPT": "1"}
        env.pop("LUMABRI_HOME_FOREGROUND", None)
        access = home / ".lumabri/api-access"
        upstream = access / "proxy.sock"

        def run(*args, good=True):
            result = subprocess.run([str(ROOT / "lumabri"), *args], env=env, text=True,
                                    capture_output=True, timeout=20)
            if good:
                assert result.returncode == 0, (args, result.stderr)
            return result

        def states():
            return {s["role"]: s for s in json.loads(run("service", "status", "--json").stdout)["services"]}

        def request(path="/api/v1/models", token=None, headers=None, body=None):
            conn = UnixHTTP(upstream)
            options = {"Origin": ORIGIN}
            if token is not None:
                options["Authorization"] = "Bearer " + token
            if body is not None:
                options["Content-Type"] = "application/json"
            options.update(headers or {})
            conn.request("GET" if body is None else "POST", path, body, options)
            response = conn.getresponse()
            answer = response.status, response.read(), dict(response.getheaders())
            conn.close()
            return answer

        start = ("api", "start", "--tracker", "127.0.0.1:1", "--https-origin", ORIGIN)
        pending = None
        try:
            alice = run("api", "user-add", "alice").stdout.strip()
            bob = run("api", "user-add", "bob").stdout.strip()
            # History-only authority fixture: no invented model readiness.
            for name in ("alice", "bob"):
                path = access / f"{name}.user"
                raw = path.read_bytes()
                assert raw[:8] == b"LMBUSER1" and len(raw) == 76
                path.write_bytes(raw[:72] + struct.pack("<I", 1) + bytes.fromhex("01" * 32))
            for origin in ("http://chat.test", "https://chat.test/", "https://chat.test:443"):
                assert run(*start[:-1], origin, good=False).returncode != 0
            assert run(*start, "--port", "47380", good=False).returncode != 0
            assert run(*start, "--https-origin", ORIGIN, good=False).returncode != 0
            assert not upstream.exists()
            run(*start)
            assert stat.S_ISSOCK(upstream.stat().st_mode) and stat.S_IMODE(upstream.stat().st_mode) == 0o600
            assert stat.S_IMODE(access.stat().st_mode) == 0o700
            initial = states()["api"]
            assert initial["live"] and initial["phase"] == 2 and ORIGIN in initial["detail"]
            assert request()[0] == 401 and request(token="invalid")[0] == 401
            assert request(token=alice)[:2] == (200, b'{"schema":1,"models":[]}\n')
            assert request("/")[0] == 200  # shell contains no private data
            assert "frame-ancestors 'none'" in request("/")[2]["Content-Security-Policy"]
            for headers in ({"Host": "evil.test", "X-Forwarded-Host": ORIGIN[8:]},
                            {"Origin": "http://chat.example.test:8443", "X-Forwarded-Proto": "https"},
                            {"Origin": "https://evil.test"}, {"Origin": "null"},
                            {"Host": "localhost:47380"}):
                assert request(token=alice, headers=headers)[0] == 403, headers
            assert request(token=alice, headers={"X-Forwarded-Host": "evil.test", "Forwarded": "host=evil.test"})[0] == 200
            assert request("/api/v1/workspace", token=alice)[0] == 403
            run("api", "operator-grant", "alice")
            assert request("/api/v1/workspace", token=alice)[0] == 200
            assert request("/api/v1/preparation", token=alice, body="{}")[0] == 403
            conversation = {"revision": 0, "conversation": {"model": "01" * 32, "title": "private",
                            "state": "complete", "messages": [{"role": "user", "content": "private text"},
                                                               {"role": "assistant", "content": "private reply"}]}}
            status, body, _ = request("/api/v1/conversations", token=alice, body=json.dumps(conversation))
            assert status == 200, body
            saved = json.loads(body); path = "/api/v1/conversations/" + saved["id"]
            assert request(path, token=bob)[0] == 404
            assert json.loads(request(path, token=alice)[1])["conversation"] == conversation["conversation"]
            run(*start)
            assert states()["api"]["instance"] == initial["instance"]
            assert run(*start[:-1], "https://other.example.test", good=False).returncode != 0
            assert run("api", "start", "--tracker", "127.0.0.1:1", good=False).returncode != 0
            run("service", "restart")
            assert states()["api"]["instance"] == initial["instance"]
            # A request child must not inherit the Unix listener/singleton lock.
            pending = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            pending.connect(str(upstream)); pending.sendall(b"GET ")
            time.sleep(.2)
            os.kill(initial["pid"], signal.SIGKILL)
            deadline = time.monotonic() + 5
            while states()["api"]["live"]:
                assert time.monotonic() < deadline
                time.sleep(.05)
            assert upstream.exists(), "crash fixture should leave a stale socket"
            run(*start)
            assert states()["api"]["instance"] != initial["instance"]
            pending.close(); pending = None
            assert request(path, token=alice)[0] == 200
            run("api", "revoke", "alice")
            assert request(path, token=alice)[0] == 401
            run("api", "stop"); assert not upstream.exists()

            # No clobbering arbitrary files, symlinks or another live listener.
            sentinel = home / "sentinel"; sentinel.write_text("unchanged")
            for kind in ("file", "symlink", "active", "unsafe"):
                occupied = None
                if kind == "file":
                    upstream.write_text("unchanged"); upstream.chmod(0o600)
                elif kind == "symlink":
                    upstream.symlink_to(sentinel)
                else:
                    occupied = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    occupied.bind(str(upstream)); upstream.chmod(0o600 if kind == "active" else 0o666)
                    if kind == "active":
                        occupied.listen(1)
                inode = upstream.lstat().st_ino
                try:
                    assert run(*start, good=False).returncode != 0
                    assert upstream.lstat().st_ino == inode and sentinel.read_text() == "unchanged"
                finally:
                    if occupied:
                        occupied.close()
                    upstream.unlink()  # exact fixture entry created above
            run(*start)
            assert request(token=bob)[0] == 200
            run("service", "stop")
            assert not upstream.exists() and all(not s["live"] for s in states().values())
        finally:
            if pending:
                pending.close()
            run("service", "stop", good=False)
    print("API PROXY: PASS (private socket, auth, origin, grants, history isolation, restart, crash, no unsafe replacement)")


if __name__ == "__main__":
    main()
