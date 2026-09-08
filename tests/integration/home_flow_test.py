"""Real encrypted tracker, two donor TUIs, Segment engines and hosted chat.

Requires one actual small planner-supported checkpoint (not a mock engine).
This loopback integration gate is not a physical LAN or native-platform test.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--models-dir", required=True)
    parser.add_argument("--donor-ram-gb", type=float, default=0.5)
    parser.add_argument("--context", type=int, default=128,
                        help="approved context, including the actual family chat template")
    parser.add_argument("--expect-greedy", action="store_true")
    parser.add_argument("--repeat-cached", action="store_true",
                        help="run a separately approved second plan; verify shared weight reuse and source byte counters")
    parser.add_argument("--expect-metrics", action="store_true",
                        help="require versioned generation timings through Hosted into the TUI")
    parser.add_argument("--expect-no-fit", action="store_true",
                        help="verify insufficient-memory admission, without starting engines")
    parser.add_argument("--kill-donor", action="store_true",
                        help="kill a donor TUI after generation; assert engines and leases are released")
    args = parser.parse_args()
    if args.repeat_cached and (args.kill_donor or args.expect_no_fit):
        parser.error("--repeat-cached requires a normal completed first session")
    if args.repeat_cached and not (ROOT / "swarm_probe").is_file():
        parser.error("build swarm_probe before running --repeat-cached")
    tmp = Path(tempfile.mkdtemp(prefix="lumabri-home-flow-"))
    children, terminals = [], []
    print(f"Household test logs: {tmp}", flush=True)
    # Reserve and pass the actual listener, exactly as the household launcher
    # does. Closing a port probe before exec races other ephemeral sockets
    # (observed on the second native macOS run: EADDRINUSE).
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(64)
    port = listener.getsockname()[1]
    unreachable_socket = None
    addr = f"127.0.0.1:{port}"
    # Each simulated computer has its own endpoint namespace, just as it
    # would have its own IP on a physical LAN. Reusing one loopback endpoint
    # for two different donor identities must (correctly) fail persistent
    # TOFU authentication; do not erase known_hosts to make a repeat pass.
    service_base = 12000 + (port % 200) * 160
    service_slots = {"donor-a": 1, "donor-b": 2, "reject": 3,
                     "chatter": 4, "chatter-repeat": 5}

    def env(name):
        home = tmp / name
        home.mkdir(exist_ok=True)
        settings = home / ".lumabri" / "home.conf"
        if not settings.exists():
            settings.parent.mkdir(exist_ok=True)
            settings.write_text(f"tracker={addr}\ntoken=household-test\nmodels={Path(args.models_dir).resolve()}\nram={args.donor_ram_gb}\n")
            settings.chmod(0o600)
        return {**os.environ, "HOME": str(home), "LUMABRI_ENCRYPT": "1",
                "LUMABRI_HOME_PORT_BASE": str(service_base + 16 * service_slots.get(name, 0)),
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

    def source_stats(chat):
        # Source counters are published every ten seconds. Wait for a fresh
        # heartbeat after generation; a startup counter of zero proves nothing.
        model = re.search(r"home-[0-9a-f]{12}-[0-9a-f]{12}-[A-Za-z0-9_-]+", chat.text)
        assert model, "chat did not identify its model session"
        time.sleep(11)
        p = subprocess.run(["./swarm_probe", "--tracker", addr, "--model", model.group()],
                           cwd=ROOT, env=env("observer"), capture_output=True, text=True, timeout=15)
        assert p.returncode == 0, p.stderr
        sources = [row for row in json.loads(p.stdout)["peers"] if row["name"].startswith("home-source-")]
        assert len(sources) == 1 and sources[0]["age_s"] <= 11, sources
        return sources[0]["storage"]

    def cached_weights():
        result = {}
        for name in ("donor-a", "donor-b"):
            home = tmp / name / ".lumabri/home"
            mirrors = list((home / "mirrors").glob("*/cache"))
            assert len(mirrors) == 1 and re.fullmatch(r"[a-z0-9_]+", mirrors[0].parent.name), mirrors
            chunks = {str(p.relative_to(home / "cas")): p.stat().st_ino
                      for p in (home / "cas").glob("*/*") if re.fullmatch(r"[0-9a-f]{64}", p.name)}
            assert chunks, f"{name} did not retain verified content chunks"
            result[name] = (str(mirrors[0]), chunks)
        return result

    base = ["./lumabri", "models", "--models-dir", str(Path(args.models_dir).resolve()),
            "--tracker", addr, "--context", str(args.context), "--max-new", "8"]

    try:
        with open(tmp / "tracker.log", "wb") as log:
            tracker_env = {**env("tracker"), "LUMABRI_HOME_LISTEN_FD": str(listener.fileno())}
            tracker = subprocess.Popen(["./tracker", "--port", str(port), "--token", "household-test",
                "--peer-bindings", str(tmp / "bindings")], cwd=ROOT,
                env=tracker_env, pass_fds=(listener.fileno(),), stdout=log, stderr=subprocess.STDOUT)
        listener.close()
        children.append(tracker)

        def listening():
            if tracker.poll() is not None:
                raise AssertionError("tracker exited before readiness: " +
                                     (tmp / "tracker.log").read_text(errors="replace"))
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    return True
            except OSError:
                return False
        until(listening, message="reserved tracker listener did not become reachable")

        # A signed inventory advert does not prove the donor port is reachable.
        # Fail before indexing or sending an allocation, with a useful reason.
        # Bound but not listening: the negative case stays unreachable without
        # allowing another process to acquire the address during this test.
        unreachable_socket = socket.socket()
        unreachable_socket.bind(("127.0.0.1", 0))
        unreachable = unreachable_socket.getsockname()[1]
        with open(tmp / "offline-worker.log", "wb") as log:
            offline_worker = subprocess.Popen(["./lumabri", "worker", "--join", addr,
                "--name", "offline-test-donor", "--ram-gb", str(args.donor_ram_gb), "--disk", str(tmp),
                "--control-address", f"127.0.0.1:{unreachable}"], cwd=ROOT,
                env=env("offline-worker"), stdout=log, stderr=subprocess.STDOUT)
        children.append(offline_worker)
        offline = Terminal("offline-request", base)
        until(lambda: offline.has("2 computers"), message="offline worker advert missing")
        offline.send("\t\x1b[B\r\t")
        time.sleep(.5)
        offline.send("\r\r")
        until(lambda: offline.p.poll() is not None, message="unreachable donor did not fail preflight")
        failure = "No complete resident plan fits" if args.expect_no_fit else "Cannot reach offline-test-donor"
        assert offline.p.returncode != 0 and offline.has(failure)
        assert not list((tmp / "offline-request").rglob("home-source-*.log")), "indexed before reachability check"
        offline_worker.terminate(); offline_worker.wait(timeout=5)

        a = Terminal("donor-a", ["./lumabri"])
        b = Terminal("donor-b", ["./lumabri"])
        until(lambda: a.has("your workspace") and b.has("your workspace"))
        a.send("\x1b[B\x1b[B\x1b[B\r"); b.send("\x1b[B\x1b[B\x1b[B\r")
        until(lambda: a.has("Available") and b.has("Available"))
        a.send("\r"); b.send("\r")  # Enter without a request must not stop sharing.
        time.sleep(.3)
        assert a.p.poll() is None and b.p.poll() is None

        def inventory_ready():
            p = subprocess.run(base + ["--json"], cwd=ROOT, env=env("observer"),
                               capture_output=True, text=True, timeout=15)
            return p.returncode == 0 and len(json.loads(p.stdout)["nodes"]) == 3
        until(inventory_ready)
        reject = Terminal("reject", base)
        until(lambda: reject.has("3 computers"))
        reject.send("\t")
        until(lambda: reject.has("Nothing is selected automatically"))
        reject.send("\x1b[B\r\x1b[B\r\t")
        time.sleep(.5)
        if args.expect_no_fit:
            reject.send("\r")
            until(lambda: reject.has("model plan") and reject.has("sizing available"),
                  message="no valid sizing result in model detail")
            assert reject.has("Plan: not runnable"), "undersized donors were admitted"
            assert not a.has("Waiting for your approval") and not b.has("Waiting for your approval")
            assert not engines_started("donor-a") and not engines_started("donor-b")
            assert not list((tmp / "reject").rglob("home-source-*.log"))
            print("HOME ADMISSION: PASS (native memory floor; no offers or engines)", flush=True)
            return
        reject.send("\r\r")
        until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"),
              seconds=60, message="offers never reached both donor TUIs")
        assert not engines_started("donor-a") and not engines_started("donor-b")
        a.send("\x1b[A\r")
        until(lambda: a.has("Accepted; waiting"))
        assert not engines_started("donor-a") and not engines_started("donor-b")
        b.send("\r")  # Safe default is Decline, not Accept.
        until(lambda: reject.p.poll() is not None, message="rejection did not cancel the whole plan")
        assert reject.p.returncode != 0
        assert not engines_started("donor-a") and not engines_started("donor-b")
        until(lambda: a.has("Released") and b.has("Released"))
        a.text = b.text = ""
        chat = Terminal("chatter", base)
        until(lambda: chat.has("3 computers"))
        chat.send("\t")
        until(lambda: chat.has("Nothing is selected automatically"))
        chat.send("\x1b[B\r\x1b[B\r\t")
        time.sleep(.5)
        chat.send("\r\r")
        until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"), seconds=60)
        a.send("\x1b[A\r"); b.send("\x1b[A\r")
        until(lambda: chat.has("receives the text") or chat.p.poll() is not None, seconds=180,
              message="accepted plan did not reach real hosted chat")
        assert chat.p.poll() is None, "accepted plan failed; inspect donor engine logs"
        if args.expect_greedy:
            assert chat.has("greedy decoding"), "greedy-only capability was not shown to the client"
        until(lambda: chat.has("/experts shows tracker activity."),
              message="the accepted compute allocation is missing from chat")
        assert "Approved Segment plan: 2 compute donors" in chat.text
        assert "This chat process runs no model layers" in chat.text
        announced_ranges = sorted((int(begin), int(end)) for begin, end in
                                  re.findall(r"layers \[(\d+),(\d+)\)", chat.text))
        assert len(announced_ranges) == 2, announced_ranges
        chat.send("/he")
        until(lambda: chat.has("List chat commands"), message="slash suggestions did not appear")
        chat.send("\t\n")
        until(lambda: chat.has("Tab completes commands"), message="slash completion did not execute help")
        chat.send("hi\n")
        until(lambda: chat.has("tok/s") or chat.has("generated tokens") or chat.has("prompt plus output exceeds context") or
              chat.has("logits are unavailable for sampling") or chat.has("Segment generation failed") or
              chat.has("invalid token count") or chat.p.poll() is not None, seconds=120,
              message="real model did not finish a response")
        assert chat.has("tok/s") or chat.has("generated tokens"), "engine failed during generation"
        if args.expect_metrics:
            assert chat.has("host prefill") and chat.has("generated tokens"), "versioned engine timings did not reach the TUI"
            assert not chat.has("timing unavailable"), "inconsistent engine timings"
        assert "hosted stream · no local checkpoint" in chat.text
        executed_ranges = []
        for donor in ("donor-a", "donor-b"):
            log = (tmp / donor / ".lumabri/home/engines.log").read_text(errors="replace")
            commits = re.findall(r"\[segment-node [^\]\n]+ (\d+):(\d+)\] committed_runs=(\d+)", log)
            assert commits, f"{donor} did not report any committed model execution"
            ranges = {(int(begin), int(end)) for begin, end, _ in commits}
            assert len(ranges) == 1, ranges
            executed_ranges.extend(ranges)
        assert sorted(executed_ranges) == announced_ranges, (executed_ranges, announced_ranges)
        chat.send("/plan\n")
        until(lambda: chat.text.count("Approved Segment plan: 2 compute donors") >= 2,
              message="/plan did not show the same accepted allocation")
        chat.send("/experts\n")
        until(lambda: chat.has("executor activity"),
              message="hosted chat did not retain its household tracker for diagnostics")
        if args.repeat_cached:
            cold_bytes = source_stats(chat)["bytes_served"]
            assert cold_bytes > 0, "cold run did not exercise the byte source"
        owned_groups = set()
        if args.kill_donor:
            rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,pgid="], text=True)
            processes = [tuple(map(int, row.split())) for row in rows.splitlines()]
            owned = {a.p.pid}
            while True:
                descendants = owned | {pid for pid, parent, group in processes if parent in owned}
                if descendants == owned:
                    break
                owned = descendants
            owned_groups = {group for pid, parent, group in processes
                            if pid in owned and group != os.getpgrp() and group == pid}
            assert owned_groups, "test did not find the owned runtime groups"
            a.p.kill(); a.p.wait(timeout=5)
            until(lambda: chat.p.poll() is not None, seconds=45,
                  message="lost donor left hosted chat blocked")
            until(lambda: b.has("Released"), message="surviving donor was not released")
        else:
            chat.send("/quit\n")
            until(lambda: chat.p.poll() is not None, message="quit left hosted chat blocked")
            assert chat.p.returncode == 0
            until(lambda: a.has("Released") and b.has("Released"), message="donor leases were not released")
        def leases_released():
            for name in ("donor-a", "donor-b"):
                for lock in ("compute-donor.lock", "home/weights.lock"):
                    with open(tmp / name / ".lumabri" / lock, "r") as lease:
                        try:
                            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            return False
            return True
        until(leases_released, message="a child retained a donor resource lease after closing chat")
        if args.repeat_cached:
            before = cached_weights()
            a.text = b.text = ""
            repeat = Terminal("chatter-repeat", base)
            until(lambda: repeat.has("3 computers"))
            repeat.send("\t")
            until(lambda: repeat.has("Nothing is selected automatically"))
            repeat.send("\x1b[B\r\x1b[B\r\t")
            time.sleep(.5)
            repeat.send("\r\r")
            until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"), seconds=60)
            # Cached weights do not confer permission to execute again.
            assert not repeat.has("receives the text")
            a.send("\x1b[A\r"); b.send("\x1b[A\r")
            until(lambda: repeat.has("receives the text") or repeat.p.poll() is not None, seconds=180)
            assert repeat.p.poll() is None, "cached plan failed to start"
            repeat.send("hi\n")
            until(lambda: repeat.has("tok/s") or repeat.has("generated tokens") or repeat.p.poll() is not None,
                  seconds=120, message="cached model did not finish a response")
            assert repeat.p.poll() is None and "no local checkpoint" in repeat.text
            warm = source_stats(repeat)
            # The requester and host each inspect config.json directly to
            # select the engine. These metadata reads still cross the wire;
            # do not misreport a warm weight cache as zero network traffic.
            configs = list(Path(args.models_dir).glob("*/config.json"))
            assert len(configs) == 1, "cache counter gate requires one fixture model"
            metadata_bytes = 2 * configs[0].stat().st_size
            assert warm["reads"] == 2 and warm["bytes_served"] == metadata_bytes, (cold_bytes, warm)
            assert cold_bytes > metadata_bytes
            assert cached_weights() == before, "the same checkpoint duplicated its persistent weight cache"
            repeat.send("/quit\n")
            until(lambda: repeat.p.poll() is not None)
            assert repeat.p.returncode == 0
            until(lambda: a.has("Released") and b.has("Released"))
            until(leases_released)
            assert not list((tmp / "chatter-repeat").rglob("*.safetensors"))
            print(f"HOME CACHE: PASS (new approval, same checkpoint cache, source bytes {cold_bytes} -> {warm['bytes_served']}, two config reads remain)", flush=True)
        def no_owned_processes():
            rows = subprocess.check_output(["ps", "-axo", "pgid=,stat="], text=True)
            return not any(int(row.split()[0]) in owned_groups and not row.split()[1].startswith("Z")
                           for row in rows.splitlines())
        if args.kill_donor:
            until(no_owned_processes, message="a donor engine survived its terminated TUI")
        for name in ("chatter", "reject"):
            assert not list((tmp / name).rglob("*.safetensors")), "thin client downloaded weights"
            assert not list((tmp / name).rglob("vroot")), "thin client mounted a checkpoint"
        for donor in ([b] if args.kill_donor else [a, b]):
            donor.text = ""
            donor.send("\x1b")
            until(lambda: donor.has("your workspace"))
            donor.send("\x1b")
        until(lambda: a.p.poll() is not None and b.p.poll() is not None)
        print(f"HOME FLOW: PASS (consent, two executing ranges match the plan, real Segment generation, {'killed donor cleanup' if args.kill_donor else 'normal cleanup'})", flush=True)
    except Exception:
        # Only test-owned engine logs: no shell environment or real keys.
        for log in tmp.rglob("engines.log"):
            print(f"\n{log.relative_to(tmp)}:\n{log.read_text(errors='replace')[-12000:]}", file=sys.stderr)
        raise
    finally:
        listener.close()
        if unreachable_socket is not None:
            unreachable_socket.close()
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
