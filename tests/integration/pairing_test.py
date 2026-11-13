"""Adding a computer with a short code, with the real commands on loopback.

One owner HOME runs `lumabri start` and `lumabri pair`; other HOMEs run
`lumabri join --code`. Nothing is offered to the LAN beyond a discovery reply
on a private port range, no model is loaded, and every process started here is
stopped through its own HOME.

Attempts are spaced, because the tracker limits them per source address and
every HOME here is 127.0.0.1. No setting can relax that limit; the only test
knob is LUMABRI_PAIR_LIFETIME_S, which can shorten a code's life, never extend
it.
"""
import fcntl
import os
from pathlib import Path
import pty
import queue
import re
import select
import socket
import stat
import struct
import subprocess
import tempfile
import termios
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
CODE = re.compile(r"\b([0-9A-HJKMNP-TV-Z]{4})-([0-9A-HJKMNP-TV-Z]{4})\b")
REFILL = 2.3   # seconds: a little more than the tracker's refill interval per source


def free_base():
    for base in range(40000, 46000, 16):
        probes = []
        try:
            for offset in range(16):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                probes.append(s); s.bind(("0.0.0.0", base + offset))
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probes.append(s); s.bind(("0.0.0.0", base))
            return base
        except OSError:
            pass
        finally:
            for s in probes:
                s.close()
    raise AssertionError("no isolated household port range")


class Pairing:
    """`lumabri pair`, read line by line with deadlines."""

    def __init__(self, env):
        self.process = subprocess.Popen([str(ROOT / "lumabri"), "pair"], env=env, text=True,
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.lines, self.seen = queue.Queue(), []
        threading.Thread(target=self._read, daemon=True).start()
        code = CODE.search(self.wait_for("Code:"))
        assert code, self.seen
        self.shown, self.code = code.group(0), code.group(1) + code.group(2)

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def wait_for(self, text, seconds=20):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                break
            self.seen.append(line)
            if text in line:
                return line
        raise AssertionError(f"lumabri pair never printed {text!r}")   # its output holds a code: not shown

    def finish(self, seconds=20):
        code = self.process.wait(timeout=seconds)
        self.process.stdout.close(); self.process.stderr.close()
        return code

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()


class Workspace:
    """The terminal workspace on a PTY, for the join that types the full key."""

    def __init__(self, env):
        self.data = bytearray()
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 120, 0, 0))
        self.process = subprocess.Popen([str(ROOT / "lumabri")], env=env, cwd=env["HOME"],
                                        stdin=self.slave, stdout=self.slave, stderr=self.slave)

    def expect(self, text, seconds=20):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            while select.select([self.master], [], [], .02)[0]:
                self.data.extend(os.read(self.master, 65536))
            if text.encode() in self.data:
                return
            if self.process.poll() is not None:
                break
        raise AssertionError(f"the workspace never showed {text!r}")   # its output may hold a key

    def send(self, data):
        self.data.clear()
        os.write(self.master, data)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait()
        os.close(self.master); os.close(self.slave)


