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
            assert request(path="/api/v1/workspace")[0] == 403
            assert request(path="/api/v1/demand")[0] == 403
            assert request(path="/api/v1/elastic-policy")[0] == 403
            assert request(path="/api/v1/capacity")[0] == 403
            assert json.loads(request(path="/api/v1/session")[1])["operator"] is False
            assert cli("operator-grant", "alice").returncode == 0
            status, body = request(path="/api/v1/demand")
            demand = json.loads(body)
            assert status == 200 and demand["models"] == [] and not demand["window_complete"]
            assert demand["scope"] == "this_gateway_authorized_chat_only" and demand["telemetry_gaps"] == 0, demand
            status, body = request(path="/api/v1/elastic-policy")
            assert status == 200 and json.loads(body) == {"schema": 1, "policies": []}
            status, body = request(path="/api/v1/capacity")
            assert status == 200 and json.loads(body) == {"schema": 1, "scope": "historical_not_revalidated", "records": []}
            status, body = request(path="/api/v1/workspace")
            workspace = json.loads(body)
            assert status == 200 and workspace["inventory_ok"] is False and workspace["nodes"] == []
            assert workspace["registry_ok"] and workspace["models"] == [] and workspace["allocations"] == []
            assert workspace["demand"]["instance"] == demand["instance"]
            management_path = "/api/v1/capacity/apply"
            json_headers = {"Content-Type": "application/json"}
            assert request(method="POST", path=management_path, body="{}", headers=json_headers)[0] == 403
            assert request(method="POST", path="/api/v1/elastic-policy", body="{}", headers=json_headers)[0] == 403
            capacity_paths = ["/api/v1/capacity/" + action for action in ("measure", "check", "select", "remove")]
            for path in capacity_paths:
                assert request(method="POST", path=path, body="{}", headers=json_headers)[0] == 403
            assert cli("manage-grant", "alice").returncode == 0
            valid_capacity = {"record": "sample", "clients": 1, "rounds": 3, "max_tokens": 8, "allocations": ["01" * 32]}
            invalid_capacity = ["{}", '{"record":"one","record":"two"}',
                json.dumps({**valid_capacity, "endpoint": "http://example.invalid"}),
                json.dumps({**valid_capacity, "record": "../escape"}),
                json.dumps({**valid_capacity, "record": "probe.lock"}),
                json.dumps({**valid_capacity, "clients": True}),
                json.dumps({**valid_capacity, "clients": 9}),
                json.dumps({**valid_capacity, "rounds": 17}),
                json.dumps({**valid_capacity, "rounds": -1}),
                json.dumps({**valid_capacity, "max_tokens": 1}),
                json.dumps({**valid_capacity, "allocations": []}),
                json.dumps({**valid_capacity, "allocations": ["00" * 32]}),
                json.dumps({**valid_capacity, "clients": 2, "allocations": ["ab" * 32, "AB" * 32]}),
                json.dumps({**valid_capacity, "allocations": ["01" * 32, "02" * 32]})]
            for invalid in invalid_capacity:
                assert request(method="POST", path=capacity_paths[0], body=invalid, headers=json_headers)[0] == 400, invalid
            for path in capacity_paths:
                assert request(method="POST", path=path, body="{}", headers=json_headers)[0] == 400
                assert request(method="POST", path=path, body=" " * 2049, headers=json_headers)[0] == 413
            for invalid in ({"ttft_ms": 1, "gap_ms": 1, "max_age_seconds": 1, "records": ["a", "a"]},
                            {"ttft_ms": 0, "gap_ms": 1, "max_age_seconds": 1, "records": ["a", "b"]}):
                assert request(method="POST", path=capacity_paths[2], body=json.dumps(invalid), headers=json_headers)[0] == 400
            assert request(method="POST", path=capacity_paths[0], body=json.dumps(valid_capacity), headers=json_headers)[0] == 409
            assert json.loads(request(path="/api/v1/capacity")[1])["records"] == []
            for invalid in ('{}', '{"action":"disable","id":"' + '01' * 32 + '","revision":true}',
                            '{"action":"resume","id":"' + '01' * 32 + '","revision":1,"nodes":[]}'):
                assert request(method="POST", path="/api/v1/elastic-policy", body=invalid, headers=json_headers)[0] == 400
            assert request(method="POST", path="/api/v1/elastic-policy", body=" " * 8193, headers=json_headers)[0] == 413
            # Rejection arrives before the body: the request may already be
            # in flight in another TCP packet. The close must preserve the
            # HTTP response, not reset it. A second pipelined command is only
            # discarded; it must never be executed as another request.
            for _ in range(8):
                with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                    header = (f"POST /api/v1/elastic-policy HTTP/1.1\r\nHost: localhost:{port}\r\n"
                              f"Authorization: Bearer {token}\r\nContent-Type: application/json\r\n"
                              "Content-Length: 8193\r\n\r\n")
                    sock.sendall(header.encode())
                    response = http.client.HTTPResponse(sock); response.begin()
                    assert response.status == 413
                    sock.sendall(b" " * 8193 + b"GET /api/v1/models HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    sock.shutdown(socket.SHUT_WR)
                    assert json.loads(response.read())["error"] == "management_request_too_large"
                    response.close()
                    assert sock.recv(1) == b"", "rejection close reset or parsed trailing bytes"
            # A client that never finishes sending cannot hold cleanup open.
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.sendall(header.encode())
                started = time.monotonic()
                response = http.client.HTTPResponse(sock); response.begin()
                assert response.status == 413
                response.read(); response.close()
                assert sock.recv(1) == b"" and time.monotonic() - started < 2
            for invalid in ('{}', '{"record":"one","record":"two"}',
                            '{"record":"../escape","ttft_ms":1,"gap_ms":1,"max_age_seconds":1,"models":[]}',
                            '{"models":[],"endpoint":"http://example.invalid"}'):
                assert request(method="POST", path=management_path, body=invalid, headers=json_headers)[0] == 400
            valid_shape = {"record": "not-present", "ttft_ms": 1, "gap_ms": 1, "max_age_seconds": 1,
                           "models": [{"id": "01" * 32, "revision": 1}]}
            for invalid_model in ({"id": "not-an-id", "revision": 1}, {"id": "01" * 32, "revision": 0},
                                  {"id": "01" * 32, "revision": 4294967295},
                                  {"id": "01" * 32, "revision": 1, "endpoint": "forbidden"}):
                invalid = json.dumps({**valid_shape, "models": [invalid_model]})
                assert request(method="POST", path=management_path, body=invalid, headers=json_headers)[0] == 400
            assert request(method="POST", path=management_path, body=" " * 2049, headers=json_headers)[0] == 413
            assert cli("manage-revoke", "alice").returncode == 0
            assert cli("operator-revoke", "alice").returncode == 0
            assert request(path="/api/v1/workspace")[0] == 403
            assert request(path="/api/v1/demand")[0] == 403
            assert request(path="/api/v1/capacity")[0] == 403
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
    print("API GATEWAY: PASS (loopback, credentials, framing, bounded early-rejection close, origin, model grants, bounded users, disconnect and revoke)")


if __name__ == "__main__":
    main()
