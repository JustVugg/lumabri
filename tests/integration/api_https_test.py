"""Exercise the shipped NGINX recipe with real TLS; no model inference."""
import argparse
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile

from https_proxy_fixture import ROOT, TLSProxy


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nginx", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lmb-tls-", dir="/tmp") as temporary:
        home = Path(temporary).resolve()
        env = {**os.environ, "HOME": str(home), "LUMABRI_PEER_KEY": str(home / "key"),
               "LUMABRI_KNOWN_HOSTS": str(home / "known")}
        env.pop("LUMABRI_HOME_FOREGROUND", None)
        def cli(*args):
            result = subprocess.run([str(ROOT / "lumabri"), *args], env=env, capture_output=True,
                                    text=True, timeout=20)
            assert result.returncode == 0, (args[:2], result.stderr)
            return result.stdout.strip()
        token = cli("api", "user-add", "alice")
        proxy = TLSProxy(home / "tls", home / ".lumabri/api-access/proxy.sock", args.nginx)
        try:
            cli("api", "start", "--tracker", "127.0.0.1:1", "--https-origin", proxy.origin)
            proxy.start()
            untrusted = proxy.connection(trusted=False)
            try:
                untrusted.connect()
                raise AssertionError("untrusted certificate was accepted")
            except ssl.SSLCertVerificationError:
                pass
            finally:
                untrusted.close()
            with socket.create_connection(("127.0.0.1", proxy.port), timeout=5) as cleartext:
                cleartext.sendall(f"GET / HTTP/1.1\r\nHost: {proxy.authority}\r\n\r\n".encode())
                assert b" 400 " in cleartext.recv(4096), "HTTP reached the HTTPS application"
            page = proxy.request("/")
            assert page[0] == 200, (page[0], (home / "tls/nginx-error.log").read_text())
            assert proxy.request("/api/v1/models")[0] == 401
            status, body, headers = proxy.request("/api/v1/models", token)
            assert status == 200 and json.loads(body)["models"] == []
            assert headers["Cache-Control"] == "no-store"
            status, body, _ = proxy.request("/api/v1/client-config", token)
            config = json.loads(body)
            assert status == 200 and config["openai_base_url"] == proxy.origin + "/v1"
            assert config["anthropic_base_url"] == proxy.origin and token not in body.decode()
            status, body, _ = proxy.request("/v1/models", token)
            assert status == 200 and json.loads(body) == {"object": "list", "data": []}
            sample = json.dumps({"model": "01" * 32, "max_tokens": 8, "messages": [{"role": "user", "content": "hi"}]})
            status, body, _ = proxy.request("/v1/messages", body=sample,
                headers={"x-api-key": token, "anthropic-version": "2023-06-01"})
            assert status == 403 and json.loads(body)["error"]["message"] == "model_not_authorized", body
            assert proxy.request("/api/v1/workspace", token)[0] == 403
            for headers in ({"Origin": "https://evil.test"}, {"Origin": "null"},
                            {"Host": "evil.test", "X-Forwarded-Host": proxy.authority},
                            {"Origin": "http://" + proxy.authority, "X-Forwarded-Proto": "https"}):
                assert proxy.request("/api/v1/models", token, headers=headers)[0] == 403
            assert proxy.request("/api/v1/models", token, headers={"Forwarded": "host=evil.test"})[0] == 200
            assert proxy.request("/api/v1/chat", token, body="x" * ((1 << 20) + 1))[0] == 413
            cli("api", "revoke", "alice")
            assert proxy.request("/api/v1/models", token)[0] == 401
        finally:
            proxy.stop()
            cli("service", "stop")
        for log in (home / "tls").glob("*.log"):
            assert token not in log.read_text(), "proxy logged a bearer credential"
    print("API HTTPS: PASS (real NGINX, verified certificate, no cleartext, preserved auth/Origin, bounded body, revoke)")


if __name__ == "__main__":
    main()
