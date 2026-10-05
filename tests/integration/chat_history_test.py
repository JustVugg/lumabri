"""Private, bounded browser-history API; no model allocation is executed."""
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
MODEL = "01" * 32


def main():
    with tempfile.TemporaryDirectory(prefix="lmb-chat-history-") as temporary:
        home = Path(temporary)
        env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
               "LUMABRI_KNOWN_HOSTS": temporary + "/known"}

        def cli(*args):
            answer = subprocess.run([str(ROOT / "lumabri"), "api", *args], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert answer.returncode == 0, answer.stderr
            return answer.stdout.strip()

        alice, bob = cli("user-add", "alice"), cli("user-add", "bob")
        # Grant fixture authority without inventing a live model/engine. The
        # history boundary must still reject every ungranted allocation.
        for name in ("alice", "bob"):
            path = home / ".lumabri/api-access" / f"{name}.user"
            raw = path.read_bytes()
            assert raw[:8] == b"LMBUSER1" and len(raw) == 76
            path.write_bytes(raw[:72] + struct.pack("<I", 1) + bytes.fromhex(MODEL))
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        process = subprocess.Popen([str(ROOT / "lumabri"), "api", "serve", "--tracker", "127.0.0.1:1", "--port", str(port)],
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        def request(path="/api/v1/conversations", method="GET", body=None, token=alice):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            headers = {"Authorization": "Bearer " + token}
            if body is not None:
                body = json.dumps(body); headers["Content-Type"] = "application/json"
            conn.request(method, path, body, headers); response = conn.getresponse()
            payload = response.read(); status = response.status; response_headers = dict(response.getheaders()); conn.close()
            if response_headers.get("Content-Type") == "application/json": payload = json.loads(payload)
            return status, payload, response_headers

        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.2): break
                except OSError:
                    assert process.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            initial = request(); assert initial[0] == 200, initial
            assert initial[1]["conversations"] == []
            for asset in ("/", "/app.css", "/app.js", "/logo.svg"):
                status, content, headers = request(asset, token="")
                assert status == 200 and len(content) > 100
                assert "frame-ancestors 'none'" in headers["Content-Security-Policy"]
                assert headers["Cache-Control"] == "no-store"
            assert request(token="")[0] == 401
            conversation = {"model": MODEL, "title": "Private <script>alert(1)</script> café",
                            "state": "pending", "messages": [{"role": "user", "content": "private question"}]}
            create = {"revision": 0, "conversation": conversation}
            assert request(method="POST", body={**create, "conversation": {**conversation, "model": "02" * 32}})[0] == 403
            status, saved, _ = request(method="POST", body=create); assert status == 200, saved
            path = "/api/v1/conversations/" + saved["id"]
            assert saved["revision"] == 1 and saved["conversation"] == conversation
            assert request(path, token=bob)[0] == 404
            assert request(token=bob)[1]["conversations"] == []
            assert request(path, "DELETE", {"revision": 1}, token=bob)[0] == 404
            assert request(path, "POST", {"revision": 1, "conversation": conversation}, token=bob)[0] == 404
            status, listing, _ = request(); assert status == 200
            assert listing["conversations"][0]["title"] == conversation["title"]
            assert "private question" not in json.dumps(listing)
            conversation = {**conversation, "state": "complete", "messages": conversation["messages"] +
                            [{"role": "assistant", "content": "reply €\n<script>text only</script>"}]}
            assert request(path, "POST", {"revision": 0, "conversation": conversation})[0] == 400
            assert request(path, "POST", {"revision": 2, "conversation": conversation})[0] == 409
            assert request(path, "POST", {"revision": 1, "conversation": conversation})[0] == 200
            assert request(path, "POST", {"revision": 1, "conversation": conversation})[0] == 409
            for state in ("idle", "pending", "invented"):
                assert request(path, "POST", {"revision": 2, "conversation": {**conversation, "state": state}})[0] == 400
            invalid = {**conversation, "messages": [{"role": "system", "content": "no"}]}
            assert request(path, "POST", {"revision": 2, "conversation": invalid})[0] == 400
            archival = {**conversation, "messages": [{"role": "user", "content": "hi"},
                                                    {"role": "assistant", "content": "before\0after"}]}
            assert request(path, "POST", {"revision": 2, "conversation": archival})[0] == 200
            assert request(path)[1]["conversation"]["messages"][-1]["content"] == "before\0after"
            assert request(path, "POST", {"revision": 3, "conversation": {**archival, "title": "bad\0title"}})[0] == 400
            invalid = {**archival, "messages": [{"role": "user", "content": "bad\0prompt"}, {"role": "assistant", "content": "hi"}]}
            assert request(path, "POST", {"revision": 3, "conversation": invalid})[0] == 400
            assert request(path, "POST", {"revision": 3, "conversation": conversation})[0] == 200
            for _ in range(31): assert request(method="POST", body=create)[0] == 200
            assert request(method="POST", body=create)[0] == 409
            assert request(path, "POST", {"revision": 4, "conversation": conversation})[0] == 200
            assert request(path, "DELETE", {"revision": 2})[0] == 409
            assert request(path, "DELETE", {"revision": 5})[1] == {"deleted": True}
            assert request(path)[0] == 404
            assert len(request()[1]["conversations"]) == 31
            # A revoked username recreated by the operator does not inherit
            # the prior credential's private namespace.
            cli("revoke", "alice"); assert request()[0] == 401
            replacement = cli("user-add", "alice")
            assert request(token=replacement)[1]["conversations"] == []
            for entry in (home / ".lumabri/api-access/history").rglob("*"):
                assert entry.stat().st_mode & 0o077 == 0, entry
        finally:
            process.terminate(); process.wait(timeout=10)
            errors = process.stderr.read().decode(); process.stderr.close()
            assert alice not in errors and "private question" not in errors
    print("CHAT HISTORY: PASS (authorization, isolation, revisions, schema, quota, deletion, credential replacement, compiled assets)")


if __name__ == "__main__":
    main()
