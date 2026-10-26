"""Quiescent whole-household previous -> candidate -> previous acceptance.

Only verified installed production binaries run. Four disposable local homes
persist across stages: never restore an old snapshot over newer control state,
delete journals, import live operator homes or reuse an approval. This tests
one version pair with planned downtime, not a live rolling upgrade.
"""
import argparse
import base64
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import pty
import random
import re
import secrets
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import termios
import time

from home_flow_test import TerminalText, approval_frame_ready
from installed_gateway_update_test import candidate_pair
from installed_managed_test import checked_package


def packages(paths):
    # The gateway check additionally requires different revisions AND real
    # executable bytes, and the same engine revision. Both packages must be
    # complete; a gateway-only bundle is not a whole-household upgrade.
    for path in paths:
        checked_package(path)
    return candidate_pair(paths)


class Terminal(TerminalText):
    def __init__(self, fixture, node):
        super().__init__()
        self.closed = False
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
        self.log = open(fixture.root / f"{fixture.stage}-{node}.terminal.log", "wb")
        try:
            self.process = subprocess.Popen([str(fixture.launcher)], cwd=fixture.package,
                env=fixture.env(node), stdin=slave, stdout=slave, stderr=slave)
        finally:
            os.close(slave)

    def drain(self):
        if self.closed:
            return
        while select.select([self.master], [], [], 0)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            self.log.write(data); self.log.flush(); self.feed(data)

    def send(self, text):
        os.write(self.master, text.encode())

    def close(self):
        if self.closed:
            return
        try:
            if self.process.poll() is None:
                self.process.terminate()
                end = time.monotonic() + 10
                # A fullscreen TUI can be blocked writing its PTY. Drain it
                # while waiting; wait() alone can deadlock on terminal output.
                while self.process.poll() is None:
                    self.drain()
                    if time.monotonic() >= end:
                        self.process.kill()  # this fixture's direct TUI child only
                        self.process.wait(timeout=5)
                        raise AssertionError("owned test TUI did not detach")
                    time.sleep(.05)
            self.drain()
        finally:
            self.log.close(); os.close(self.master); self.closed = True