def main():
    with tempfile.TemporaryDirectory(prefix="lmb-pairing-") as temporary:
        root, base = Path(temporary), free_base()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); page = reservation.getsockname()[1]
        address = f"127.0.0.1:{base}"

        def environment(name, **extra):
            home = root / name
            home.mkdir(exist_ok=True)
            env = {**os.environ, "HOME": str(home), "LUMABRI_PEER_KEY": str(home / "peer.key"),
                   "LUMABRI_KNOWN_HOSTS": str(home / "known"), "LUMABRI_HOME_PORT_BASE": str(base),
                   "LUMABRI_NO_DISK_PROBE": "1", "LUMABRI_RAM_RESERVE_MB": "256", **extra}
            for variable in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND", "LUMABRI_PEER_PINS",
                             "LUMABRI_REQUIRE_PIN", "LUMABRI_ADVERTISE", "LUMABRI_PAIR_LIFETIME_S"):
                if variable not in extra:
                    env.pop(variable, None)
            return env

        def run(env, *args, seconds=90):
            return subprocess.run([str(ROOT / "lumabri"), *args], env=env, text=True,
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=seconds)

        def settings(name):
            path = root / name / ".lumabri/home.conf"
            return dict(line.split("=", 1) for line in path.read_text().splitlines()) if path.exists() else None

        attempts = [0]

        def join(name, code, *where):
            # The first few attempts fit the tracker's burst; later ones wait for a refill.
            attempts[0] += 1
            if attempts[0] > 4:
                time.sleep(REFILL)
            return run(environment(name), "join", "--code", code, *where, seconds=40)

        def probe(name, token):
            env = environment(name, LUMABRI_ENCRYPT="1", LUMABRI_TOKEN=token)
            return subprocess.run([str(ROOT / "swarm_probe"), "--tracker", address], env=env, text=True,
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=20).returncode

        owner = environment("owner", LUMABRI_ADVERTISE="127.0.0.1")
        codes, pairing, workspace = [], None, None
        try:
            # Before any household exists there is nothing to pair with.
            refused = run(owner, "pair")
            assert refused.returncode == 1 and "does not own a household" in refused.stderr, refused.stderr
            assert run(owner, "pair", "--anything").returncode == 2
            assert run(environment("first"), "join").returncode == 2
            assert run(environment("first"), "join", "--code", "7K3M-9QX").returncode == 1   # not a code: nothing sent

            started = run(owner, "start", "--no-browser", "--port", str(page))
            assert started.returncode == 0, (started.stdout, started.stderr)
            household = settings("owner")
            assert household["owner"] == "1" and household["tracker"] == address
            key = household["token"]
            assert len(key) == 32
            identity = run(owner, "peer-key").stdout.strip()
            assert len(identity) == 64

            # ---- one code: a wrong guess is counted, the right code joins once ----
            pairing = Pairing(owner); codes.append(pairing)
            wrong = pairing.code[:-1] + ("7" if pairing.code[-1] != "7" else "8")
            miss = join("first", wrong, address)
            assert miss.returncode == 1 and "Pairing failed" in miss.stderr, (miss.stdout, miss.stderr)
            assert settings("first") is None and not (root / "first/known").exists()
            assert "(1 of 5)" in pairing.wait_for("A wrong code was tried from 127.0.0.1")

            # No address: the household started by `lumabri start` answers discovery.
            joined = join("first", pairing.shown.lower())
            assert joined.returncode == 0, (joined.stdout, joined.stderr)
            assert f"Joined the household at {address}" in joined.stdout and key not in joined.stdout + joined.stderr
            pairing.wait_for("A computer joined from 127.0.0.1")
            assert pairing.finish() == 0
            member = settings("first")
            assert member["tracker"] == address and member["token"] == key and member["owner"] == "0"
            assert stat.S_IMODE((root / "first/.lumabri/home.conf").stat().st_mode) == 0o600
            assert (root / "first/known").read_text() == f"{address} {identity}\n"
            # The saved settings authenticate to the tracker; another key does not.
            assert probe("first", member["token"]) == 0
            assert probe("first", "0" * 32) != 0
            # The product path on the joined computer: it shares with that household.
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0)); second_page = reservation.getsockname()[1]
            sharing = run(environment("first"), "start", "--no-browser", "--port", str(second_page))
            try:
                assert sharing.returncode == 0 and "(joined earlier)" in sharing.stdout, (sharing.stdout, sharing.stderr)
            finally:
                assert run(environment("first"), "service", "stop").returncode == 0

            # A used code cannot be used again.
            again = join("second", pairing.code, address)
            assert again.returncode == 1 and "Pairing failed" in again.stderr and settings("second") is None

            # ---- an expired code ----
            pairing = Pairing({**owner, "LUMABRI_PAIR_LIFETIME_S": "2"}); codes.append(pairing)
            pairing.wait_for("The code expired unused")
            assert pairing.finish() == 1
            late = join("second", pairing.code, address)
            assert late.returncode == 1 and "Pairing failed" in late.stderr and settings("second") is None
            # The variable cannot lengthen a code's life.
            pairing = Pairing({**owner, "LUMABRI_PAIR_LIFETIME_S": "86400"}); codes.append(pairing)
            assert "Valid for 300 seconds" in "\n".join(pairing.seen + [pairing.wait_for("Valid for")])
            pairing.process.send_signal(2)                      # Ctrl-C withdraws it
            pairing.wait_for("The code was withdrawn")
            assert pairing.finish() == 130
            gone = join("second", pairing.code, address)
            assert gone.returncode == 1 and "Pairing failed" in gone.stderr

            # ---- five wrong attempts destroy the code; the sixth finds nothing ----
            pairing = Pairing(owner); codes.append(pairing)
            for count in range(1, 6):
                guess = pairing.code[:3] + ("ABCDE"[count - 1] if pairing.code[3] != "ABCDE"[count - 1] else "F") + \
                    pairing.code[4:]
                miss = join("second", guess, address)
                assert miss.returncode == 1 and "Pairing failed" in miss.stderr, (count, miss.stderr)
                if count < 5:
                    assert f"({count} of 5)" in pairing.wait_for("A wrong code was tried")
            pairing.wait_for("destroyed after 5 wrong attempts")
            assert pairing.finish() == 1
            sixth = join("second", pairing.code, address)       # the right code, too late
            assert sixth.returncode == 1 and "Pairing failed" in sixth.stderr and settings("second") is None
            assert probe("second", "0" * 32) != 0

            # ---- only the owner's computer creates codes ----
            member_pair = run(environment("first"), "pair")
            assert member_pair.returncode == 1 and "does not own a household" in member_pair.stderr

            # ---- the full household key still joins, in the workspace ----
            workspace = Workspace(environment("third", LUMABRI_ENCRYPT="1", LUMABRI_TOKEN=""))
            workspace.expect("What would you like to do?")
            workspace.send(b"/"); workspace.expect("Workspace actions")
            workspace.send(b"\x1b[B\r"); workspace.expect("Choose your computer")
            workspace.send(b"\r"); workspace.expect("Pairing code shown on the owner's computer")
            workspace.send(b"\n"); workspace.expect("Type yes only")
            workspace.send(b"yes\n"); workspace.expect("Household key from its owner")
            workspace.send(key.encode() + b"\n"); workspace.expect("Connected. Open Your computers")
            assert settings("third")["token"] == key and settings("third")["owner"] == "0"
            workspace.send(b"\x1b"); workspace.process.wait(timeout=10)

            # No code and no household key reached a log or any file but the settings.
            secrets_seen = [p.code for p in codes] + [p.shown for p in codes]
            for path in root.rglob("*"):
                if not path.is_file():
                    continue
                data = path.read_bytes()
                for secret in secrets_seen:
                    assert secret.encode() not in data, f"a pairing code was written to {path.name}"
                if key.encode() in data:
                    assert path.name == "home.conf", f"the household key was written to {path.name}"
            log = (root / "owner/.lumabri/home-tracker.log")
            if not log.exists():
                log = next((root / "owner/.lumabri").rglob("tracker.log"))
            text = log.read_text()
            assert "pairing code created" in text and "pairing code destroyed after 5 failed attempts" in text
            assert "pairing code used by identity" in text
        finally:
            for running in codes:
                running.stop()
            if workspace:
                workspace.close()
            for name in ("first", "second", "third"):
                if (root / name / ".lumabri/service").exists():
                    run(environment(name), "service", "stop")
            stopped = run(owner, "service", "stop")
        assert stopped.returncode == 0, stopped.stderr
    print("pairing: PASS (code joins once; wrong, used, expired, withdrawn and destroyed codes fail; "
          "full key still joins)")


if __name__ == "__main__":
    main()
