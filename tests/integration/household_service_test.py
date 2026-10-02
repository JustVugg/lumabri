"""Private manager lifecycle, independent of model/hardware availability."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="lmb-service-") as temporary:
        home = Path(temporary)
        env = {**os.environ, "HOME": temporary, "LUMABRI_ENCRYPT": "1",
               "LUMABRI_PEER_KEY": str(home / "peer.key"),
               "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts")}
        env.pop("LUMABRI_HOME_FOREGROUND", None)

        def run(*args, check=True):
            return subprocess.run([str(ROOT / "lumabri"), "service", *args], env=env,
                                  text=True, capture_output=True, timeout=20, check=check)

        def manager():
            return json.loads(run("status", "--json").stdout)["services"][0]

        try:
            assert not manager()["live"]
            run("start")
            initial = manager()
            assert initial["live"] and initial["pid"] > 0, initial
            # Multiple independent clients never create another manager.
            procs = [subprocess.Popen([str(ROOT / "lumabri"), "service", "start"],
                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                     for _ in range(4)]
            for p in procs:
                p.communicate(timeout=20)
                assert p.returncode == 0
            assert manager()["instance"] == initial["instance"]
            directory = home / ".lumabri/service"
            assert directory.stat().st_mode & 0o777 == 0o700
            assert (directory / "manager.sock").stat().st_mode & 0o777 == 0o600
            assert (directory / "manager.state").stat().st_mode & 0o777 == 0o600
            # Malformed/stale stop cannot terminate the service.
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(directory / "manager.sock"))
                client.settimeout(2)
                client.sendall(struct.pack("<II", 1, 1) + bytes(72))
                assert client.recv(1) == b""
            assert manager()["instance"] == initial["instance"]
            # A partial frame has an absolute deadline and cannot monopolize IPC.
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(directory / "manager.sock"))
                client.sendall(b"\x01")
                time.sleep(.4)
                assert manager()["live"]
            run("restart")
            restarted = manager()
            assert restarted["live"] and restarted["instance"] != initial["instance"]
            # Only a PID just authenticated by this test is signalled.
            os.kill(restarted["pid"], signal.SIGKILL)
            for _ in range(40):
                if not manager()["live"]:
                    break
                time.sleep(.05)
            assert manager()["state"] == "interrupted"
            run("start")
            assert manager()["live"]
            run("stop")
            assert manager()["state"] == "stopped"
            # Unsafe service directories fail closed instead of chmod/adoption.
            directory.chmod(0o777)
            assert run("start", check=False).returncode != 0
            directory.chmod(0o700)
        finally:
            if (home / ".lumabri/service").exists():
                (home / ".lumabri/service").chmod(0o700)
            run("stop", check=False)
    print("HOUSEHOLD SERVICE: PASS (singleton, private IPC, fencing, bounded input, restart and crash reconciliation)")


if __name__ == "__main__":
    main()
