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
    global ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-dir", type=Path, default=ROOT,
                        help="directory containing the installed household binaries; no source tree fallback")
    parser.add_argument("--models-dir", required=True)
    parser.add_argument("--expect-disjoint-plans", action="store_true",
                        help="verify two simultaneous household chats using different approved compute donors")
    parser.add_argument("--expect-quick-calibration", action="store_true",
                        help="request the optional 8-token probe through the TUI and verify its saved measurement")
    parser.add_argument("--donor-ram-gb", type=float, default=0.5)
    parser.add_argument("--context", type=int, default=128,
                        help="approved context, including the actual family chat template")
    parser.add_argument("--expect-greedy", action="store_true")
    parser.add_argument("--repeat-cached", action="store_true",
                        help="run a separately approved second plan; verify shared weight reuse and source byte counters")
    parser.add_argument("--clear-cached", action="store_true",
                        help="after reuse, clear weights from the donor workspace and verify an approved cold restart")
    parser.add_argument("--expect-metrics", action="store_true",
                        help="require versioned generation timings through Hosted into the TUI")
    parser.add_argument("--expect-calibration", action="store_true",
                        help="require saved real timings, matching catalogue speed and changed-context invalidation")
    parser.add_argument("--expect-no-fit", action="store_true",
                        help="verify insufficient-memory admission, without starting engines")
    parser.add_argument("--expect-unused-donor", action="store_true",
                        help="select a donor below the process floor; require a valid plan on the other donor only")
    parser.add_argument("--kill-donor", action="store_true",
                        help="kill a donor TUI after generation; assert engines and leases are released")
    parser.add_argument("--crash-requester", action="store_true",
                        help="resident contract: lose the chatter, retain weights beyond the control lease, reconnect")
    args = parser.parse_args()
    resident = os.environ.get("LUMABRI_RESIDENT_REQUIRED") == "1"
    if resident and args.repeat_cached:
        parser.error("resident weights persist in RAM; --repeat-cached exercises the legacy disk cache")
    if args.crash_requester and (not resident or args.kill_donor or args.repeat_cached):
        parser.error("--crash-requester requires the resident path and no other termination mode")
    if args.expect_quick_calibration and any((args.expect_disjoint_plans, args.repeat_cached,
        args.expect_no_fit, args.expect_unused_donor, args.kill_donor,
        args.expect_calibration, args.expect_metrics, args.expect_greedy)):
        parser.error("--expect-quick-calibration is a separate measurement test")
    if args.expect_disjoint_plans and any((args.repeat_cached, args.expect_no_fit,
        args.expect_unused_donor, args.kill_donor, args.expect_calibration,
        args.expect_metrics, args.expect_greedy)):
        parser.error("--expect-disjoint-plans is a separate concurrency test")
    if args.clear_cached and not args.repeat_cached:
        parser.error("--clear-cached requires --repeat-cached")
    if args.expect_unused_donor and (args.repeat_cached or args.expect_no_fit or
                                    args.kill_donor or args.expect_calibration):
        parser.error("--expect-unused-donor is a separate single-session admission test")
    ROOT = args.runtime_dir.resolve(strict=True)
    for binary in ("lumabri", "tracker", "maintainer", "segment_node", "segment_chat"):
        if not (ROOT / binary).is_file() or not os.access(ROOT / binary, os.X_OK):
            parser.error(f"missing executable in runtime directory: {binary}")
    if args.repeat_cached and (args.kill_donor or args.expect_no_fit):
        parser.error("--repeat-cached requires a normal completed first session")
    if args.expect_calibration and (args.kill_donor or args.expect_no_fit):
        parser.error("--expect-calibration requires a normally completed session")
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
                     "chatter": 4, "chatter-repeat": 5, "chatter-parallel": 6}

    def env(name):
        home = tmp / name
        home.mkdir(exist_ok=True)
        settings = home / ".lumabri" / "home.conf"
        if not settings.exists():
            settings.parent.mkdir(exist_ok=True)
            ram = 0.1 if args.expect_unused_donor and name == "donor-b" else args.donor_ram_gb
            settings.write_text(f"tracker={addr}\ntoken=household-test\nmodels={Path(args.models_dir).resolve()}\nram={ram}\n")
            settings.chmod(0o600)
        return {**os.environ, "HOME": str(home), "LUMABRI_ENCRYPT": "1",
                "LUMABRI_HOME_PORT_BASE": str(service_base + 16 * service_slots.get(name, 0)),
                "LUMABRI_PEER_KEY": str(home / "peer.key"),
                "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts"),
                "LUMABRI_TOKEN": "household-test", "LUMABRI_NO_DISK_PROBE": "1",
                # An inherited preference must not override CPU-only approval.
                "LUMABRI_ENGINE_BACKEND": "cuda",
                "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_IO_TIMEOUT_MS": "10000",
                "OMP_NUM_THREADS": "2", "COLI_NO_OMP_TUNE": "1", "PIN": "off"}

    class Terminal:
        def __init__(self, name, argv, environment_name=None):
            self.name, self.text = name, ""
            self.master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 140, 0, 0))
            self.log = open(tmp / f"{name}.terminal.log", "wb")
            self.p = subprocess.Popen(argv, cwd=ROOT, env=env(environment_name or name),
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
        failure = "No complete resident plan found" if args.expect_no_fit else "Cannot reach offline-test-donor"
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
        if args.expect_quick_calibration:
            chat = Terminal("chatter", base)
            until(lambda: chat.has("3 computers"))
            chat.send("\t")
            until(lambda: chat.has("Nothing is selected automatically"))
            chat.send("\x1b[B\r\x1b[B\r\t")
            time.sleep(.5)
            chat.send("/" + "\x1b[B" * 4 + "\r")
            until(lambda: chat.has("Quick calibration") and chat.has("Preparation is not included"))
            assert not a.has("Waiting for your approval") and not b.has("Waiting for your approval")
            assert not list((tmp / "chatter").rglob("home-source-*.log"))
            # Reviewing or dismissing the measurement is not consent to load.
            chat.text = ""; chat.send("\x1b")
            until(lambda: chat.has("A plan before a download."))
            assert not engines_started("donor-a") and not engines_started("donor-b")
            chat.send("/")
            until(lambda: chat.has("/calibrate"))
            chat.text = ""
            # One burst, intentionally: close/reopen actions without a sleep.
            # The catalogue used to swallow '/' while decoding the prior Esc.
            chat.send("\x1b/" + "\x1b[B" * 4 + "\r")
            until(lambda: chat.has("Preparation is not included"))
            chat.send("\r")
            until(lambda: a.has("Waiting for your approval") and b.has("Waiting for your approval"), seconds=60)
            assert not engines_started("donor-a") and not engines_started("donor-b")
            a.send("\x1b[A\r"); b.send("\x1b[A\r")
            until(lambda: chat.p.poll() is not None, seconds=180,
                  message="quick measurement did not return after its one bounded turn")
            assert chat.p.returncode == 0 and chat.has("Quick calibration complete"), chat.text[-2500:]
            assert chat.has("at most 8 tokens, 20 seconds")
            generated = re.findall(r"host prefill [\d.]+s · (\d+) generated tokens", chat.text)
            assert len(generated) == 1 and 2 <= int(generated[0]) <= 8, generated
            assert chat.has("hosted stream") and chat.has("no local checkpoint")
            records = list((tmp / "chatter/.lumabri/calibrations").glob("*.cal"))
            assert len(records) == 1 and records[0].stat().st_mode & 0o077 == 0
            until(lambda: a.has("Released") and b.has("Released"))
            for name in ("donor-a", "donor-b"):
                for lock in ("compute-donor.lock", "home/weights.lock"):
                    with open(tmp / name / ".lumabri" / lock, "r") as lease:
                        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            assert not list((tmp / "chatter").rglob("*.safetensors"))
            reopened = Terminal("probe-catalogue", base, "chatter")
            until(lambda: reopened.has("3 computers"))
            reopened.send("\t\x1b[B\r\x1b[B\r\t")
            until(lambda: reopened.has("tok/s (last)"), seconds=30,
                  message="the short measurement did not reappear for the same selected plan")
            reopened.send("\r")
            until(lambda: reopened.has("Last turn:") and reopened.has(" / " + generated[0] + " generated tokens"))
            print("HOME QUICK CALIBRATION: PASS (explicit TUI confirmation and donor approval, one <=8-token turn, real timing saved and reopened, released leases)", flush=True)
            return
        if args.expect_disjoint_plans:
            chats, owners = [], []

            def lease_is_held(donor, lock):
                with open(tmp / donor.name / ".lumabri" / lock, "r") as lease:
                    try:
                        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        return True
                return False

            def completed_turn(chat):
                return chat.has("hosted stream") and chat.has("no local checkpoint")

            def reject_busy(label):
                collision = Terminal(label, base, "reject")
                until(lambda: collision.has("3 computers"))
                collision.send("\t")
                until(lambda: collision.has("Nothing is selected automatically"))
                collision.send("\x1b[B\r\t")
                time.sleep(.5); collision.send("\r\r")
                until(lambda: collision.p.poll() is not None, seconds=60,
                      message="an occupied donor did not refuse the conflicting plan")
                assert collision.p.returncode != 0 and collision.has("BUSY:"), collision.text[-1500:]
                assert not collision.has("receives the text")

            for index, name in enumerate(("chatter", "chatter-parallel"), 1):
                chat = Terminal(name, base)
                chats.append(chat)
                until(lambda: chat.has("3 computers"))
                chat.send("\t")
                until(lambda: chat.has("Nothing is selected automatically"))
                chat.send("\x1b[B" * index + "\r\t")
                time.sleep(.5)
                chat.send("\r\r")
                available = [donor for donor in (a, b) if donor not in owners]
                until(lambda: any(donor.has("Waiting for your approval") for donor in available)
                      or chat.p.poll() is not None, seconds=60,
                      message="a disjoint plan did not reach its unused donor")
                assert chat.p.poll() is None
                pending = [donor for donor in available if donor.has("Waiting for your approval")]
                assert len(pending) == 1, "single-donor selection was not preserved"
                owner = pending[0]; owners.append(owner)
                assert not chat.has("receives the text"), "a plan started without its owner's approval"
                owner.send("\x1b[A")
                if index == 1:
                    # The owner has selected Accept but has not confirmed it.
                    # Another client's preflight and offer must neither replace
                    # that request nor silently reset Accept to Decline.
                    time.sleep(.3)
                    reject_busy("reject-pending")
                owner.send("\r")
                until(lambda: chat.has("Approved Segment plan: 1 compute donor") or chat.p.poll() is not None,
                      seconds=180, message="an approved independent plan failed to start")
                assert chat.p.poll() is None
            # Both actual hosted sessions and both independent resource/cache
            # leases must coexist. This is not two serial requests relabelled
            # as concurrency, nor two sessions sharing one compute process.
            assert len(set(owner.name for owner in owners)) == 2
            reject_busy("reject-active")
            for owner in owners:
                for lock in ("compute-donor.lock", "home/weights.lock"):
                    assert lease_is_held(owner, lock), "a live plan did not own its reservation"
            for chat, prompt in zip(chats, ("hi", "hello")):
                chat.text = ""; chat.send(prompt + "\n")
            until(lambda: all(completed_turn(chat) for chat in chats)
                  or any(chat.p.poll() is not None for chat in chats), seconds=120,
                  message="simultaneous independent chats did not both generate")
            assert all(chat.p.poll() is None and completed_turn(chat) for chat in chats)
            chats[0].send("/quit\n")
            until(lambda: chats[0].p.poll() is not None and owners[0].has("Released"))
            assert chats[0].p.returncode == 0
            for lock in ("compute-donor.lock", "home/weights.lock"):
                assert not lease_is_held(owners[0], lock)
                assert lease_is_held(owners[1], lock), "closing one plan released the other plan's lease"
            chats[1].text = ""; chats[1].send("again\n")
            until(lambda: completed_turn(chats[1]) or chats[1].p.poll() is not None,
                  seconds=120, message="the surviving chat could not continue")
            assert chats[1].p.poll() is None and completed_turn(chats[1])
            chats[1].send("/quit\n")
            until(lambda: chats[1].p.poll() is not None and owners[1].has("Released"))
            assert chats[1].p.returncode == 0
            ranges = []
            for owner in owners:
                for lock in ("compute-donor.lock", "home/weights.lock"):
                    assert not lease_is_held(owner, lock)
                log = (tmp / owner.name / ".lumabri/home/engines.log").read_text(errors="replace")
                runs = re.findall(r"\[segment-node [^\]\n]+ (\d+):(\d+)\] committed_runs=(\d+)", log)
                assert runs, "a selected compute donor did not execute"
                executed = {(int(begin), int(end)) for begin, end, _ in runs}
                assert len(executed) == 1
                ranges.append(next(iter(executed)))
            assert ranges[0] == ranges[1] and ranges[0][0] == 0 and ranges[0][1] > 0
            for chat in chats:
                assert not list((tmp / chat.name).rglob("*.safetensors"))
            print("HOME DISJOINT PLANS: PASS (two simultaneous approved chats, independent engines and leases, closing one preserves the other)", flush=True)
            return
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
        if args.expect_unused_donor:
            until(lambda: a.has("Waiting for your approval"), seconds=60,
                  message="feasible alternative did not reach its donor")
            assert not b.has("Waiting for your approval"), "unused donor received an offer"
            assert not engines_started("donor-a") and not engines_started("donor-b")
            a.send("\x1b[A\r")
            until(lambda: reject.has("/experts shows tracker activity.") or reject.p.poll() is not None,
                  seconds=180, message="alternative plan did not reach hosted chat")
            assert reject.p.poll() is None, "accepted alternative failed"
            assert "Approved Segment plan: 1 compute donor" in reject.text
            reject.send("hi\n")
            until(lambda: reject.has("hosted stream · no local checkpoint") or reject.p.poll() is not None,
                  seconds=120, message="alternative plan did not finish real generation")
            assert reject.p.poll() is None
            log = (tmp / "donor-a/.lumabri/home/engines.log").read_text(errors="replace")
            commits = re.findall(r"\[segment-node [^\]\n]+ (\d+):(\d+)\] committed_runs=(\d+)", log)
            assert any(int(begin) == 0 and int(end) > 0 and int(runs) > 0
                       for begin, end, runs in commits), "no actual full-range execution"
            assert not b.has("Waiting for your approval") and not engines_started("donor-b")
            reject.send("/quit\n")
            until(lambda: reject.p.poll() is not None, message="alternative quit blocked")
            assert reject.p.returncode == 0
            until(lambda: a.has("Released"), message="alternative donor lease leaked")
            for lock in ("compute-donor.lock", "home/weights.lock"):
                with open(tmp / "donor-a/.lumabri" / lock, "r") as lease:
                    fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            print("HOME ALTERNATIVE PLAN: PASS (two selected, one used; real generation, no unused-donor offer, cleanup)", flush=True)
            return
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
        assert "Transferring weights and loading approved segments" in chat.text
        assert "MB served from this computer" in chat.text
        assert "remaining time unavailable" in chat.text, "on-demand transfer invented a denominator"
        assert "Loading the chat host; segments are ready" in chat.text
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
              chat.has("invalid token count") or chat.has("cannot read DeepSeek V4 embedding") or chat.p.poll() is not None, seconds=120,
              message="real model did not finish a response")
        assert chat.has("tok/s") or chat.has("generated tokens"), "engine failed during generation"
        if args.expect_metrics:
            assert chat.has("host prefill") and chat.has("generated tokens"), "versioned engine timings did not reach the TUI"
            assert not chat.has("timing unavailable"), "inconsistent engine timings"
        assert "hosted stream · no local checkpoint" in chat.text
        executed_ranges = []
        edge_policies = 0
        for donor in ("donor-a", "donor-b"):
            log = (tmp / donor / ".lumabri/home/engines.log").read_text(errors="replace")
            assert "[segment-node] backend policy=cpu adapter_mask=0x100" in log, \
                f"{donor} did not enforce the approved CPU policy"
            edge_log = tmp / donor / ".lumabri/home/edge.log"
            if edge_log.exists():
                edge_policies += "[segment-chat] backend policy=cpu adapter_mask=0x100" in edge_log.read_text(errors="replace")
                observations = re.findall(r"\[segment-stage\] request=\d+ (\{[^\n]+\})", edge_log.read_text(errors="replace"))
                assert observations, "the Edge host did not report per-range RUN observations"
                profile = json.loads(observations[-1])
                assert profile["version"] == 1 and profile["valid"], profile
                assert profile["scope"] == "client_run_round_trip"
                assert [(s["begin"], s["end"]) for s in profile["stages"]] == announced_ranges
                counts = {s["decode_calls"] for s in profile["stages"]}
                assert len(counts) == 1 and min(counts) > 0, counts
                for s in profile["stages"]:
                    assert s["prefill_calls"] > 0 and s["prefill_rows"] > 0
                    assert 0 <= s["decode_min_seconds"] <= s["decode_max_seconds"]
                    assert s["decode_min_seconds"] * s["decode_calls"] <= s["decode_seconds"] + 1e-6
                    assert s["decode_seconds"] <= s["decode_max_seconds"] * s["decode_calls"] + 1e-6
            commits = re.findall(r"\[segment-node [^\]\n]+ (\d+):(\d+)\] committed_runs=(\d+)", log)
            assert commits, f"{donor} did not report any committed model execution"
            ranges = {(int(begin), int(end)) for begin, end, _ in commits}
            assert len(ranges) == 1, ranges
            executed_ranges.extend(ranges)
        assert sorted(executed_ranges) == announced_ranges, (executed_ranges, announced_ranges)
        assert edge_policies == 1, "the approved Edge host did not enforce CPU execution"
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
            if args.crash_requester:
                chat.p.kill()
            else:
                chat.send("/quit\n")
            until(lambda: chat.p.poll() is not None, message="quit left hosted chat blocked")
            assert chat.p.returncode == (-signal.SIGKILL if args.crash_requester else 0)
            if resident:
                until(lambda: a.has("Weights retained in RAM") and b.has("Weights retained in RAM"),
                      message="closing chat unloaded the resident donor allocation")
            else:
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
        if resident and not args.kill_donor:
            assert not leases_released(), "resident allocation lost its memory reservation"
            if args.crash_requester:
                deadline = time.monotonic() + 17
                while time.monotonic() < deadline:
                    for terminal in terminals:
                        terminal.drain()
                    assert not leases_released(), "control lease expiration evicted resident weights"
                    time.sleep(.1)
            engine_logs = [tmp / name / ".lumabri/home/engines.log" for name in ("donor-a", "donor-b")]
            def reset_count():
                return sum(path.read_text(errors="replace").count("conversation reset; resident weights retained")
                           for path in engine_logs)
            until(lambda: reset_count() >= 1, message="host reloaded weights instead of resetting its conversation")
            boot_counts = {path: path.read_text(errors="replace").count("weight input sealed") for path in engine_logs}
            endpoint = re.search(r"host (127\.0\.0\.1:[0-9]+) ·", chat.text)
            assert endpoint, "the original host endpoint is missing"
            # The original requester and its checkpoint-serving child have
            # exited. Reconnect with the same accepted identity: no source,
            # no donor restart and no checkpoint download are available.
            resume = Terminal("resident-resume", ["./lumabri", "chat", "--host", endpoint.group(1),
                "--tracker", addr, "--ctx", str(args.context), "--max-new", "4"], "chatter")
            until(lambda: resume.has("receives the text") or resume.p.poll() is not None)
            assert resume.p.poll() is None, "retained host cannot accept a second conversation"
            resume.send("hi\n")
            until(lambda: resume.has("hosted stream · no local checkpoint") or resume.p.poll() is not None,
                  seconds=120, message="resident generation failed with the weight source offline")
            assert resume.p.poll() is None and resume.has("generated tokens"), "resident second turn did not generate"
            resume.send("/quit\n")
            until(lambda: resume.p.poll() is not None)
            assert resume.p.returncode == 0
            until(lambda: reset_count() >= 2, message="second conversation was not reset in place")
            for path, count in boot_counts.items():
                assert path.read_text(errors="replace").count("weight input sealed") == count, "resident engine rebooted"
            for donor in ("donor-a", "donor-b"):
                for weight in (tmp / donor).rglob("*.safetensors"):
                    assert weight.stat().st_blocks == 0, f"weight payload reached disk: {weight}"
            assert not leases_released(), "second chat released the resident reservation"
            print("HOME RESIDENT PASS: second private conversation, source offline, no reload, no weight mirror writes", flush=True)
            a.text = b.text = ""
            a.send("x"); b.send("x")
            until(lambda: a.has("Released") and b.has("Released"), message="owner Stop did not unload weights")
        until(leases_released, message="an allocation retained its lease after explicit release")
        if args.expect_calibration:
            records = list((tmp / "chatter/.lumabri/calibrations").glob("*.cal"))
            assert len(records) == 1, "completed real generation did not save a bound measurement"
            assert records[0].stat().st_mode & 0o077 == 0, "calibration record is not private"
            for condition in ("current", "context", "selection", "runtime"):
                changed = condition != "current"
                if condition == "runtime":
                    a.text = ""; a.send("\x1b")
                    until(lambda: a.has("your workspace"), message="donor did not stop sharing")
                    a.send("\x1b")  # exit the workspace; this PTY has no foreground signal group
                    until(lambda: a.p.poll() is not None, message="donor workspace did not close")
                    a = Terminal("donor-a-restarted", ["./lumabri"], "donor-a")
                    until(lambda: a.has("your workspace"))
                    a.send("\x1b[B\x1b[B\x1b[B\r")
                    until(lambda: a.has("Available"))
                argv = base + (["--context", str(args.context * 2)] if condition == "context" else [])
                view = Terminal("calibration-" + condition, argv, "chatter")
                until(lambda: view.has("3 computers"))
                view.send("\t")
                until(lambda: view.has("Nothing is selected automatically"))
                view.send("\x1b[B\r\t" if condition == "selection" else "\x1b[B\r\x1b[B\r\t")
                until(lambda: view.has("A plan before a download."), message="catalogue did not reopen")
                time.sleep(1); view.text = ""
                until(lambda: view.has("stale") if changed else view.has("tok/s"), seconds=30,
                      message=f"changed {condition} did not invalidate the speed" if changed else "matching plan did not recover its measured speed")
                if changed:
                    assert "tok/s" not in view.text, "stale catalogue retained a numerical speed"
                view.send("q")
                until(lambda: view.p.poll() is not None)
                assert view.p.returncode == 0
            print("HOME CALIBRATION: PASS (real timing saved, catalogue reopened; context, selection and donor restart invalidate speed)", flush=True)
        if args.repeat_cached:
            before = cached_weights()
            # The requester and host each inspect config.json directly to
            # select the engine. These metadata reads still cross the wire;
            # do not misreport a warm weight cache as zero network traffic.
            configs = list(Path(args.models_dir).glob("*/config.json"))
            assert len(configs) == 1, "cache counter gate requires one fixture model"
            metadata_bytes = 2 * configs[0].stat().st_size
            assert cold_bytes > metadata_bytes
            for cleared in ([False, True] if args.clear_cached else [False]):
                if cleared:
                    for donor in ("donor-a", "donor-b"):
                        storage = Terminal("storage-" + donor, ["./lumabri"], donor)
                        until(lambda: storage.has("What would you like to do?"))
                        storage.send("/")
                        until(lambda: storage.has("/storage"))
                        storage.send("\x1b[A\r")
                        until(lambda: storage.has("Household weight storage"))
                        storage.send("\x1b[B\r")
                        until(lambda: storage.has("Clear unused household weights?"))
                        storage.send("\x1b[B\r")
                        until(lambda: storage.has("Household weights cleared"))
                        for tree in ("cas", "mirrors"):
                            assert not list((tmp / donor / ".lumabri/home" / tree).iterdir())
                        storage.text = ""; storage.send("\x1b")
                        until(lambda: storage.has("What would you like to do?"))
                        storage.send("\x1b")
                        until(lambda: storage.p.poll() is not None)
                        assert storage.p.returncode == 0
                a.text = b.text = ""
                repeat = Terminal("chatter-refetch" if cleared else "chatter-repeat", base, "chatter-repeat")
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
                assert repeat.p.poll() is None, "cache reuse/refetch plan failed to start"
                # A returning user's catalogue can already contain a measured
                # tok/s value. Only this new turn's Hosted footer proves that
                # generation finished; old catalogue output is not a response.
                repeat.text = ""
                repeat.send("hi\n")
                until(lambda: (repeat.has("hosted stream") and repeat.has("no local checkpoint")) or repeat.p.poll() is not None,
                      seconds=120, message="model did not finish a response after cache reuse/refetch")
                assert repeat.p.poll() is None and (repeat.has("tok/s") or repeat.has("generated tokens"))
                stats = source_stats(repeat)
                if cleared:
                    assert stats["bytes_served"] > metadata_bytes, stats
                    assert cached_weights(), "cleared cache was not rebuilt"
                else:
                    assert stats["reads"] == 2 and stats["bytes_served"] == metadata_bytes, (cold_bytes, stats)
                    assert cached_weights() == before, "the same checkpoint duplicated its persistent weight cache"
                repeat.send("/quit\n")
                until(lambda: repeat.p.poll() is not None)
                assert repeat.p.returncode == 0
                until(lambda: a.has("Released") and b.has("Released"))
                until(leases_released)
                assert not list((tmp / "chatter-repeat").rglob("*.safetensors"))
                if cleared:
                    print("HOME STORAGE: PASS (TUI cleanup, renewed approval, weight refetch, real generation, released leases)", flush=True)
                else:
                    print(f"HOME CACHE: PASS (new approval, same checkpoint cache, source bytes {cold_bytes} -> {stats['bytes_served']}, two config reads remain)", flush=True)
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
