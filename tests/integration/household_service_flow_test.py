"""Real Segment engines across service/TUI lifetimes (loopback, not LAN)."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import random
import select
import signal
import socket
import struct
import subprocess
import tempfile
import termios
import time

from home_flow_test import TerminalText, hosted_turn_complete


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--models-dir", required=True, type=Path)
    parser.add_argument("--runtime-dir", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--keep-requester", action="store_true", help="also verify the normal post-preparation chat and saved calibration")
    args = parser.parse_args()
    runtime = args.runtime_dir.resolve()
    tmp = Path(tempfile.mkdtemp(prefix="lmb-service-flow-"))
    print(f"Artifacts: {tmp}", flush=True)
    terminals = []
    slots = {"owner": 0, "a": 1, "b": 2, "chatter": 3}
    for _ in range(100):
        base = random.randrange(20000, 55000, 100)
        sockets = []
        try:
            for port in range(base, base + 64):
                sock = socket.socket(); sockets.append(sock)
                sock.bind(("127.0.0.1", port))
            break
        except OSError:
            continue
        finally:
            for sock in sockets:
                sock.close()
    else:
        raise AssertionError("no isolated port range")
    tracker = f"127.0.0.1:{base}"

    def env(name):
        home = tmp / name; (home / ".lumabri").mkdir(parents=True, exist_ok=True)
        settings = home / ".lumabri/home.conf"
        if not settings.exists():
            settings.write_text(f"tracker={tracker}\ntoken=service-test\nmodels={args.models_dir.resolve()}\nram=0.5\nowner={int(name == 'owner')}\n")
            settings.chmod(0o600)
        e = {**os.environ, "HOME": str(home), "LUMABRI_TOKEN": "service-test",
             "LUMABRI_ENCRYPT": "1", "LUMABRI_RESIDENT_REQUIRED": "1", "LUMABRI_HOME_HYBRID": "0",
             "LUMABRI_ADVERTISE": "127.0.0.1", "LUMABRI_HOME_PORT_BASE": str(base + 16 * slots[name]),
             "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_NO_DISK_PROBE": "1",
             "LUMABRI_PEER_KEY": str(home / "peer.key"), "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts"),
             "LUMABRI_IO_TIMEOUT_MS": "2000", "COLI_NO_OMP_TUNE": "1", "OMP_NUM_THREADS": "2", "PIN": "off"}
        e.pop("LUMABRI_HOME_FOREGROUND", None)
        return e

    class Terminal(TerminalText):
        def __init__(self, name, argv=None):
            super().__init__()
            self.master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
            self.log = open(tmp / f"{name}-{len(terminals)}.terminal.log", "wb")
            self.p = subprocess.Popen([str(runtime / "lumabri"), *(argv or [])], cwd=runtime, env=env(name),
                                      stdin=slave, stdout=slave, stderr=slave)
            os.close(slave); terminals.append(self)

        def drain(self):
            while select.select([self.master], [], [], 0)[0]:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.log.write(data); self.log.flush(); self.feed(data)

        def has(self, text):
            self.drain(); return text in self.text

        def send(self, text):
            os.write(self.master, text.encode())

    def until(check, message, seconds=45):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for t in terminals:
                t.drain()
            if check():
                return
            time.sleep(.1)
        raise AssertionError(message)

    def service(name, action="status"):
        p = subprocess.run([str(runtime / "lumabri"), "service", action, *( ["--json"] if action == "status" else [])],
                           env=env(name), cwd=runtime, text=True, capture_output=True, timeout=25)
        assert p.returncode == 0, (name, action, p.stderr)
        return {s["role"]: s for s in json.loads(p.stdout)["services"]} if action == "status" else None

    def sharing(name):
        t = Terminal(name)
        until(lambda: t.has("your workspace"), "workspace missing")
        t.send("\x1b[B" * 3 + "\r")
        until(lambda: t.has("share resources"), "sharing client missing")
        return t

    try:
        owner = Terminal("owner")
        until(lambda: owner.has("your workspace"), "household did not start")
        owner.send("\x1b")
        until(lambda: owner.p.poll() is not None, "owner TUI did not exit")
        original_tracker = service("owner")["tracker"]
        assert original_tracker["live"], original_tracker
        a, b = sharing("a"), sharing("b")
        base_args = ["models", "--models-dir", str(args.models_dir.resolve()), "--tracker", tracker,
                     "--context", "128", "--max-new", "8"]
        chat = Terminal("chatter", base_args)
        until(lambda: chat.has("3 computers"), "signed inventory missing")
        chat.send("\t\x1b[B\r\x1b[B\r\t")
        time.sleep(.7); chat.send("\r")
        until(lambda: chat.has("Plan: resident"), "resident preview missing")
        chat.send("\r")
        until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"), "offers not delivered", 150)
        pending = service("a")["donor"]
        assert pending["phase"] == 1 and not pending["segment_pid"]
        # Killing the requester interface must not kill source/preparation.
        if not args.keep_requester:
            chat.p.terminate(); until(lambda: chat.p.poll() is not None, "requester UI did not terminate")
        a.send("\x1b")
        until(lambda: a.has("your workspace"), "donor did not detach")
        a.send("\x1b"); until(lambda: a.p.poll() is not None, "donor TUI did not close")
        service("a", "restart")
        current = service("a")["donor"]
        assert current["instance"] == pending["instance"] and current["phase"] == 1
        assert not current["segment_pid"], "reconciliation approved an old offer"
        a = sharing("a")
        until(lambda: a.has("Waiting for your approval"), "pending approval did not survive reconnect")
        a.send("\x1b[A\r"); b.send("\x1b[A\r")
        until(lambda: service("chatter")["prepare"]["state"] == "stopped", "preparation did not terminate", 180)
        record = tmp / "chatter/.lumabri/resident-plan"
        assert record.exists(), (tmp / "chatter/.lumabri/service/prepare.log").read_text()[-5000:]
        if args.keep_requester:
            until(lambda: chat.has("receives the text"), "normal prepared chat did not start")
            chat.send("hi\n")
            until(lambda: hosted_turn_complete(chat.text), "normal prepared chat did not generate", 120)
            assert list((tmp / "chatter").rglob("*.cal")), "service preparation lost the content/calibration key"
            chat.send("/quit\n")
            until(lambda: chat.p.poll() is not None, "requester chat did not close")
        before = {name: service(name)["donor"] for name in ("a", "b")}
        assert all(x["segment_pid"] and x["phase"] in (4, 6) for x in before.values()), before
        for t in (a, b):
            t.p.terminate(); until(lambda: t.p.poll() is not None, "donor UI did not terminate")
        for name in ("owner", "a", "b", "chatter"):
            service(name, "restart")
        assert service("owner")["tracker"]["instance"] == original_tracker["instance"]
        for name, old in before.items():
            current = service(name)["donor"]
            assert (current["instance"], current["segment_pid"], current["host_pid"]) == (old["instance"], old["segment_pid"], old["host_pid"])
        checker = runtime / "test_chat_ui"
        if checker.exists():
            checked = subprocess.run([str(checker), "resident-deny-stale", str(record), tracker],
                                     env=env("chatter"), capture_output=True, text=True, timeout=20)
            assert checked.returncode == 0, checked.stderr
        # Crash the manager (not the allocation keeper), reconcile without a
        # second approval, reservation, weight load or different engine PID.
        manager = service("a")["manager"]
        os.kill(manager["pid"], signal.SIGKILL)
        service("a", "start")
        assert service("a")["donor"]["segment_pid"] == before["a"]["segment_pid"]
        resumed = Terminal("chatter")
        until(lambda: resumed.has("New conversation · retained model"), "retained entry missing")
        resumed.send("\r")
        until(lambda: resumed.has("receives the text"), "retained host did not answer")
        resumed.send("hi\n")
        until(lambda: hosted_turn_complete(resumed.text), "real generation failed after all TUI/manager restarts", 120)
        resumed.text = ""; resumed.send("/quit\n")
        until(lambda: resumed.has("your workspace"), "chat did not close")
        resumed.send("\x1b[B" * 4 + "\r")
        until(lambda: resumed.has("resident models"), "resident library missing")
        resumed.send("r")
        until(lambda: resumed.has("Every donor confirmed"), "library did not authenticate all allocations")
        resumed.send("x")
        until(lambda: resumed.has("Active chats will stop"), "unload did not require confirmation")
        resumed.send("\r")
        until(lambda: resumed.has("All donors confirmed release"), "approved allocation release failed")
        for name in ("a", "b"):
            state = service(name)["donor"]
            assert state["live"] and not state["segment_pid"] and not state["host_pid"] and state["phase"] == 9, state
        assert len(list((tmp / "chatter/.lumabri/resident-plans").glob("*.plan"))) == 1
        assert not list((tmp / "chatter").rglob("*.safetensors")), "client downloaded a checkpoint"
        print("HOUSEHOLD SERVICE FLOW: PASS (approval survives reconnect, detached preparation, real model after TUI/manager exit and crash, exact live library, authenticated unload)", flush=True)
    finally:
        for t in terminals:
            if t.p.poll() is None:
                t.p.terminate()
        for name in ("chatter", "a", "b", "owner"):
            try:
                service(name, "stop")
            except (AssertionError, subprocess.TimeoutExpired) as exc:
                print(f"Cleanup failed for test-only {name}: {exc}", flush=True)
        for t in terminals:
            try:
                t.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                t.p.kill(); t.p.wait()
            t.drain(); t.log.close(); os.close(t.master)


if __name__ == "__main__":
    main()
