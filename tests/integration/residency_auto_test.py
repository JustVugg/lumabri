"""The memory-lock choice of a household started with `lumabri start`.

An ordinary desktop allows very little locked memory. Three real loads of one
very small generated checkpoint, each in its own household, with the allowance
forced for the whole service tree:

* explicit engine-locked under a low allowance still refuses, with the same
  message, before any weight is loaded;
* the automatic choice under the same allowance loads, answers, and reports
  "not locked" in the start output, the donor log, the service status and the
  page's API for this computer;
* the automatic choice under an allowance that covers the whole reservation
  locks, and reports that.

The third case needs an allowance at least as large as the reservation. Where
the hard limit of this account is lower it is skipped and says so; the rule
itself is covered at unit level by test_memlock. Loopback only.
"""
import contextlib
import http.client
import io
import json
import os
from pathlib import Path
import resource
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
APPROVAL = "/api/v1/donor/approval"
LOW = 1 << 20


def checkpoint(folder):
    """A valid OLMoE-shaped checkpoint of a few megabytes, from the fixture generator."""
    sys.path.insert(0, str(ROOT))
    import make_tiny_olmoe
    make_tiny_olmoe.PRESETS["micro"] = {"hidden_size": 64, "intermediate_size": 64,
                                        "num_experts": 8, "num_experts_per_tok": 4}
    saved = sys.argv
    sys.argv = ["make_tiny_olmoe.py", str(folder), "micro"]
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            make_tiny_olmoe.main()
    finally:
        sys.argv = saved
    return folder


