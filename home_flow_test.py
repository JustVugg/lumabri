"""Real encrypted tracker, two donor TUIs, Segment engines and hosted chat.

Requires an actual converted small OLMoE checkpoint (not a mock engine).
This loopback integration gate is not a physical LAN or native-platform test.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--models-dir", required=True)
    args = parser.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="lumabri-home-flow-"))
    children, terminals = [], []
    print(f"Household test logs: {tmp}", flush=True)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    addr = f"127.0.0.1:{port}"

    def env(name):
        home = tmp / name
        home.mkdir(exist_ok=True)
        settings = home / ".lumabri" / "home.conf"
        if not settings.exists():
            settings.parent.mkdir(exist_ok=True)
            settings.write_text(f"tracker={addr}\ntoken=household-test\nmodels={Path(args.models_dir).resolve()}\nram=0.5\n")
            settings.chmod(0o600)
        return {**os.environ, "HOME": str(home), "LUMABRI_ENCRYPT": "1",
                "LUMABRI_PEER_KEY": str(home / "peer.key"),
                "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts"),
                "LUMABRI_TOKEN": "household-test", "LUMABRI_NO_DISK_PROBE": "1",
                "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_IO_TIMEOUT_MS": "10000",
                "OMP_NUM_THREADS": "2", "COLI_NO_OMP_TUNE": "1", "PIN": "off"}

    class Terminal:
        def __init__(self, name, argv):
            self.name, self.text = name, ""
            self.master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 140, 0, 0))
            self.log = open(tmp / f"{name}.terminal.log", "wb")
            self.p = subprocess.Popen(argv, cwd=ROOT, env=env(name),
                                      stdin=slave, stdout=slave, stderr=slave)
            os.close(slave)
            terminals.append(self)
            children.append(self.p)

        def drain(self):
            while select.select([self.master], [], [], 0)[0]:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.log.write(data); self.log.flush()
                self.text += data.decode("utf-8", errors="replace")
                self.text = self.text[-200000:]

        def send(self, keys):
            os.write(self.master, keys.encode())

        def has(self, text):
            self.drain()
            return text in self.text

    def until(check, seconds=30, message="condition timed out"):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for t in terminals:
                t.drain()
            if check():
                return
            time.sleep(.05)
        raise AssertionError(message)

    def engines_started(name):
        return any((tmp / name).rglob("cache"))

    base = ["./lumabri", "models", "--models-dir", str(Path(args.models_dir).resolve()),
            "--tracker", addr, "--context", "128", "--max-new", "8"]

    try:
        with open(tmp / "tracker.log", "wb") as log:
            tracker = subprocess.Popen(["./tracker", "--port", str(port),
                "--peer-bindings", str(tmp / "bindings")], cwd=ROOT,
                env=env("tracker"), stdout=log, stderr=subprocess.STDOUT)
        children.append(tracker)

        def listening():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    return True
            except OSError:
                return False
        until(listening)
        a = Terminal("donor-a", ["./lumabri"])
        b = Terminal("donor-b", ["./lumabri"])
        until(lambda: a.has("YOUR COMPUTERS") and b.has("YOUR COMPUTERS"))
        a.send("d"); b.send("d")
        until(lambda: a.has("Available") and b.has("Available"))

        def inventory_ready():
            p = subprocess.run(base + ["--json"], cwd=ROOT, env=env("observer"),
                               capture_output=True, text=True, timeout=15)
            return p.returncode == 0 and len(json.loads(p.stdout)["nodes"]) == 3
        until(inventory_ready)
        reject = Terminal("reject", base)
        until(lambda: reject.has("3 computers"))
        reject.send("\t")
        until(lambda: reject.has("CORES/THREADS"))
        reject.send("j j \t")
        time.sleep(.5)
        reject.send("c")
        until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"),
              seconds=60, message="offers never reached both donor TUIs")
        assert not engines_started("donor-a") and not engines_started("donor-b")
        a.send("y")
        until(lambda: a.has("Accepted; waiting"))
        assert not engines_started("donor-a") and not engines_started("donor-b")
        b.send("n")
        until(lambda: reject.p.poll() is not None, message="rejection did not cancel the whole plan")
        assert reject.p.returncode != 0
        assert not engines_started("donor-a") and not engines_started("donor-b")
        until(lambda: a.has("Released") and b.has("Released"))
        a.text = b.text = ""
        chat = Terminal("chatter", base)
        until(lambda: chat.has("3 computers"))
        chat.send("\t")
        until(lambda: chat.has("CORES/THREADS"))
        chat.send("j j \t")
        time.sleep(.5)
        chat.send("c")
        until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"), seconds=60)
        a.send("y"); b.send("y")
        until(lambda: chat.has("receives the text") or chat.p.poll() is not None, seconds=180,
              message="accepted plan did not reach real hosted chat")
        assert chat.p.poll() is None, "accepted plan failed; inspect donor engine logs"
        chat.send("/he")
        until(lambda: chat.has("List chat commands"), message="slash suggestions did not appear")
        chat.send("\t\n")
        until(lambda: chat.has("Tab completes commands"), message="slash completion did not execute help")
        chat.send("hi\n")
        until(lambda: chat.has("tok/s") or chat.p.poll() is not None, seconds=120,
              message="real model did not finish a response")
        assert chat.has("tok/s"), "engine failed during generation"
        assert "hosted stream · no local checkpoint" in chat.text
        chat.send("/quit\n")
        until(lambda: chat.p.poll() is not None, message="quit left hosted chat blocked")
        assert chat.p.returncode == 0
        until(lambda: a.has("Released") and b.has("Released"), message="donor leases were not released")
        def leases_released():
            for name in ("donor-a", "donor-b"):
                with open(tmp / name / ".lumabri" / "compute-donor.lock", "r") as lease:
                    try:
                        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        return False
            return True
        until(leases_released, message="a child retained a donor resource lease after closing chat")
        for name in ("chatter", "reject"):
            assert not list((tmp / name).rglob("*.safetensors")), "thin client downloaded weights"
            assert not list((tmp / name).rglob("vroot")), "thin client mounted a checkpoint"
        previous_a = a.text.count("YOUR COMPUTERS")
        previous_b = b.text.count("YOUR COMPUTERS")
        a.send("q"); b.send("q")
        until(lambda: a.text.count("YOUR COMPUTERS") > previous_a and b.text.count("YOUR COMPUTERS") > previous_b)
        a.send("q"); b.send("q")
        until(lambda: a.p.poll() is not None and b.p.poll() is not None)
        print("HOME FLOW: PASS (all-party consent, rejection, real Segment load, hosted generation, cleanup)", flush=True)
    finally:
        for p in reversed(children):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in reversed(children):
            try:
                p.wait(timeout=8)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait()
        for t in terminals:
            t.drain(); t.log.close(); os.close(t.master)


if __name__ == "__main__":
    main()
