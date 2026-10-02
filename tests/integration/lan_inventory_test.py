"""Real tracker + workers + CLI/TUI; loopback protocol gate, not a physical LAN benchmark."""
import json
import errno
import os
import fcntl
from pathlib import Path
import pty
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
    children, logs = [], []
    terminal = None
    with tempfile.TemporaryDirectory(prefix="lumabri-lan-inventory-") as tmp:
        tmp = Path(tmp)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        addr = f"127.0.0.1:{port}"

        def env(name):
            home = tmp / name
            home.mkdir(exist_ok=True)
            result = {**os.environ, "HOME": str(home), "LUMABRI_ENCRYPT": "1",
                    "LUMABRI_PEER_KEY": str(home / "peer.key"),
                    "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts"),
                    "LUMABRI_TOKEN": "inventory-test", "LUMABRI_NO_DISK_PROBE": "1",
                    "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_IO_TIMEOUT_MS": "20000"}
            for key in ("LUMABRI_COST_PER_HOUR", "LUMABRI_COST_CURRENCY", "LUMABRI_ESTIMATED_POWER_WATTS"):
                result.pop(key, None)
            if name == "a":
                result.update(LUMABRI_COST_PER_HOUR="0", LUMABRI_COST_CURRENCY="EUR",
                              LUMABRI_ESTIMATED_POWER_WATTS="12.5")
            return result

        def start(name, args, identity=None):
            log = open(tmp / f"{name}.log", "w+")
            logs.append(log)
            p = subprocess.Popen(args, cwd=ROOT, env=env(identity or name),
                                 stdout=log, stderr=subprocess.STDOUT)
            children.append(p)
            return p

        models = tmp / "models" / "tiny"
        models.mkdir(parents=True)
        (models / "config.json").write_text(json.dumps({
            "model_type": "olmoe", "num_hidden_layers": 4, "hidden_size": 64,
            "intermediate_size": 128, "num_experts": 8, "num_experts_per_tok": 2,
            "num_attention_heads": 4, "num_key_value_heads": 4, "vocab_size": 256}))
        subprocess.run([sys.executable, str(ROOT / "tests/integration/prepare_olmoe_headers.py"),
                        str(models)], check=True)  # header-only, not an inference fixture
        base = ["./lumabri", "models", "--models-dir", str(models.parent), "--tracker", addr]

        def snapshot(identity="observer", success=True):
            p = subprocess.run(base + ["--json"], cwd=ROOT, env=env(identity),
                               capture_output=True, text=True, timeout=15)
            if success:
                assert p.returncode == 0, p.stderr
            return json.loads(p.stdout)

        def until(check, seconds=25):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if check():
                    return
                time.sleep(.2)
            raise AssertionError("inventory condition timed out")

        try:
            tracker = start("tracker", ["./tracker", "--port", str(port),
                                        "--peer-bindings", str(tmp / "bindings")])
            def listening():
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.2):
                        return True
                except OSError:
                    return False
            until(listening)
            a = start("a", ["./lumabri", "worker", "--join", addr, "--name", "home-a", "--ram-gb", "1"])
            b = start("b", ["./lumabri", "worker", "--join", addr, "--name", "home-b", "--ram-gb", "2"])
            until(lambda: len(snapshot()["nodes"]) == 3)
            doc = snapshot()
            for node in doc["nodes"]:
                assert node["cpu_model"] and node["threads"] > 0
                assert node["ram_budget_bytes"] <= node["ram_available_bytes"]
                assert not node["segment_gpu_verified"]
                assert node["runtime_state"] == "unknown"  # inventory workers are not loaded engines
                assert node["resource_facts"]["load_one"] is not None
                assert node["resource_facts"]["energy_joules"] is None
                assert node["resource_facts"]["link_to_edge"] is None
            facts_a = next(n for n in doc["nodes"] if n["name"] == "home-a")["resource_facts"]
            facts_b = next(n for n in doc["nodes"] if n["name"] == "home-b")["resource_facts"]
            assert facts_a["machine_cost"] == {"state": "declared", "micro_units_per_hour": 0, "currency": "EUR"}
            assert facts_a["power"] == {"state": "declared_estimate", "watts": 12.5}
            assert facts_b["machine_cost"] is None and facts_b["power"] is None
            assert next(n for n in doc["nodes"] if n["name"] == "home-a")["ram_budget_bytes"] <= 1e9
            assert not doc["execution_ready"] and doc["models"][0]["calibration"] is None
            assert len(snapshot("b")["nodes"]) == 2  # this computer counted once
            master, slave = pty.openpty()
            terminal = master
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
            live_log = open(tmp / "live.log", "w+"); logs.append(live_log)
            live = subprocess.Popen(base, cwd=ROOT, env=env("observer"),
                                    stdin=slave, stdout=slave, stderr=live_log)
            children.append(live); os.close(slave)
            display = ""

            def screen_has(text):
                nonlocal display
                while select.select([master], [], [], .05)[0]:
                    try:
                        data = os.read(master, 65536)
                    except OSError as exc:
                        if exc.errno != errno.EIO:
                            raise
                        break  # Linux PTY EOF after the child closes its slave.
                    if not data:
                        break
                    display += data.decode("utf-8", errors="replace")
                    if len(display) > 262144:
                        display = display[-131072:]
                # The shared C canvas redraws in place without erasing first.
                # Only the latest frame is evidence of current inventory.
                latest = display.rsplit("\x1b[H", 1)[-1]
                return text in latest

            until(lambda: screen_has("3 computers"))
            os.write(master, b"\t")
            until(lambda: screen_has("home-a") and screen_has("home-b"))
            p = subprocess.run(base + ["--snapshot", "--keys", "\t"], cwd=ROOT,
                               env=env("observer"), capture_output=True, text=True, timeout=15)
            assert p.returncode == 0 and "home-a" in p.stdout and "home-b" in p.stdout
            assert "CORES/THREADS" in p.stdout and "\x1b" not in p.stdout
            # The tab remains useful before any model has been downloaded.
            empty = subprocess.run(["./lumabri", "models", "--models-dir", str(tmp / "empty"),
                                    "--tracker", addr, "--snapshot", "--keys", "\t"],
                                   cwd=ROOT, env=env("observer"), capture_output=True,
                                   text=True, timeout=15)
            assert "home-a" in empty.stdout
            duplicate = start("duplicate", ["./lumabri", "worker", "--join", addr,
                                            "--name", "duplicate", "--ram-gb", "1"], identity="b")
            time.sleep(1)
            assert len(snapshot()["nodes"]) == 3
            assert "duplicate" not in {n["name"] for n in snapshot()["nodes"]}
            duplicate.terminate(); duplicate.wait(timeout=10)
            subprocess.run(["./test_inventory", addr], cwd=ROOT, env=env("bad"), check=True, timeout=15)
            assert len(snapshot()["nodes"]) == 3
            a.send_signal(signal.SIGSTOP)
            until(lambda: "home-a" not in {n["name"] for n in snapshot()["nodes"]}, seconds=25)
            a.send_signal(signal.SIGCONT)
            until(lambda: len(snapshot()["nodes"]) == 3)
            a.terminate(); a.wait(timeout=10)
            until(lambda: len(snapshot()["nodes"]) == 2)
            until(lambda: screen_has("2 computers"))  # automatic background refresh
            tracker.terminate(); tracker.wait(timeout=10)
            offline = snapshot(success=False)
            assert not offline["inventory_ok"] and len(offline["nodes"]) == 1
            assert not any(m["planned"] for m in offline["models"])
            until(lambda: screen_has("TRACKER OFFLINE"))
            os.write(master, b"q")
            # A terminal consumes output while the process exits; a stopped
            # PTY reader can otherwise block its final full-frame write.
            until(lambda: (screen_has("TRACKER OFFLINE"), live.poll() is not None)[1], seconds=5)
            assert live.returncode == 0
            print("LAN INVENTORY: PASS (signed reports, two workers, dedupe, expiry, disconnect, JSON/live TUI)", flush=True)
        finally:
            for p in children:
                if p.poll() is None:
                    p.send_signal(signal.SIGCONT)
                    p.terminate()
            for p in children:
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
            for log in logs:
                log.flush(); log.seek(0)
                if sys.exc_info()[0]:
                    print(log.read()[-4000:])
                log.close()
            if terminal is not None:
                os.close(terminal)


if __name__ == "__main__":
    main()
