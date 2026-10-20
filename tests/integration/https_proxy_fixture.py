"""Real, unprivileged NGINX + verified local TLS, never a public listener.

No download, system install, CA-store change or real operator credential.
The caller supplies an already available nginx executable and owns the homes.
"""
import http.client
from pathlib import Path
import socket
import ssl
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


class TLSProxy:
    def __init__(self, directory, upstream, nginx):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=False)
        self.process = None
        self.nginx = str(Path(nginx).resolve(strict=True))
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            self.port = reserve.getsockname()[1]
        self.authority = f"localhost:{self.port}"
        self.origin = "https://" + self.authority
        self.cert, self.key = self.directory / "cert.pem", self.directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
            "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout", str(self.key), "-out", str(self.cert)], check=True, capture_output=True, timeout=20)
        self.context = ssl.create_default_context(cafile=str(self.cert))
        assert self.context.check_hostname and self.context.verify_mode == ssl.CERT_REQUIRED
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        config = (ROOT / "deploy/nginx-https.conf.example").read_text()
        for key, value in {"RUNTIME": self.directory, "UPSTREAM": Path(upstream).resolve(),
            "LISTEN": f"127.0.0.1:{self.port}", "SERVER_NAME": "localhost", "AUTHORITY": self.authority,
            "CERTIFICATE": self.cert, "KEY": self.key}.items():
            value = str(value)
            assert not any(c in value for c in ('"', "'", "\n", "\r", "$", ";", "\\"))
            config = config.replace("@" + key + "@", value)
        self.config = self.directory / "nginx.conf"
        self.config.write_text(config)
        self.command = [self.nginx, "-p", str(self.directory) + "/", "-c", str(self.config)]

    def start(self):
        checked = subprocess.run([*self.command, "-t"], capture_output=True, text=True, timeout=10)
        assert checked.returncode == 0, checked.stderr
        self.process = subprocess.Popen(self.command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while True:
            try:
                connection = self.connection(); connection.connect(); connection.close()
                return
            except OSError:
                assert self.process.poll() is None and time.monotonic() < deadline
                time.sleep(.05)

    def connection(self, trusted=True):
        # Connect to an explicit loopback address (no DNS); the certificate
        # contains its IP SAN as well. HTTP uses the configured DNS authority.
        return http.client.HTTPSConnection("127.0.0.1", self.port, timeout=20,
                                          context=self.context if trusted else ssl.create_default_context())

    def request(self, path, token=None, body=None, headers=None):
        connection = self.connection()
        options = {"Host": self.authority, "Origin": self.origin}
        if token is not None:
            options["Authorization"] = "Bearer " + token
        if body is not None:
            options["Content-Type"] = "application/json"
        options.update(headers or {})
        try:
            connection.request("GET" if body is None else "POST", path, body, options)
            response = connection.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            connection.close()

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
