"""Real tracker + PTYs: fixed endpoint, restart, discovery and authenticated join.

Only temporary identities/settings are used. No firewall changes are made.
This is loopback/local-interface coverage, not a physical-LAN certification.
"""
import fcntl
import os
from pathlib import Path
import pty
import select
import socket
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[2]


def free_base():
    for base in range(53000, 62000, 16):
        probes = []
        try:
            for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                s = socket.socket(socket.AF_INET, kind)
                probes.append(s)
                s.bind(("0.0.0.0", base))
            return base
        except OSError:
            pass
        finally:
            for s in probes:
                s.close()
    raise AssertionError("no isolated test port")


class Terminal:
    def __init__(self, home, base):
        self.home, self.data = Path(home), bytearray()
        self.home.mkdir(exist_ok=True)
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 120, 0, 0))
        env = {**os.environ, "HOME": str(home), "LUMABRI_HOME_PORT_BASE": str(base),
               "LUMABRI_PEER_KEY": str(self.home / "peer.key"), "LUMABRI_TOKEN": "",
               "LUMABRI_KNOWN_HOSTS": str(self.home / "known.hosts"), "LUMABRI_ENCRYPT": "1"}
        self.p = subprocess.Popen([str(ROOT / "lumabri")], cwd=home, env=env,
                                  stdin=self.slave, stdout=self.slave, stderr=self.slave)

    def drain(self):
        while select.select([self.master], [], [], .02)[0]:
            self.data.extend(os.read(self.master, 65536))

    def expect(self, text, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.drain()
            if text.encode() in self.data:
                return
            if self.p.poll() is not None:
                break
        # Do not print the output: /create includes a household key.
        states = [s for s in ("Connected.", "authentication failed", "secure handshake failed",
                              "Could not save", "Household key from", "Type yes", "Workspace actions")
                  if s.encode() in self.data]
        raise AssertionError(f"missing {text!r}; exit={self.p.poll()}; states={states}")

    def send(self, data):
        self.data.clear()
        os.write(self.master, data)

    def action(self, index):
        self.send(b"/")
        self.expect("Workspace actions")
        self.send(b"\x1b[B" * index + b"\r")

    def settings(self):
        p = self.home / ".lumabri/home.conf"
        return dict(line.split("=", 1) for line in p.read_text().splitlines())

    def quit(self):
        self.send(b"\x1b")
        deadline = time.monotonic() + 8
        while self.p.poll() is None and time.monotonic() < deadline:
            self.drain()
        assert self.p.wait(timeout=2) == 0

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill(); self.p.wait()
        os.close(self.master); os.close(self.slave)


def main():
    terminals = []
    with tempfile.TemporaryDirectory(prefix="lumabri-household-network-") as tmp:
        base = free_base()

        def start(name):
            t = Terminal(Path(tmp) / name, base)
            terminals.append(t)
            t.expect("What would you like to do?")
            return t

        try:
            owner = start("owner")
            owner.action(2)
            owner.expect("Folder containing your model directories")
            model_folder = str(Path(tmp) / "models with spaces")
            Path(model_folder).mkdir()
            owner.send(model_folder.encode() + b"\n")
            owner.expect("Maximum RAM to offer")
            owner.send(b"0.5\n")
            owner.expect("Settings saved.")
            assert owner.settings()["models"] == model_folder
            assert owner.settings()["ram"] == "0.5"
            owner.action(0)
            owner.expect("Host identity:")
            saved = owner.settings()
            assert saved["owner"] == "1" and saved["tracker"].endswith(f":{base}")
            assert saved["token"] and not saved["tracker"].startswith("10.255.255.254:")
            owner.send(b"\n"); owner.expect("What would you like to do?")

            second = start("owner")
            second.expect("Household ready")
            assert second.settings() == saved
            second.quit()  # An attached second window must not kill the first tracker.

            wrong = start("wrong")
            wrong.action(1)
            wrong.expect("Choose your computer")
            wrong.send(b"\r"); wrong.expect("Type yes only")
            wrong.send(b"yes\n"); wrong.expect("Household key from its owner")
            wrong.send(b"incorrect-test-key\n"); wrong.expect("Household authentication failed")
            assert not (wrong.home / ".lumabri/home.conf").exists()
            wrong.data.clear()
            time.sleep(.3)
            wrong.expect("Could not join the household")  # persists until acknowledged
            wrong.send(b"\r"); wrong.expect("What would you like to do?")
            wrong.quit()

            joiner = start("joiner")
            joiner.action(1)
            joiner.expect("Choose your computer")
            joiner.send(b"\r"); joiner.expect("Type yes only")
            joiner.send(b"yes\n"); joiner.expect("Household key from its owner")
            joiner.send(saved["token"].encode() + b"\n")
            joiner.expect("Connected. Open Your computers")
            assert joiner.settings()["token"] == saved["token"]
            assert joiner.settings()["owner"] == "0"
            joiner.quit()
            owner.quit()

            resumed = start("owner")
            resumed.expect("Household ready")
            assert resumed.settings() == saved
            resumed.quit()

            with socket.socket() as blocked:
                blocked.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                blocked.bind(("0.0.0.0", base)); blocked.listen()
                # The listener cannot authenticate; creation must not fall back to another port.
                occupied = start("occupied")
                occupied.action(0)
                occupied.expect("is occupied", seconds=20)
                assert not (occupied.home / ".lumabri/home.conf").exists()
                occupied.quit()
            print("HOUSEHOLD NETWORK: PASS (fixed port/key, restart, second window, discovery, auth rejection, join, occupied port)")
        finally:
            for t in reversed(terminals):
                t.close()


if __name__ == "__main__":
    main()