class Household:
    def __init__(self, model, allowance, policy):
        self.home = Path(tempfile.mkdtemp(prefix="lmb-residency-"))
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0)); self.port = reservation.getsockname()[1]
        self.env = {**os.environ, "HOME": str(self.home), "LUMABRI_PEER_KEY": str(self.home / "peer.key"),
                    "LUMABRI_KNOWN_HOSTS": str(self.home / "known"), "LUMABRI_ADVERTISE": "127.0.0.1",
                    "LUMABRI_HOME_PORT_BASE": str(12000 + (self.port % 200) * 160),
                    "LUMABRI_NO_DISK_PROBE": "1", "LUMABRI_RAM_RESERVE_MB": "256"}
        for name in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND", "LUMABRI_RESIDENT_REQUIRED",
                     "LUMABRI_RESIDENCY_POLICY"):
            self.env.pop(name, None)
        if policy:
            self.env["LUMABRI_RESIDENCY_POLICY"] = policy
        self.allowance = allowance
        self.model = model
        self.passed = False

    def run(self, *arguments, timeout=90):
        # Every service this command starts inherits the forced allowance.
        return subprocess.run([str(ROOT / "lumabri"), *arguments], env=self.env, text=True,
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_MEMLOCK,
                                                                    (self.allowance, self.allowance)))

    def call(self, path, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        headers = {"Authorization": "Bearer " + self.token}
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection.request("POST" if body is not None else "GET", path,
                           None if body is None else json.dumps(body), headers)
        response = connection.getresponse(); data = response.read(); connection.close()
        return response.status, data

    def json(self, path, body=None):
        status, data = self.call(path, body)
        assert status in (200, 202), (path, status, data)
        return json.loads(data)

    def donor(self):
        answer = self.run("service", "status", "--json")
        assert answer.returncode == 0, answer.stderr
        return {s["role"]: s for s in json.loads(answer.stdout)["services"]}["donor"]

    def logs(self):
        return "\n".join(p.read_text(errors="replace") for p in sorted(self.home.rglob("*.log")))

    def start(self):
        answer = self.run("start", "--no-browser", "--port", str(self.port))
        assert answer.returncode == 0, (answer.stdout, answer.stderr)
        (link,) = [line.strip() for line in answer.stdout.splitlines() if "#token=" in line]
        self.token = link.split("#token=", 1)[1]
        (self.home / ".lumabri/models/reduced").symlink_to(self.model, target_is_directory=True)
        return answer.stdout

    def prepare(self):
        """Preview, start and accept one load on this computer; returns (plan, final status)."""
        nodes = until(lambda: [n["id"] for n in self.json("/api/v1/workspace")["nodes"]
                               if n["workload"] is not None and n["runtime_threads"]],
                      "this computer did not report its capacity")
        intent = {"models": ["reduced"], "nodes": nodes, "context": 128, "sessions": 1, "max_new": 8}
        preview = self.json("/api/v1/preparation", {"action": "preview", **intent})
        operation = secrets.token_hex(32)
        self.json("/api/v1/preparation", {"action": "start", "operation": operation,
                                          "review": preview["review"], **intent})
        shown = until(lambda: (lambda a: a if a["pending"] else None)(self.json(APPROVAL)),
                      "the donor's request did not reach the page API", 120)
        fence = {"instance": shown["instance"], "allocation": shown["request"]["allocation"],
                 "revision": shown["revision"]}
        accepted = self.json(APPROVAL, {"action": "accept", **fence})
        assert accepted["result"] == "accepted", accepted
        status = lambda: self.json("/api/v1/preparation", {"action": "status", "operation": operation})
        final = until(lambda: (lambda s: s if s["state"] in ("done", "failed") else None)(status()),
                      "the accepted preparation did not end", 240)
        return preview["plan"], shown, final

    def chat(self, allocation):
        status, data = self.call("/api/v1/chat", {"model": allocation, "max_tokens": 8,
                                                  "messages": [{"role": "user", "content": "Count from one to ten."}]})
        assert status == 200, (status, data)
        done = [json.loads(line[6:]) for line in data.splitlines() if line.startswith(b"data: ") and b"timing" in line]
        assert done and done[-1]["timing"]["token_notifications"] == 8, data

    def stop(self):
        stopped = None
        for _ in range(4):
            stopped = self.run("service", "stop")
            if stopped.returncode == 0:
                break
        if stopped.returncode == 0 and self.passed:
            shutil.rmtree(self.home, ignore_errors=True)
        else:
            print(f"Artifacts kept: {self.home}", file=sys.stderr)
        assert stopped.returncode == 0, stopped.stderr


def until(condition, message, seconds=60):
    deadline = time.monotonic() + seconds
    while True:
        value = condition()
        if value:
            return value
        assert time.monotonic() < deadline, message
        time.sleep(.2)


def explicit_refuses(model):
    house = Household(model, LOW, "engine-locked")
    try:
        output = house.start()
        assert "Memory:         engine-locked, requested explicitly" in output, output
        before = house.donor()
        assert before["memory"] == {"mode": "engine-locked", "locked": True, "memlock_limit_bytes": LOW}, before
        plan, shown, final = house.prepare()
        assert shown["request"]["memory_locked"] is True and shown["memory"]["warning"] is False, shown
        assert final["state"] == "failed" and final["chat_access"] == "none_ready", final
        logs = house.logs()
        assert "OS locked-memory limit is too low. Ask this computer's owner to configure the Lumabri service's " \
               "memlock allowance" in logs, logs[-3000:]
        assert f"OS locked-memory allowance (RLIMIT_MEMLOCK) is {LOW} bytes" in logs and \
            "Refusing before loading weights" in logs, logs[-3000:]
        assert "event=ready" not in logs and "automatic policy" not in logs, "an explicit request was weakened"
        after = house.donor()
        assert after["model_count"] == 0 and after["reserved_total_bytes"] == 0 and after["segment_pid"] == 0, after
        assert json.loads(house.call("/api/v1/models")[1])["models"] == []
        house.passed = True
    finally:
        house.stop()
    print("explicit engine-locked under a low allowance: refused, same message")


def automatic(model, allowance, locks):
    house = Household(model, allowance, None)
    try:
        output = house.start()
        if allowance == resource.RLIM_INFINITY:
            assert "Memory:         model memory is locked in RAM" in output, output
        else:
            assert f"Memory:         not locked for models above {allowance >> 20} MiB" in output and \
                "Such models are still loaded fully into RAM" in output and "To lock it:" in output, output
        idle = house.json(APPROVAL)
        assert idle["memory"]["mode"] == "automatic-locked" and idle["memory"]["locked"] is True, idle
        plan, shown, final = house.prepare()
        reserved = plan["models"][0]["slices"][0]["reserved_bytes"]
        assert (allowance == resource.RLIM_INFINITY or reserved <= allowance) == locks, (reserved, allowance)
        assert shown["request"]["memory_locked"] is locks, shown
        assert final["state"] == "done" and final["chat_access"] == "granted", final
        allocation = final["allocations"][0]["id"]
        logs = house.logs(); state = house.donor(); page = house.json(APPROVAL)
        # Resident either way: the engines were started under the resident
        # contract and reported ready only after loading their weights.
        assert state["model_count"] == 1 and state["reserved_total_bytes"] == reserved, state
        if locks:
            assert f"automatic policy=engine-locked allocation={allocation[:12]}" in logs, logs[-3000:]
            assert "event=ready policy=engine-locked fault=0" in logs and "policy=observe" not in logs, logs[-3000:]
            assert state["memory"]["mode"] == "automatic-locked" and state["memory"]["locked"] is True, state
            assert page["memory"]["locked"] is True and "not locked on this computer" not in page["memory"]["detail"], page
        else:
            assert f"automatic policy=not-locked allocation={allocation[:12]} reservation={reserved} bytes " \
                   f"allowance={allowance} bytes" in logs, logs[-3000:]
            assert "Weights are loaded fully into RAM but not locked" in logs
            assert "event=ready policy=observe fault=0" in logs and "policy=engine-locked fault" not in logs, logs[-3000:]
            assert "Refusing before loading weights" not in logs
            assert state["memory"] == {"mode": "automatic-not-locked", "locked": False,
                                       "memlock_limit_bytes": allowance}, state
            memory = page["memory"]
            assert memory["mode"] == "automatic-not-locked" and memory["locked"] is False and memory["warning"] is True, page
            assert memory["detail"].startswith("Memory is not locked on this computer: under memory pressure the "
                                               "system may swap") and "To lock it: " in memory["detail"], memory
            assert page["request"]["memory_locked"] is False, page
            listed = house.run("service", "status")
            assert "(memory not locked)" in listed.stdout, listed.stdout
        house.chat(allocation)
        doctor = house.run("doctor", "--json")
        (check,) = [c for c in json.loads(doctor.stdout)["checks"] if c["name"] == "memory-lock"]
        assert check["required"] is False and ("not locked" in check["detail"]) == (not locks), check
        house.passed = True
    finally:
        house.stop()
    print(f"automatic choice, allowance {allowance}: {'locked' if locks else 'resident, not locked'}")
    return reserved


def main():
    if sys.platform != "linux":
        print("residency automatic choice: Linux allowance semantics only; not claimed on this kernel")
        return
    caps = next(line.split()[1] for line in Path("/proc/self/status").read_text().splitlines()
                if line.startswith("CapEff:"))
    if int(caps, 16) & (1 << 14):
        raise RuntimeError("Run this gate without CAP_IPC_LOCK")
    hard = resource.getrlimit(resource.RLIMIT_MEMLOCK)[1]
    with tempfile.TemporaryDirectory(prefix="lmb-residency-model-") as folder:
        model = checkpoint(Path(folder) / "reduced")
        explicit_refuses(model)
        reserved = automatic(model, LOW, locks=False)
        if hard == resource.RLIM_INFINITY or hard >= reserved:
            automatic(model, hard, locks=True)
            print("residency automatic choice: PASS (explicit refusal, not locked, locked)")
        else:
            print(f"residency automatic choice: PASS (explicit refusal, not locked); the locked case needs an "
                  f"allowance of {reserved} bytes and this account's hard limit is {hard}: covered by test_memlock only")


if __name__ == "__main__":
    main()
