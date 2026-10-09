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
import sys
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
    parser.add_argument("--api", action="store_true", help="exercise authenticated API users while the second model stays in its TUI")
    parser.add_argument("--replicas", action="store_true", help="also approve a third allocation and test same-checkpoint routing after selective unload")
    parser.add_argument("--recovery", action="store_true", help="verify visible replay using the test-only real-codec fault proxy")
    parser.add_argument("--joint", choices=("complete", "reject", "cancel"), help="select and prepare two models together from the TUI")
    parser.add_argument("--sessions", type=int, choices=(1, 2, 4, 8), default=1)
    parser.add_argument("--donor-ram-gb", type=float, default=0.5)
    parser.add_argument("--prepare-timeout", type=int, default=180)
    parser.add_argument("--measure-sessions", action="store_true", help="also benchmark the single-slot baseline")
    args = parser.parse_args()
    if args.api and not (args.multi_model and args.keep_requester):
        parser.error("--api requires --multi-model and --keep-requester")
    if args.replicas and not args.api:
        parser.error("--replicas requires --api")
    if args.recovery and (not args.replicas or not (args.runtime_dir / "segment_chat.real").is_file()):
        parser.error("--recovery requires --replicas and a disposable candidate with recovery_codec_proxy.py installed")
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
                # Match the runtime's wildcard listener: loopback-only bind
                # can succeed while another interface already owns this port.
                sock.bind(("0.0.0.0", port))
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
        if args.recovery:
            e["LUMABRI_TEST_RECOVERY_CONTROL"] = str(tmp / "recovery-control.json")
        return e

    class Terminal(TerminalText):
        def __init__(self, name, argv=None, program="lumabri"):
            super().__init__()
            self.master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
            self.log = open(tmp / f"{name}-{len(terminals)}.terminal.log", "wb")
            self.p = subprocess.Popen([str(runtime / program), *(argv or [])], cwd=runtime, env=env(name),
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

    def joint_flow(a, b):
        configs = list(args.models_dir.glob("*/config.json"))
        assert len(configs) == 1
        assert sum(p.stat().st_size for p in configs[0].parent.rglob("*") if p.is_file()) < 32 << 20
        combined = tmp / "joint-models"; combined.mkdir()
        (combined / "model-a").symlink_to(configs[0].parent.resolve(), target_is_directory=True)
        shutil.copytree(configs[0].parent, combined / "model-b")
        config = json.loads((combined / "model-b/config.json").read_text())
        config["_lumabri_test_variant"] = "joint-second-checkpoint"
        (combined / "model-b/config.json").write_text(json.dumps(config))
        chat = Terminal("chatter", ["models", "--models-dir", str(combined), "--tracker", tracker,
                                   "--context", "128", "--max-new", "8", "--sessions", str(args.sessions)])
        until(lambda: "3 computers visible" in current_frame(chat), "joint inventory missing")
        chat.send("\t")
        until(lambda: "Nothing is selected automatically" in current_frame(chat), "joint node view missing")
        chat.send("\x1b[B\r")
        until(lambda: "1 selected" in current_frame(chat), "joint first donor selection missing")
        chat.send("\x1b[B\r")
        until(lambda: "2 selected" in current_frame(chat), "joint second donor selection missing")
        chat.send("\t ")
        until(lambda: "[✓] model-" in current_frame(chat), "first model checkbox missing")
        chat.send("\x1b[B ")
        until(lambda: "[✓] model-a" in current_frame(chat) and "[✓] model-b" in current_frame(chat), "second model checkbox missing")
        chat.send("/" + "\x1b[B" * 5 + "\r")
        until(lambda: "Enter prepares this joint plan" in current_frame(chat) and
              "model-a" in current_frame(chat) and "model-b" in current_frame(chat) and
              "layers" in current_frame(chat), "reviewed joint placement missing")
        assert all(service(name)["donor"]["model_count"] == 0 for name in ("a", "b")), "review created allocations"
        chat.send("\r")
        until(lambda: any(service(n)["donor"]["phase"] == 1 for n in ("a", "b")), "joint first offer missing", 120)
        # The keeper owns the batch; the TUI is not its lifecycle owner.
        chat.p.terminate(); until(lambda: chat.p.poll() is not None, "joint requester did not detach")
        library = tmp / "chatter/.lumabri/resident-plans"
        handled = set(); rejected = False; cancelled = False
        def drive():
            nonlocal rejected, cancelled
            ready = len(list(library.glob("*.plan"))) if library.exists() else 0
            for name, terminal in (("a", a), ("b", b)):
                state = service(name)["donor"]
                key = (name, state["model"])
                if state["phase"] != 1 or key in handled:
                    continue
                if "Waiting for your approval" not in current_frame(terminal):
                    continue
                handled.add(key)
                if ready and args.joint == "reject":
                    terminal.send("\r"); rejected = True
                elif ready and args.joint == "cancel":
                    result = subprocess.run([str(runtime / "test_chat_ui"), "cancel-prepare"],
                        env=env("chatter"), text=True, capture_output=True, timeout=20)
                    assert result.returncode == 0, result.stderr
                    cancelled = True
                else:
                    terminal.send("\x1b[A\r")
            return service("chatter")["prepare"].get("operation_state") in ("done", "failed")
        until(drive, "joint preparation did not finish", args.prepare_timeout)
        result = service("chatter")["prepare"]
        expected = 2 if args.joint == "complete" else 1
        assert result["operation_state"] == ("done" if args.joint == "complete" else "failed"), result
        plans = sorted(library.glob("*.plan"))
        assert len(plans) == expected, (result, plans)
        assert f"{expected}/2 ready" in result["detail"], result
        assert args.joint != "reject" or rejected
        assert args.joint != "cancel" or cancelled
        retained = sum(service(name)["donor"]["model_count"] for name in ("a", "b"))
        assert retained == expected, "incomplete allocation survived or a ready allocation was unloaded"
        service("chatter", "restart")
        assert not list((tmp / "chatter").rglob("*.cal")), "preparation fabricated a speed"
        # READY is local; leased inventory arrives on the next heartbeat.
        # With two preparations, an older one-model report can be sufficient
        # for a turn to begin but change during that turn. The runtime must
        # discard that timing, not label it a stable two-model measurement.
        # Establish the committed workload before testing persistence.
        committed = sorted((service(name)["donor"]["model_count"],
                            service(name)["donor"]["reserved_total_bytes"]) for name in ("a", "b"))
        def joint_inventory_committed():
            result = subprocess.run([str(runtime / "lumabri"), "models", "--models-dir", str(combined),
                                     "--tracker", tracker, "--context", "128", "--sessions", str(args.sessions), "--json"],
                                    env=env("chatter"), text=True, capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr
            reports = [node["workload"] for node in json.loads(result.stdout)["nodes"] if node["workload"] is not None]
            return sorted((w["allocations"], w["reserved_bytes"]) for w in reports) == committed
        until(joint_inventory_committed, "joint committed workload did not reach leased inventory", 30)
        for plan in plans:
            conversation = Terminal("chatter", ["resident-chat", str(plan), tracker], program="test_chat_ui")
            until(lambda: conversation.has("receives the text"), "joint resident host unavailable")
            conversation.send("hello\n")
            until(lambda: hosted_turn_complete(conversation.text), "joint model did not run real inference", 120)
            conversation.send("/quit\n")
            until(lambda: conversation.p.poll() is not None, "joint conversation did not close")
            assert conversation.p.returncode == 0, conversation.text[-3000:]
        observations = list((tmp / "chatter/.lumabri/calibrations").glob("*.cal"))
        assert len(observations) == expected, "first retained-model chat did not create calibration"
        for r in observations:
            raw = r.read_bytes()
            nodes = struct.unpack_from("<I", raw, len(raw) - 36)[0]
            assert assert_stage_record(r, stages=nodes) == 1
        assert not list((tmp / "chatter").rglob("*.safetensors"))
        print(f"JOINT PREPARATION: PASS ({args.joint}, {expected} resident models, detached TUI, real inference)", flush=True)

    try:
        owner = Terminal("owner")
        until(lambda: owner.has("your workspace"), "household did not start")
        owner.send("\x1b")
        until(lambda: owner.p.poll() is not None, "owner TUI did not exit")
        original_tracker = service("owner")["tracker"]
        assert original_tracker["live"], original_tracker
        a, b = sharing("a"), sharing("b")
        if args.joint:
            joint_flow(a, b)
            return
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
        def api_command(*words):
            result = subprocess.run([str(runtime / "lumabri"), "api", *words, "--tracker", tracker],
                                    env=env("chatter"), text=True, capture_output=True, timeout=20)
            assert result.returncode == 0, (words, result.stderr, result.stdout)
            return json.loads(result.stdout)

        if args.keep_requester:
            until(lambda: chat.has("receives the text"), "normal prepared chat did not start")
            chat.send("hi\n")
            until(lambda: hosted_turn_complete(chat.text), "normal prepared chat did not generate", 120)
            records = list((tmp / "chatter/.lumabri/calibrations").glob("*.cal"))
            assert len(records) == 1, "service preparation lost the content/calibration key"
            assert assert_stage_record(records[0]) == 1, "background preparation telemetry lost"
            allocation=api_command("list")["models"][0]["id"]
            initial_nodes=api_command("segments",allocation,"status")["nodes"]
            assert len(initial_nodes)==2 and all(n["sessions"]==1 and n["state"]=="accepting" for n in initial_nodes), initial_nodes
            guards=subprocess.run([str(runtime/"test_chat_ui"),"resident-node-guards",str(record),tracker],
                                  env=env("chatter"),capture_output=True,text=True,timeout=30)
            assert guards.returncode==0,guards.stderr
            outsider=subprocess.run([str(runtime/"lumabri"),"api","segments",allocation,"drain","--tracker",tracker],
                env={**env("chatter"),"LUMABRI_PEER_KEY":str(tmp/"outsider.key")},capture_output=True,text=True,timeout=30)
            assert outsider.returncode and all(n["state"]=="unknown" for n in json.loads(outsider.stdout)["nodes"]), outsider.stderr
            assert api_command("segments",allocation,"status")["nodes"]==initial_nodes, "household token controlled another requester's segments"
            draining_nodes=api_command("segments",allocation,"drain")["nodes"]
            assert all(n["state"]=="draining" and n["sessions"]==1 for n in draining_nodes), draining_nodes
            # Existing KV is deliberately not idle capacity. Resume before a
            # new user turn: retokenization/route changes may legitimately
            # rebuild that turn with a NEW OPEN, which drain must reject.
            resumed_nodes=api_command("segments",allocation,"resume")["nodes"]
            for before_node,after_node in zip(initial_nodes,resumed_nodes):
                assert after_node["state"]=="accepting" and after_node["instance"]==before_node["instance"]
                assert after_node["revision"]==before_node["revision"]+2
            chat.text=""; chat.send("hi again\n")
            until(lambda: hosted_turn_complete(chat.text), "conversation did not resume after segment drain", 120)
            print("SEGMENT CONTROL: PASS (real inherited channel, live KV not idle, same engine resume)",flush=True)
            chat.send("/quit\n")
            until(lambda: chat.p.poll() is not None, "requester chat did not close")
        # Exercise cmd_host's real one-slot startup too. A manually built pool
        # would miss accidentally routing the normal single-slot host to the
        # legacy, non-controllable acceptor.
        saved = api_command("list")["models"]
        assert len(saved) == 1, saved
        allocation = saved[0]["id"]
        initial_host = api_command("replica", allocation, "status")
        assert initial_host["state"] == "accepting"
        api_command("replica", allocation, "drain")
        until(lambda: api_command("replica", allocation, "status")["state"] == "drained",
              "host did not drain", 10)
        resumed_host = api_command("replica", allocation, "resume")
        assert resumed_host["instance"] == initial_host["instance"] and resumed_host["state"] == "accepting"
        assert not resumed_host["weights_unloaded"]
        before = {name: service(name)["donor"] for name in ("a", "b")}
        assert all(x["segment_pid"] and x["phase"] in (4, 6) for x in before.values()), before
        assert all(x["compute"]["enabled"] for x in before.values()), "no private compute admission"
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
        assert all(service(name)["donor"]["compute"]["grants"] > 0 for name in ("a", "b")), "real kernels bypassed compute admission"
        if args.keep_requester:
            assert assert_stage_record(records[0]) == 3, "resident real session did not update both preceding real turns"
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
            # Engine TCP idle timeout is 15 seconds in this fixture. Keep
            # the remote conversation slots alive but let their sockets
            # expire before reusing them, reproducing the native quota leak.
            time.sleep(16)
            records = list((tmp / "chatter/.lumabri/calibrations").glob("*.cal"))
            assert len(records) == 1, "first resident model has no observed speed"
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
            # Joint preview must not allocate memory or replay approvals.
            combined = tmp / "joint-catalogue"; combined.mkdir()
            (combined / configs[0].parent.name).symlink_to(configs[0].parent.resolve(), target_is_directory=True)
            (combined / second_model.name).symlink_to(second_model, target_is_directory=True)
            preview_args = [str(runtime / "lumabri"), "models", "--models-dir", str(combined),
                            "--tracker", tracker, "--context", "128", "--sessions", "2", "--json"]
            observed = []
            def workload_ready(expected=1):
                result = subprocess.run(preview_args, env=env("chatter"), capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stderr
                observed[:] = [n for n in json.loads(result.stdout)["nodes"] if n["workload"] is not None]
                return len(observed) == 2 and all(n["workload"]["allocations"] == expected and
                    n["workload"]["reserved_bytes"] > 0 for n in observed)
            until(workload_ready, "leased workload inventory did not show the retained model", 30)
            for n in observed:
                assert n["workload"]["reserved_bytes"] > 0 and n["workload"]["compute_policy"] == "local_fifo"
                assert len(n["workload"]["allocation_set"]) == 64
                preview_args.extend(["--node", n["identity"]])
            preview_args.extend(["--together", configs[0].parent.name, "--together", second_model.name])
            preview = subprocess.run(preview_args, env=env("chatter"), capture_output=True, text=True, timeout=30)
            assert preview.returncode == 0, (preview.stderr, preview.stdout)
            joint = json.loads(preview.stdout); (tmp / "joint-preview.json").write_text(preview.stdout)
            assert joint["state"] == "joint_resident_candidate" and joint["requires_approval"]
            assert not joint["performance_validated"] and joint["decode_tok_s"] is None
            assert len(joint["models"]) == 2
            assert all(n["added_reserved_bytes"] <= n["offered_ram_bytes"] for n in joint["nodes"])
            assert all(service(name)["donor"]["model_count"] == 1 for name in ("a", "b")), "preview changed allocations"
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
            second_plan = tmp / "second.plan"; shutil.copyfile(record, second_plan); second_plan.chmod(0o600)
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
            def first_calibration_state():
                checked = subprocess.run([str(runtime / "test_chat_ui"), "resident-calibration-state", str(first_plan), tracker],
                    env=env("chatter"), text=True, capture_output=True, timeout=20)
                assert checked.returncode == 0, checked.stderr
                return checked.stdout.strip()
            # The first changed report can still describe a pending offer:
            # both worker reports must contain the committed reservations
            # before expecting the next turn to remain on one workload key.
            preview_args = preview_args[:preview_args.index("--node")]
            until(lambda: workload_ready(2) and sorted(n["workload"]["reserved_bytes"] for n in observed) ==
                  sorted(s["reserved_total_bytes"] for s in second_live.values()),
                  "both committed models have not reached the leased inventory", 30)
            until(lambda: first_calibration_state() == "stale", "adding second model retained first model speed", 30)
            parallel = Terminal("chatter", ["resident-chat", str(first_plan), tracker], program="test_chat_ui")
            until(lambda: parallel.has("receives the text"), "first model could not open a parallel conversation")
            prior_grants = {name: service(name)["donor"]["compute"]["grants"] for name in ("a", "b")}
            another.text = ""; another.send("beta\n"); parallel.send("alpha\n")
            until(lambda: hosted_turn_complete(another.text) and hosted_turn_complete(parallel.text),
                  "concurrent independent models failed to generate", 120)
            assert all(service(name)["donor"]["compute"]["grants"] > prior_grants[name] for name in ("a", "b")), "mixed model turns bypassed node compute admission"
            parallel.send("/quit\n")
            until(lambda: parallel.p.poll() is not None, "parallel first-model chat did not close")
            assert first_calibration_state() == "current", ("new mixed-workload observation not bound to current models", parallel.text[-4000:])
            assert assert_stage_record(records[0]) == 1, "changed workload inherited previous observation count"
            if args.api:
                from resident_api_test import verify_resident_api
                verify_resident_api(runtime, env("chatter"), tracker, names[4], tmp, records[0])
            if args.replicas:
                # Same byte-for-byte checkpoint, but a separate approval and
                # signed routing root. Never copy a production model here.
                third_models = tmp / "replica-models"
                shutil.copytree(configs[0].parent, third_models / "replica-checkpoint")
                replica = Terminal("chatter", ["models", "--models-dir", str(third_models), "--tracker", tracker,
                                               "--context", "128", "--max-new", "8", "--sessions", "2"])
                until(lambda: "3 computers visible" in current_frame(replica), "replica inventory missing")
                replica.send("\t")
                until(lambda: "Nothing is selected automatically" in current_frame(replica), "replica selection missing")
                replica.send("\x1b[B\r")
                until(lambda: "1 selected" in current_frame(replica), "replica first donor missing")
                replica.send("\x1b[B\r")
                until(lambda: "2 selected" in current_frame(replica), "replica second donor missing")
                replica.send("\t\r")
                until(lambda: "Plan: resident" in current_frame(replica) and current_frame(replica).count("GB reserved") == 2,
                      "third allocation did not fit beside two tiny models")
                replica.send("\r")
                until(lambda: "Waiting for your approval" in current_frame(a) and "Waiting for your approval" in current_frame(b),
                      "replica missing independent donor approval", 150)
                a.send("\x1b[A\r"); b.send("\x1b[A\r")
                until(lambda: replica.has("receives the text"), "approved replica did not start", args.prepare_timeout)
                third_plan = tmp / "third.plan"; shutil.copyfile(record, third_plan); third_plan.chmod(0o600)
                replica.send("/quit\n")
                until(lambda: replica.p.poll() is not None, "replica TUI did not close")
                replica_live = {name: service(name)["donor"] for name in ("a", "b")}
                assert all(s["model_count"] == 3 for s in replica_live.values())
                # As with the second model, READY precedes the inventory
                # heartbeat. Do not benchmark while the leased workload still
                # describes two allocations or pending (larger) reservations.
                until(lambda: workload_ready(3) and sorted(n["workload"]["reserved_bytes"] for n in observed) ==
                      sorted(s["reserved_total_bytes"] for s in replica_live.values()),
                      "three committed models have not reached the leased inventory", 30)
                from managed_routes_test import verify_managed_routes
                verify_managed_routes(runtime, env("chatter"), tracker, first_plan, third_plan, names[4], tmp)
            # The authenticated release names the first allocation, not the
            # keeper's currently selected second model.
            if not args.replicas:
                release = subprocess.run([str(runtime / "test_chat_ui"), "resident-release", str(first_plan), tracker],
                                         env=env("chatter"), text=True, capture_output=True, timeout=20)
                assert release.returncode == 0, release.stderr
            assert all(service(name)["donor"]["model_count"] == 1 for name in ("a", "b"))
            if args.replicas:
                # Status describes the currently displayed allocation, which
                # is now the released third model, not the retained second.
                # Verify its original processes and run its still-open chat.
                for name in ("a", "b"):
                    os.kill(second_live[name]["segment_pid"], 0)
            else:
                assert all(service(name)["donor"]["segment_pid"] == second_live[name]["segment_pid"] for name in ("a", "b")), "unload killed the other model"
            # This fixture has a 128-token context. Reset before the third
            # formatted turn; an overflow ERROR is not a successful chat.
            another.text = ""; another.send("/reset\n")
            until(lambda: another.has("new conversation"), "second chat did not reset")
            another.text = ""; another.send("still here?\n")
            until(lambda: hosted_turn_complete(another.text), "second model stopped after first model unloaded", 120)
            another.send("/quit\n")
            until(lambda: another.p.poll() is not None, "second chat did not close")
            # A replica preparation updates the "most recent plan" shortcut.
            # Save the original second plan before opening a third model.
            release = subprocess.run([str(runtime / "test_chat_ui"), "resident-release", str(second_plan), tracker],
                                     env=env("chatter"), text=True, capture_output=True, timeout=20)
            assert release.returncode == 0, release.stderr
            assert all(not service(name)["donor"]["reserved_total_bytes"] for name in ("a", "b"))
            print("MULTI-MODEL SERVICE: PASS (two checkpoints on SAME donors, separate approval and RAM, manager restart, unload first while second chat lives)", flush=True)
            return
        resumed.send("\x1b[B" * 4 + "\r")
        until(lambda: resumed.has("resident models"), "resident library missing")
        resumed.send("r")
        until(lambda: resumed.has("Every donor confirmed"), "library did not authenticate all allocations")
        # The library uses coordinated retirement now, not forced release.
        # Merely opening/cancelling the confirmation must neither drain the
        # host nor create a durable operation or drop any donor reservation.
        prior_host = api_command("replica", allocation, "status")
        prior_donors = {name: service(name)["donor"] for name in ("a", "b")}
        confirmation = "finish admitted chats, then release RAM?"
        retirement_dir = tmp / "chatter/.lumabri/api-access/retirements"
        assert not list(retirement_dir.glob("*.retire"))
        resumed.send("x")
        until(lambda: confirmation in current_frame(resumed), "retirement did not require confirmation")
        resumed.send("\x1b")
        until(lambda: "resident models" in current_frame(resumed) and confirmation not in current_frame(resumed),
              "cancelling retirement did not return to the library")
        unchanged_host = api_command("replica", allocation, "status")
        for key in ("instance", "revision", "state"):
            assert unchanged_host[key] == prior_host[key], "cancelled confirmation changed host admission"
        for name in ("a", "b"):
            unchanged = service(name)["donor"]
            for key in ("instance", "segment_pid", "host_pid", "reserved_total_bytes"):
                assert unchanged[key] == prior_donors[name][key], "cancelled confirmation changed an allocation"
        assert not list(retirement_dir.glob("*.retire")), "cancelled confirmation created a retirement"
        resumed.send("x")
        until(lambda: confirmation in current_frame(resumed), "second retirement confirmation missing")
        resumed.send("\r")
        until(lambda: resumed.has("All donors confirmed release"), "approved allocation release failed")
        receipt = api_command("retire", allocation)
        assert receipt["state"] == "released" and receipt["complete"], "CLI did not reconcile the TUI retirement"
        assert len(list(retirement_dir.glob("*.retire"))) == 1, "TUI retirement did not use the shared journal"
        for name in ("a", "b"):
            state = service(name)["donor"]
            assert state["live"] and not state["segment_pid"] and not state["host_pid"] and state["phase"] == 9, state
        assert len(list((tmp / "chatter/.lumabri/resident-plans").glob("*.plan"))) == 1
        assert not list((tmp / "chatter").rglob("*.safetensors")), "client downloaded a checkpoint"
        print("HOUSEHOLD SERVICE FLOW: PASS (approval survives reconnect, detached preparation, real model after TUI/manager exit and crash, exact live library, cancelled confirmation is inert, shared coordinated retirement)", flush=True)
    finally:
        if sys.exc_info()[0] is not None:
            for log in sorted(tmp.glob("*/.lumabri/home/engines.log")):
                print(f"Engine diagnostics: {log}\n{log.read_text(errors='replace')[-12000:]}", flush=True)
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
