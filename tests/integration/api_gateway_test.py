"""Bounded loopback/authentication gate. No engines, checkpoint or LAN needed."""
import argparse
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("./lumabri"))
    args = parser.parse_args(); binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="lmb-api-gate-") as temporary:
        env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
               "LUMABRI_KNOWN_HOSTS": temporary + "/known", "LUMABRI_ENCRYPT": "1"}
        def cli(*command):
            return subprocess.run([str(binary), "api", *command], env=env, capture_output=True, text=True, timeout=10)
        added = cli("user-add", "alice"); assert added.returncode == 0, added.stderr
        token = added.stdout.strip(); assert token.startswith("alice.") and len(token) == 70
        assert cli("user-add", "../other").returncode != 0
        assert cli("user-add", "alice").returncode != 0
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        server = subprocess.Popen([str(binary), "api", "serve", "--tracker", "127.0.0.1:1", "--port", str(port)],
                                  env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        held = []
        def request(token_value=token, method="GET", path="/api/v1/models", body=None, headers=None):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            options = {"Authorization": "Bearer " + token_value}
            options.update(headers or {})
            conn.request(method, path, body, options)
            response = conn.getresponse(); answer = (response.status, response.read())
            conn.close(); return answer
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.2):
                        break
                except OSError:
                    assert server.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            assert request("invalid")[0] == 401
            status, body = request(); assert status == 200 and json.loads(body) == {"schema": 1, "models": []}
            assert request(headers={"Origin": "http://evil.test"})[0] == 403
            assert request(headers={"Host": "evil.test"})[0] == 403
            assert request(method="POST", path="/api/v1/chat", body='{}')[0] == 400
            assert request(method="POST", path="/api/v1/chat", body='{}', headers={"Content-Type": "application/json"})[0] == 400
            unauthorized = json.dumps({"model": "01" * 32, "messages": [{"role": "user", "content": "not sent"}], "max_tokens": 1})
            assert request(method="POST", path="/api/v1/chat", body=unauthorized, headers={"Content-Type": "application/json"})[0] == 403
            for framing in ("Content-Length: 0\r\nContent-Length: 0\r\n", "Transfer-Encoding: chunked\r\n"):
                with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                    wire = f"POST /api/v1/chat HTTP/1.1\r\nHost: localhost:{port}\r\n{framing}\r\n"
                    sock.sendall(wire.encode()); assert b" 400 " in sock.recv(4096)
            # Hold two authenticated bodies open; a third is rejected before
            # it can reserve host slots. Processes/locks release on disconnect.
            for _ in range(2):
                sock = socket.create_connection(("127.0.0.1", port), timeout=5); held.append(sock)
                sock.sendall((f"POST /api/v1/chat HTTP/1.1\r\nHost: localhost:{port}\r\nAuthorization: Bearer {token}\r\n"
                              "Content-Type: application/json\r\nContent-Length: 100\r\n\r\n{").encode())
                time.sleep(.1)
            assert request()[0] == 429
            for sock in held:
                sock.close()
            held.clear()
            deadline = time.monotonic() + 3
            while request()[0] != 200:
                assert time.monotonic() < deadline
                time.sleep(.05)
            assert cli("revoke", "alice").returncode == 0
            assert request()[0] == 401
        finally:
            for sock in held:
                sock.close()
            server.terminate(); server.wait(timeout=10)
            errors = server.stderr.read().decode(); server.stderr.close()
            assert token not in errors
    print("API GATEWAY: PASS (loopback, credentials, framing, origin, model grants, bounded users, disconnect and revoke)")


if __name__ == "__main__":
    main()