class Fleet:
    nodes = ("owner", "a", "b", "chatter")

    def __init__(self, candidates, models):
        configs = list(models.glob("*/config.json"))
        if len(configs) != 1 or sum(p.stat().st_size for p in configs[0].parent.rglob("*") if p.is_file()) >= 32 << 20:
            raise ValueError("one local reduced checkpoint (<32 MiB) is required")
        self.candidates = candidates
        self.root = Path(tempfile.mkdtemp(prefix="lmb-installed-fleet-"))
        print(f"Artifacts: {self.root}", flush=True)
        self.models = self.root / "models"
        self.models.mkdir()
        (self.models / "model-a").symlink_to(configs[0].parent.resolve(), target_is_directory=True)
        shutil.copytree(configs[0].parent, self.models / "model-b")
        config = self.models / "model-b/config.json"
        contents = json.loads(config.read_text())
        contents["_lumabri_test_variant"] = "fleet-second-checkpoint"
        config.write_text(json.dumps(contents))
        self.terminals = []
        self.base = self.ports()
        self.tracker = f"127.0.0.1:{self.base}"
        self.port = self.base + 80
        self.token = None
        self.routes = []
        self.users = []
        self.history = []
        self.retired = []
        self.operations = []
        self.expected_files = {}
        self.previous_instances = {}
        self.report = {"schema": 1, "scope": "installed_quiescent_household_update_reduced_olmoe",
            "run_complete": False, "live_rolling_upgrade": False, "state_snapshot_restored": False,
            "automatic_approval": False, "stages": [],
            "packages": [{"commit": m["lumabri"]["commit"], "colibri_commit": m["colibri_checkout"]["commit"],
                "manifest_sha256": hashlib.sha256((p / "manifest.json").read_bytes()).hexdigest(),
                "runtime_sha256": {name: entry["sha256"] for name, entry in m["files"].items()
                                  if name.startswith("bin/")}} for p, m, _ in candidates]}

    @staticmethod
    def ports():
        for _ in range(100):
            base = random.randrange(20000, 55000, 100)
            sockets = []
            try:
                for port in (*range(base, base + 64), base + 80):
                    s = socket.socket(); sockets.append(s); s.bind(("0.0.0.0", port))
                return base
            except OSError:
                pass
            finally:
                for s in sockets:
                    s.close()
        raise RuntimeError("no isolated loopback port range")

    def env(self, node):
        home = self.root / node
        (home / ".lumabri").mkdir(parents=True, exist_ok=True)
        settings = home / ".lumabri/home.conf"
        if not settings.exists():
            settings.write_text(f"tracker={self.tracker}\ntoken=fleet-test\nmodels={self.models}\nram=1.5\nowner={int(node == 'owner')}\n")
            settings.chmod(0o600)
        result = {**os.environ, "HOME": str(home), "LUMABRI_TOKEN": "fleet-test",
            "LUMABRI_ENCRYPT": "1", "LUMABRI_RESIDENT_REQUIRED": "1", "LUMABRI_HOME_HYBRID": "0",
            "LUMABRI_RESIDENCY_POLICY": "engine-locked", "LUMABRI_ADVERTISE": "127.0.0.1",
            "LUMABRI_HOME_PORT_BASE": str(self.base + 16 * self.nodes.index(node)),
            "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_NO_DISK_PROBE": "1",
            "LUMABRI_PEER_KEY": str(home / "peer.key"), "LUMABRI_KNOWN_HOSTS": str(home / "known.hosts"),
            "LUMABRI_IO_TIMEOUT_MS": "15000", "COLI_NO_OMP_TUNE": "1", "OMP_NUM_THREADS": "2",
            "PIN": "off", "LUMABRI_COST_PER_HOUR": "0.10" if node == "a" else "0.30",
            "LUMABRI_COST_CURRENCY": "EUR"}
        result.pop("LUMABRI_HOME_FOREGROUND", None)
        return result

    def cli(self, node, *args):
        p = subprocess.run([str(self.launcher), *args], env=self.env(node), cwd=self.package,
                           text=True, capture_output=True, timeout=40)
        assert p.returncode == 0, (node, args[:2], p.returncode, p.stderr)
        return p.stdout.strip()

    def api(self, *args):
        return self.cli("chatter", "api", *args)

    def service(self, node):
        return {s["role"]: s for s in json.loads(self.cli(node, "service", "status", "--json"))["services"]}

    def until(self, check, reason, seconds=45):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for terminal in self.terminals:
                terminal.drain()
            if check():
                return
            time.sleep(.1)
        raise AssertionError(reason)

    def http(self, path, body=None, token=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            connection.request("POST" if body is not None else "GET", path,
                None if body is None else json.dumps(body), {"Authorization": "Bearer " + (token or self.token),
                                                           "Content-Type": "application/json"})
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def turn(self, route, token, expected=200):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        response = None
        try:
            connection.request("POST", "/api/v1/chat",
                json.dumps({"model": route, "max_tokens": 8,
                    "messages": [{"role": "user", "content": "Count from one to ten."}]}),
                {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            response = connection.getresponse()
            assert response.status == expected, (response.status, response.read() if response.status != expected else "")
            if expected != 200:
                return
            assert response.getheader("Content-Type") == "text/event-stream"
            event = None; done = False; text = bytearray()
            while True:
                line = response.readline()
                if not line:
                    break
                assert len(line) <= 1 << 20
                if line.startswith(b"event: "):
                    event = line[7:].strip()
                elif line.startswith(b"data: "):
                    assert not done
                    value = json.loads(line[6:])
                    if event == b"delta":
                        text.extend(base64.b64decode(value["bytes"], validate=True))
                    else:
                        assert event == b"done" and value["timing"]["token_notifications"] == 8
                        done = True
            assert text and done
            return text.decode("utf-8", errors="replace")
        finally:
            if response:
                response.close()
            connection.close()

    def start(self):
        owner = Terminal(self, "owner"); self.terminals.append(owner)
        self.until(lambda: "your workspace" in owner.text, "owner TUI did not start")
        owner.send("\x1b")
        self.until(lambda: owner.process.poll() is not None, "owner TUI did not detach")
        self.donors = {}
        for node in ("a", "b"):
            terminal = Terminal(self, node); self.terminals.append(terminal); self.donors[node] = terminal
            self.until(lambda: "your workspace" in terminal.text, "donor TUI did not start")
            terminal.send("\x1b[B" * 3 + "\r")
            self.until(lambda: "share resources" in terminal.text, "donor did not open sharing")
        if self.token is None:
            self.token = self.api("user-add", "fleet-manager")
            self.api("manage-grant", "fleet-manager")
            self.users = [self.api("user-add", name) for name in ("fleet-a", "fleet-b")]
            self.revoked = self.api("user-add", "fleet-revoked")
            self.api("revoke", "fleet-revoked")
        self.api("start", "--tracker", self.tracker, "--port", str(self.port))
        self.until(lambda: self.http("/api/v1/workspace")[1]["inventory_ok"] and
                   len([n for n in self.http("/api/v1/workspace")[1]["nodes"]
                        if n["workload"] is not None and n["runtime_threads"]]) == 2,
                   "installed donor inventory did not converge")
        assert self.http("/api/v1/workspace", token=self.users[0])[0] == 403
        assert self.http("/api/v1/models", token=self.revoked)[0] == 401
        # Every old allocation is retired. Merely starting a different binary
        # must not resurrect requests, approvals or charged model reservations.
        for node in ("a", "b"):
            state = self.service(node)["donor"]
            assert state["live"] and state["model_count"] == state["reserved_total_bytes"] == 0
        assert not self.service("chatter")["prepare"]["live"]
        for path, digest in self.expected_files.items():
            assert path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == digest, (
                "upgrade altered retained durable data", str(path.relative_to(self.root)))
        for node in self.nodes:
            current = {role: state["instance"] for role, state in self.service(node).items() if state["live"]}
            for role, instance in current.items():
                assert instance != self.previous_instances.get((node, role)), "old keeper survived whole-service switch"
        for old in self.retired:
            assert json.loads(self.api("retire", old, "--tracker", self.tracker))["state"] == "released"
        for operation in self.operations:
            code, result = self.http("/api/v1/preparation", {"action": "status", "operation": operation})
            assert code == 200 and result["state"] == "done" and not result["approval_replayed"]
        self.check_history()

    def prepare(self):
        code, workspace = self.http("/api/v1/workspace")
        assert code == 200
        nodes = [n["id"] for n in workspace["nodes"] if n["workload"] is not None and n["runtime_threads"]]
        intent = {"models": ["model-a", "model-b"], "nodes": nodes, "context": 128, "sessions": 2, "max_new": 8}
        code, preview = self.http("/api/v1/preparation", {"action": "preview", **intent})
        assert code == 200 and preview["plan"]["requires_approval"], preview
        operation = secrets.token_hex(32)
        code, result = self.http("/api/v1/preparation",
            {"action": "start", "operation": operation, "review": preview["review"], **intent})
        assert code == 202, result
        handled = set()
        def approve():
            for node, terminal in self.donors.items():
                state = self.service(node)["donor"]
                key = (node, state["model"])
                if state["phase"] == 1 and key not in handled and approval_frame_ready(terminal, state["model"]):
                    handled.add(key)
                    terminal.send("\x1b[A\r")
            status, state = self.http("/api/v1/preparation", {"action": "status", "operation": operation})
            assert status == 200 and state["state"] != "failed", state
            return state["state"] == "done"
        self.until(approve, "fresh approved preparation did not finish", 180)
        code, receipt = self.http("/api/v1/preparation", {"action": "status", "operation": operation})
        assert code == 200 and len(receipt["allocations"]) == 2 and not receipt["approval_replayed"]
        assert len(handled) == 2, "every co-resident model requires a fresh approval"
        assert sum(self.service(n)["donor"]["model_count"] == 2 for n in ("a", "b")) == 1
        allocations = [a["id"] for a in receipt["allocations"]]
        assert not set(allocations) & set(self.retired), "reused a retired allocation"
        for i, allocation in enumerate(allocations):
            if not self.routes or i >= len(self.routes):
                route = json.loads(self.api("model-add", f"fleet-model-{i}", allocation, "--tracker", self.tracker))
                self.routes.append(route)
                self.api("grant", ("fleet-a", "fleet-b")[i], route["id"], "--tracker", self.tracker)
            else:
                route = json.loads(self.api("model-set", self.routes[i]["id"], allocation, "--tracker", self.tracker))
                assert route["id"] == self.routes[i]["id"] and route["revision"] > self.routes[i]["revision"]
                self.routes[i] = route
        self.operations.append(operation)
        return allocations

    def check_history(self):
        for i, history in enumerate(self.history):
            path = "/api/v1/conversations/" + history["id"]
            assert self.http(path, token=self.users[i]) == (200, history)
            assert self.http(path, token=self.users[1-i])[0] == 404
            assert self.http(path)[0] == 404
            assert self.http(path, {"revision": history["revision"] + 1, "conversation": history["conversation"]},
                             self.users[i])[0] == 409

    def exercise(self):
        for i, (route, token) in enumerate(zip(self.routes, self.users)):
            answer = self.turn(route["id"], token)
            self.turn(self.routes[1-i]["id"], token, expected=403)
            conversation = {"model": route["id"], "title": "Installed fleet real reply", "state": "complete",
                "messages": [{"role": "user", "content": "Count from one to ten."},
                             {"role": "assistant", "content": answer}]}
            if i >= len(self.history):
                code, saved = self.http("/api/v1/conversations", {"revision": 0, "conversation": conversation}, token)
                assert code == 200
                self.history.append(saved)
            else:
                old = self.history[i]
                code, saved = self.http("/api/v1/conversations/" + old["id"],
                    {"revision": old["revision"], "conversation": conversation}, token)
                assert code == 200 and saved["revision"] > old["revision"]
                self.history[i] = saved
        self.check_history()

    def idle(self):
        code, demand = self.http("/api/v1/demand")
        assert code == 200 and not demand["telemetry_gaps"]
        return all(not m["admitted"] and not m["opening"] for m in demand["models"])

    def retire(self, allocations):
        self.until(self.idle, "turns did not drain", 15)
        for allocation in allocations:
            self.until(lambda: json.loads(self.api("retire", allocation, "--tracker", self.tracker))["state"] == "released",
                       "allocation did not release before fleet shutdown", 60)
        for node in ("a", "b"):
            donor = self.service(node)["donor"]
            assert donor["model_count"] == donor["reserved_total_bytes"] == 0
        self.retired.extend(allocations)
        self.check_history()

    def stop(self):
        errors = []
        for terminal in self.terminals:
            try:
                terminal.close()
            except Exception as error:
                errors.append(error)
        self.terminals.clear()
        for node in ("chatter", "a", "b", "owner"):
            try:
                self.cli(node, "service", "stop")
                assert not any(s["live"] or s["state"] == "interrupted" for s in self.service(node).values())
                # Status is only an observation; acquire every actual singleton
                # lock too. Do not infer process exit from a missing socket.
                for lock in (self.root / node / ".lumabri/service").glob("*.lock"):
                    with lock.open("rb") as guard:
                        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
                for lock in (self.root / node / ".lumabri/service/allocation-lifetimes").rglob("*.lifetime"):
                    assert lock.stat().st_size == 188 and lock.read_bytes().startswith(b"LMBLIF1\0")
                    with lock.open("rb") as guard:
                        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except Exception as error:
                errors.append(error)
        if errors:
            raise errors[0]

    def retain(self):
        paths = []
        for node in self.nodes:
            home = self.root / node
            paths.extend(p for p in (home / "peer.key", home / "known.hosts", home / ".lumabri/home.conf") if p.is_file())
            for directory in ("api-access/retirements", "api-access/preparations", "api-access/models",
                              "api-access/history", "resident-plans", "service/retired-allocations",
                              "service/allocation-lifetimes"):
                paths.extend(p for p in (home / ".lumabri" / directory).rglob("*") if p.is_file() and p.suffix != ".lock")
            access = home / ".lumabri/api-access"
            paths.extend(p for p in access.glob("*") if p.suffix in (".user", ".manager", ".operator"))
        self.expected_files = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
        assert len(self.expected_files) >= 20

    def locks(self):
        count = 0
        for node in ("a", "b"):
            path = self.root / node / ".lumabri/home/engines.log"
            log = path.read_text() if path.exists() else ""
            prepared = re.findall(r"\[engine-memory\] allocations=(\d+) requested_bytes=(\d+) locked_bytes=(\d+) retained_locked_bytes=(\d+)", log)
            ready = re.findall(r"event=ready policy=engine-locked fault=0", log)
            assert len(prepared) == len(ready)
            assert all(int(n) > 0 and 0 < int(wanted) <= int(locked) for n, wanted, locked, retained in prepared)
            assert "event=ready policy=observe" not in log and "event=lost" not in log
            count += len(ready)
        return count

    def run(self):
        try:
            for label, index in (("previous", 0), ("upgrade", 1), ("rollback", 0)):
                self.stage = label
                self.package, manifest, launcher = self.candidates[index]
                self.launcher = self.package / launcher
                self.start()
                allocations = self.prepare()
                self.exercise()
                instances = {(node, role): s["instance"] for node in self.nodes
                             for role, s in self.service(node).items() if s["live"]}
                self.retire(allocations)
                self.stop()
                self.retain()
                self.previous_instances = instances
                count = self.locks()
                assert count == 4 * (len(self.report["stages"]) + 1), count
                self.report["stages"].append({"stage": label, "package": index,
                    "completed_allocations": allocations, "fresh_approvals": 2,
                    "models": [m["id"] for m in self.routes], "history_revisions": [h["revision"] for h in self.history],
                    "retained_durable_files": len(self.expected_files), "all_services_stopped": True,
                    "cumulative_locked_engines": count})
                self.save()
                print(f"INSTALLED FLEET: {label} PASS (two real models, fresh consent, persistent identity/history, complete retirement and stop)", flush=True)
            assert packages([p for p, _, _ in self.candidates]) == self.candidates
            self.report["run_complete"] = True
        finally:
            if getattr(self, "launcher", None):
                try:
                    self.stop()
                except BaseException:
                    self.report["run_complete"] = False
                    raise
                finally:
                    self.save()
        print("INSTALLED FLEET: PASS (previous -> candidate -> previous; entire service, same homes, no restored snapshot or deleted journals)", flush=True)

    def save(self):
        (self.root / "installed-fleet-update.json").write_text(json.dumps(self.report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--previous-package", type=Path, required=True)
    parser.add_argument("--candidate-package", type=Path, required=True)
    parser.add_argument("--models-dir", type=Path, required=True)
    args = parser.parse_args()
    candidates = packages([args.previous_package, args.candidate_package])
    Fleet(candidates, args.models_dir).run()


if __name__ == "__main__":
    main()
