"""One-command local start: household, sharing and the loopback page. No model is loaded."""
import http.client
import json
import os
from pathlib import Path
import platform
import socket
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ROLES = ("manager", "tracker", "donor", "api")


def main():
    with tempfile.TemporaryDirectory(prefix="lmb-start-") as temporary:
        home = Path(temporary)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        # A private household port range on loopback: nothing here is offered
        # to the LAN, and a household running on this machine is not touched.
        env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
               "LUMABRI_KNOWN_HOSTS": temporary + "/known", "LUMABRI_ADVERTISE": "127.0.0.1",
               "LUMABRI_HOME_PORT_BASE": str(12000 + (port % 200) * 160),
               "LUMABRI_NO_DISK_PROBE": "1", "LUMABRI_RAM_RESERVE_MB": "256"}
        for name in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND"):
            env.pop(name, None)

        def run(*args, environment=env):
            return subprocess.run([str(ROOT / "lumabri"), *args], env=environment, text=True,
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=90)

        def services():
            answer = run("service", "status", "--json")
            assert answer.returncode == 0, answer.stderr
            return {s["role"]: s for s in json.loads(answer.stdout)["services"]}

        def request(path, token=None):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            conn.request("GET", path, headers={"Authorization": "Bearer " + token} if token else {})
            response = conn.getresponse(); body = response.read(); conn.close()
            return response.status, body

        def link_of(answer):
            lines = [line.strip() for line in answer.stdout.splitlines() if "#token=" in line]
            assert len(lines) == 1, (answer.stdout, answer.stderr)
            return lines[0]

        def private(path):
            return stat.S_IMODE(path.stat().st_mode) == 0o600

        try:
            first = run("start", "--no-browser", "--port", str(port))
            assert first.returncode == 0, (first.stdout, first.stderr)
            link = link_of(first)
            prefix = f"http://127.0.0.1:{port}/#token="
            assert link.startswith(prefix + "local.") and "?" not in link, link
            token = link[len(prefix):]
            assert token not in first.stderr

            status, page = request("/")
            assert status == 200 and b"login-form" in page
            status, body = request("/api/v1/models", token)
            assert status == 200 and json.loads(body) == {"schema": 1, "models": []}, (status, body)
            status, body = request("/api/v1/session", token)
            assert status == 200 and json.loads(body)["management"] is True, (status, body)
            assert request("/api/v1/models")[0] == 401
            assert request("/api/v1/models", "local." + "0" * 64)[0] == 401
            # The gateway is not reachable through a non-loopback address.
            listening = subprocess.run(["ss", "-ltn"], capture_output=True, text=True).stdout \
                if platform.system() == "Linux" else ""
            assert f"0.0.0.0:{port} " not in listening and f"*:{port} " not in listening

            settings = home / ".lumabri/home.conf"
            saved = settings.read_bytes()
            assert private(settings) and b"owner=1\n" in saved and b"tracker=127.0.0.1:" in saved
            access = home / ".lumabri/api-access"
            assert stat.S_IMODE(access.stat().st_mode) == 0o700
            assert private(access / "local.token") and (access / "local.token").read_text() == token
            # Only the one owner-only record holds the token; no log does.
            holders = [p for p in (home / ".lumabri").rglob("*")
                       if p.is_file() and token.encode() in p.read_bytes()]
            assert holders == [access / "local.token"], holders

            before = services()
            assert all(before[role]["live"] for role in ROLES), before

            # A second start adopts every running piece and the same sign-in.
            second = run("start", "--no-browser")
            assert second.returncode == 0, (second.stdout, second.stderr)
            assert link_of(second) == link
            after = services()
            for role in ROLES:
                assert after[role]["live"] and after[role]["instance"] == before[role]["instance"] and \
                    after[role]["pid"] == before[role]["pid"], (role, before[role], after[role])
            assert settings.read_bytes() == saved
            assert sorted(p.name for p in access.glob("*.user")) == ["local.user"]
            assert request("/api/v1/models", token)[0] == 200

            # The opener receives exactly one argument, with the token only in
            # the fragment, and the token is then not printed at all.
            tools = home / "tools"
            tools.mkdir()
            opener = tools / ("open" if platform.system() == "Darwin" else "xdg-open")
            opener.write_text('#!/bin/sh\nprintf "%s\\n" "$#" "$@" > "$HOME/opened"\n'
                              'env | grep "^LUMABRI_TOKEN=" >> "$HOME/opened"\nexit 0\n')
            opener.chmod(0o755)
            third = run("start", environment={**env, "PATH": f"{tools}{os.pathsep}{env.get('PATH', '')}"})
            assert third.returncode == 0, (third.stdout, third.stderr)
            assert (home / "opened").read_text().splitlines() == ["1", link]
            assert token not in third.stdout and token not in third.stderr
            assert services()["api"]["instance"] == before["api"]["instance"]

            # Options are refused before anything is started or changed.
            assert run("start", "--port", "0").returncode == 2
            assert run("start", "--unknown").returncode == 2
            assert run("start", "--tui").returncode == 1  # needs a terminal
        finally:
            stopped = run("service", "stop")
        assert stopped.returncode == 0, stopped.stderr
        assert not any(s["live"] for s in services().values())
    print("lumabri start: PASS")


if __name__ == "__main__":
    main()
