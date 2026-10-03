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
import shutil
import struct
import subprocess
import tempfile
import termios
import time

from home_flow_test import TerminalText, current_frame, hosted_turn_complete, assert_stage_record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--models-dir", required=True, type=Path)
    parser.add_argument("--runtime-dir", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--keep-requester", action="store_true", help="also verify the normal post-preparation chat and saved calibration")
    parser.add_argument("--multi-model", action="store_true", help="two distinct checkpoints coexist on the same donors; unload only one")
    parser.add_argument("--sessions", type=int, choices=(1, 2, 4, 8), default=1)
    parser.add_argument("--donor-ram-gb", type=float, default=0.5)
    parser.add_argument("--prepare-timeout", type=int, default=180)
    parser.add_argument("--measure-sessions", action="store_true", help="also benchmark the single-slot baseline")
    args = parser.parse_args()
    if not 30 <= args.prepare_timeout <= 3600:
        parser.error("--prepare-timeout must be between 30 and 3600 seconds")
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
            settings.write_text(f"tracker={tracker}\ntoken=service-test\nmodels={args.models_dir.resolve()}\nram={max(1.0, args.donor_ram_gb) if args.multi_model else args.donor_ram_gb}\nowner={int(name == 'owner')}\n")
            settings.chmod(0o600)
        e = {**os.environ, "HOME": str(home), "LUMABRI_TOKEN": "service-test",
             "LUMABRI_ENCRYPT": "1", "LUMABRI_RESIDENT_REQUIRED": "1", "LUMABRI_HOME_HYBRID": "0",
             "LUMABRI_ADVERTISE": "127.0.0.1", "LUMABRI_HOME_PORT_BASE": str(base + 16 * slots[name]),
             "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_NO_DISK_PROBE": "1",
             "LUMABRI_PEER_KEY": str(home / "peer.key"), "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts"),
             # Tracker idle timeout must exceed the 5-second inventory
             # heartbeat. A 2-second timeout made healthy donors disappear
             # between reports and changed the plan during key navigation.
             "LUMABRI_IO_TIMEOUT_MS": "15000", "COLI_NO_OMP_TUNE": "1", "OMP_NUM_THREADS": "2", "PIN": "off"}
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
                     "--context", "128", "--max-new", "8", "--sessions", str(args.sessions)]
        chat = Terminal("chatter", base_args)
        until(lambda: "3 computers visible" in current_frame(chat), "signed inventory missing")
        chat.send("\t")
        until(lambda: "Nothing is selected automatically" in current_frame(chat), "computer selection missing")
        chat.send("\x1b[B\r")
        until(lambda: "3 computers visible · 1 selected" in current_frame(chat), "first donor selection missing")
        chat.send("\x1b[B\r")
        until(lambda: "3 computers visible · 2 selected" in current_frame(chat), "second donor selection missing")
        chat.send("\t\r")
        # A historical frame is not approval for a new inventory snapshot.
        # Wait through brief reporter reconnects; never send a one-donor plan
        # and then wait for two offers which that plan cannot produce.
        until(lambda: "Plan: resident" in current_frame(chat) and
              "3 computers visible · 2 selected" in current_frame(chat) and
              current_frame(chat).count("GB reserved") == 2, "two-donor resident preview missing")
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
        until(lambda: service("chatter")["prepare"]["state"] == "stopped", "preparation did not terminate", args.prepare_timeout)
        record = tmp / "chatter/.lumabri/resident-plan"
        assert record.exists(), (tmp / "chatter/.lumabri/service/prepare.log").read_text()[-5000:]
        if args.keep_requester:
            until(lambda: chat.has("receives the text"), "normal prepared chat did not start")
            chat.send("hi\n")
            until(lambda: hosted_turn_complete(chat.text), "normal prepared chat did not generate", 120)
            records = list((tmp / "chatter").rglob("*.cal"))
            assert len(records) == 1, "service preparation lost the content/calibration key"
            assert assert_stage_record(records[0]) == 1, "background preparation telemetry lost"
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
        until(lambda: resumed.has("New conversation · saved plan"), "saved plan entry missing")
        resumed.send("\r")
        until(lambda: resumed.has("receives the text"), "retained host did not answer")
        resumed.send("hi\n")
        until(lambda: hosted_turn_complete(resumed.text), "real generation failed after all TUI/manager restarts", 120)
        if args.keep_requester:
            assert assert_stage_record(records[0]) == 2, "resident real session did not update existing observations"
        resumed.text = ""; resumed.send("/quit\n")
        until(lambda: resumed.has("your workspace"), "chat did not close")
        if args.sessions > 1 or args.measure_sessions:
            raw = record.read_bytes(); at = 4; names = []
            for _ in range(5):
                size = struct.unpack_from("<H", raw, at)[0]; at += 2
                names.append(raw[at:at+size].decode()); at += size
            benchmark = subprocess.run([str(runtime / "test_hosted_sessions"), names[1], names[2], str(args.sessions)],
                                       env=env("chatter"), text=True, capture_output=True, timeout=180)
            (tmp / "sessions.jsonl").write_text(benchmark.stdout)
            (tmp / "sessions.stderr").write_text(benchmark.stderr)
            assert benchmark.returncode == 0, benchmark.stderr
            print(benchmark.stdout, flush=True)
        if args.multi_model:
            first_plan = tmp / "first.plan"; shutil.copyfile(record, first_plan); first_plan.chmod(0o600)
            configs = list(args.models_dir.glob("*/config.json"))
            assert len(configs) == 1, "multi-model test expects one tiny checkpoint"
            assert sum(p.stat().st_size for p in configs[0].parent.rglob("*") if p.is_file()) < 32 << 20, "only tiny fixtures may be copied"
            second_models = tmp / "second-models"; second_model = second_models / "other-checkpoint"
            shutil.copytree(configs[0].parent, second_model)
            config = json.loads((second_model / "config.json").read_text())
            config["_lumabri_test_variant"] = "independent-second-checkpoint"
            (second_model / "config.json").write_text(json.dumps(config))
            a, b = sharing("a"), sharing("b")
            another = Terminal("chatter", ["models", "--models-dir", str(second_models), "--tracker", tracker,
                                            "--context", "128", "--max-new", "8"])
            until(lambda: "3 computers visible" in current_frame(another), "second-model inventory missing")
            another.send("\t")
            until(lambda: "Nothing is selected automatically" in current_frame(another), "second selection missing")
            another.send("\x1b[B\r")
            until(lambda: "1 selected" in current_frame(another), "first second-model donor missing")
            another.send("\x1b[B\r")
            until(lambda: "2 selected" in current_frame(another), "second second-model donor missing")
            another.send("\t\r")
            until(lambda: "Plan: resident" in current_frame(another) and current_frame(another).count("GB reserved") == 2,
                  "existing allocations prevented a feasible second model")
            another.send("\r")
            until(lambda: "Waiting for your approval" in current_frame(a) and "Waiting for your approval" in current_frame(b),
                  "same donors did not receive second-model offers", 150)
            a.send("\x1b[A\r"); b.send("\x1b[A\r")
            until(lambda: another.has("receives the text"), "second model did not start", 180)
            another.send("hello second model\n")
            until(lambda: hosted_turn_complete(another.text), "second model did not generate", 120)
            second_live = {name: service(name)["donor"] for name in ("a", "b")}
            assert all(s["model_count"] == 2 and s["reserved_total_bytes"] > s["reserved_bytes"] for s in second_live.values()), second_live
            assert all(second_live[name]["segment_pid"] != before[name]["segment_pid"] for name in ("a", "b"))
            for name in ("a", "b", "chatter"):
                service(name, "restart")
            assert all(service(name)["donor"]["model_count"] == 2 for name in ("a", "b")), "manager lost a resident allocation"
            # Separate hosts must accept independent conversations while both
            # models are loaded; neither admission can reuse the other's KV.
            raw = first_plan.read_bytes(); at = 4; names = []
            for _ in range(5):
                size = struct.unpack_from("<H", raw, at)[0]; at += 2
                names.append(raw[at:at+size].decode()); at += size
            parallel = Terminal("chatter", ["chat", "--host", names[1], "--host-key", names[2],
                "--host-root", names[3], "--model", names[4], "--tracker", tracker, "--ctx", "128", "--max-new", "8"])
            until(lambda: parallel.has("receives the text"), "first model could not open a parallel conversation")
            another.text = ""; another.send("beta\n"); parallel.send("alpha\n")
            until(lambda: hosted_turn_complete(another.text) and hosted_turn_complete(parallel.text),
                  "concurrent independent models failed to generate", 120)
            parallel.send("/quit\n")
            until(lambda: parallel.p.poll() is not None, "parallel first-model chat did not close")
            # The authenticated release names the first allocation, not the
            # keeper's currently selected second model.
            release = subprocess.run([str(runtime / "test_chat_ui"), "resident-release", str(first_plan), tracker],
                                     env=env("chatter"), text=True, capture_output=True, timeout=20)
            assert release.returncode == 0, release.stderr
            assert all(service(name)["donor"]["model_count"] == 1 for name in ("a", "b"))
            assert all(service(name)["donor"]["segment_pid"] == second_live[name]["segment_pid"] for name in ("a", "b")), "unload killed the other model"
            another.text = ""; another.send("still here?\n")
            until(lambda: hosted_turn_complete(another.text), "second model stopped after first model unloaded", 120)
            another.send("/quit\n")
            until(lambda: another.p.poll() is not None, "second chat did not close")
            release = subprocess.run([str(runtime / "test_chat_ui"), "resident-release", str(record), tracker],
                                     env=env("chatter"), text=True, capture_output=True, timeout=20)
            assert release.returncode == 0, release.stderr
            assert all(not service(name)["donor"]["reserved_total_bytes"] for name in ("a", "b"))
            print("MULTI-MODEL SERVICE: PASS (two checkpoints on SAME donors, separate approval and RAM, manager restart, unload first while second chat lives)", flush=True)
            return
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
