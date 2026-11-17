"""A mixed household planned for speed, with no computer chosen by hand.

Real tracker, real sharing services and the real management API on loopback,
with one very small checkpoint. One test host plays five computers: each
declares a different memory offer, and the fixture-only
LUMABRI_TEST_MACHINE_SPEED stands in for the automatic speed probe so that
they also differ in measured speed. Links between them are measured by the
computers themselves, as in a real household.

Checked here:

* every computer publishes its speed and its links without being asked, also
  one that joins later;
* an automatic preparation (an empty computer list) picks the fastest
  computer that holds the whole model, never a split, and says why;
* the estimate of the chosen placement is never below the estimate of the
  best single computer, in every preview;
* a listed pool in which no single computer holds the model is split, and a
  listed pool that cannot hold it at all is refused;
* preparing the automatic plan sends an offer only to the computer the plan
  names, and the model then answers;
* a computer joining or leaving changes the advice and nothing else: the
  loaded model is never moved or unloaded by the planner.

Loopback and a toy checkpoint: this validates the choice and the protocol,
not the accuracy of the estimate on real hardware.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SPLIT_MARGIN = 1.05


def until(condition, message, seconds=60):
    deadline = time.monotonic() + seconds
    while True:
        value = condition()
        if value:
            return value
        assert time.monotonic() < deadline, message
        time.sleep(.25)


class Household:
    def __init__(self, model):
        self.tmp = Path(tempfile.mkdtemp(prefix="lmb-speed-plan-"))
        self.model = model
        self.children = {}
        self.identities = {}
        self.passed = False
        for _ in range(200):
            base = 20000 + 200 * (secrets.randbelow(170))
            taken = []
            try:
                for port in range(base, base + 130):
                    sock = socket.socket(); taken.append(sock); sock.bind(("0.0.0.0", port))
                break
            except OSError:
                continue
            finally:
                for sock in taken:
                    sock.close()
        else:
            raise AssertionError("no free port range")
        self.base = base
        self.port = base + 120
        self.slots = {}
        self.token = self.household_token = None
        self.tracker = f"127.0.0.1:{base}"

    def env(self, name, speed=None):
        home = self.tmp / name
        home.mkdir(exist_ok=True)
        slot = self.slots.setdefault(name, len(self.slots))
        env = {**os.environ, "HOME": str(home), "LUMABRI_PEER_KEY": str(home / "peer.key"),
               "LUMABRI_KNOWN_HOSTS": str(home / "known"), "LUMABRI_ADVERTISE": "127.0.0.1",
               "LUMABRI_HOME_PORT_BASE": str(self.base + 16 * slot), "LUMABRI_NO_DISK_PROBE": "1",
               "LUMABRI_RAM_RESERVE_MB": "256", "LUMABRI_HOME_HYBRID": "0", "LUMABRI_IO_TIMEOUT_MS": "15000",
               "COLI_NO_OMP_TUNE": "1", "OMP_NUM_THREADS": "2", "PIN": "off"}
        for key in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND", "LUMABRI_RESIDENT_REQUIRED",
                    "LUMABRI_RESIDENCY_POLICY", "LUMABRI_COST_PER_HOUR", "LUMABRI_COST_CURRENCY",
                    "LUMABRI_TEST_MACHINE_SPEED"):
            env.pop(key, None)
        if self.household_token:
            env.update(LUMABRI_TOKEN=self.household_token, LUMABRI_ENCRYPT="1")
        if speed:
            env["LUMABRI_TEST_MACHINE_SPEED"] = f"{speed}:{speed}"
        return env

    def run(self, name, *arguments, timeout=90):
        return subprocess.run([str(ROOT / "lumabri"), *arguments], env=self.env(name), text=True,
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout, cwd=ROOT)

    def start_desk(self, speed):
        env = self.env("desk", speed)
        answer = subprocess.run([str(ROOT / "lumabri"), "start", "--no-browser", "--port", str(self.port),
                                 "--ram-gb", "1"], env=env, text=True, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=90, cwd=ROOT)
        assert answer.returncode == 0, (answer.stdout, answer.stderr)
        (link,) = [line.strip() for line in answer.stdout.splitlines() if "#token=" in line]
        self.token = link.split("#token=", 1)[1]
        settings = dict(line.split("=", 1) for line in
                        (self.tmp / "desk/.lumabri/home.conf").read_text().splitlines() if "=" in line)
        assert settings["tracker"] == self.tracker, settings["tracker"]
        self.household_token = settings["token"]
        (self.tmp / "desk/.lumabri/models/tiny").symlink_to(self.model, target_is_directory=True)
        self.identities["desk"] = self.identity("desk")

    def join(self, name, ram_gb, speed):
        env = self.env(name, speed)
        conf = self.tmp / name / ".lumabri"; conf.mkdir(exist_ok=True)
        (conf / "home.conf").write_text(f"tracker={self.tracker}\ntoken={self.household_token}\n"
                                        f"models={self.tmp / 'desk/.lumabri/models'}\nram={ram_gb}\nowner=0\n")
        (conf / "home.conf").chmod(0o600)
        log = open(self.tmp / f"{name}.log", "w")
        self.children[name] = subprocess.Popen(
            [str(ROOT / "lumabri"), "donor", "--headless", "--join", self.tracker, "--name", name,
             "--ram-gb", str(ram_gb)], env=env, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        self.identities[name] = until(lambda: self.identity(name), f"{name} has no identity", 30)

    def identity(self, name):
        answer = self.run(name, "peer-key", timeout=15)
        return answer.stdout.strip() if answer.returncode == 0 and len(answer.stdout.strip()) == 64 else None

    def leave(self, name):
        child = self.children.pop(name)
        child.terminate()
        try:
            child.wait(timeout=20)
        except subprocess.TimeoutExpired:
            child.kill(); child.wait(timeout=10)

    def donor(self, name):
        answer = self.run(name, "service", "status", "--json", timeout=25)
        assert answer.returncode == 0, (name, answer.stderr)
        return {s["role"]: s for s in json.loads(answer.stdout)["services"]}["donor"]

    def call(self, path, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        headers = {"Authorization": "Bearer " + self.token}
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection.request("POST" if body is not None else "GET", path,
                           None if body is None else json.dumps(body), headers)
        response = connection.getresponse(); data = response.read(); connection.close()
        return response.status, data

    def json(self, path, body=None, expected=(200, 202)):
        status, data = self.call(path, body)
        assert status in expected, (path, status, data[:600])
        return json.loads(data)

    def preview(self, nodes, sessions=1, expected=(200,)):
        intent = {"models": ["tiny"], "nodes": nodes, "context": 128, "sessions": sessions, "max_new": 8}
        status, data = self.call("/api/v1/preparation", {"action": "preview", **intent})
        assert status in expected, (status, data[:600])
        return status, json.loads(data), intent

    def advice(self, *extra):
        """The command-line projection of the same decision."""
        answer = self.run("desk", "models", "--tracker", self.tracker, "--models-dir",
                          str(self.tmp / "desk/.lumabri/models"), "--context", "128", "--speed-plan", "tiny",
                          "--json", *extra, timeout=30)
        assert answer.stdout.strip(), (answer.returncode, answer.stderr)
        return json.loads(answer.stdout)

    def stop(self):
        for name in list(self.children):
            try:
                self.leave(name)
            except Exception:
                pass
        for name in self.slots:
            if (self.tmp / name / ".lumabri").is_dir():
                self.run(name, "service", "stop", timeout=60)
        if self.passed:
            shutil.rmtree(self.tmp, ignore_errors=True)
        else:
            print(f"Artifacts kept: {self.tmp}", file=sys.stderr)


def names(candidate):
    return [c["name"] for c in candidate["computers"]]


def check_never_slower(speed):
    """The rule itself, on what the API reports."""
    assert speed["status"] == "chosen", speed
    chosen, singles = speed["chosen"], [c for c in speed["candidates"] if len(c["computers"]) == 1]
    assert len(singles) == speed["computers_that_fit_alone"], speed
    fastest = max((c["estimated_tok_s"] or 0 for c in singles), default=0)
    covered = sorted((c["begin"], c["end"]) for c in chosen["computers"])
    assert covered[0][0] == 0 and all(a[1] == b[0] for a, b in zip(covered, covered[1:])), covered
    assert sum(c["chat_host"] for c in chosen["computers"]) == 1
    assert all(c["device"] == "cpu" and c["reserved_vram_bytes"] == 0 for c in chosen["computers"])
    if not singles:
        assert len(chosen["computers"]) > 1 and speed["why"] == "no_single_computer_fits", speed
    elif len(chosen["computers"]) > 1:
        assert chosen["estimated_tok_s"] and fastest and chosen["estimated_tok_s"] >= fastest * SPLIT_MARGIN, speed
    elif fastest:
        assert chosen["estimated_tok_s"] and chosen["estimated_tok_s"] >= fastest * 0.9 - 1e-9, speed
    return chosen


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--models-dir", required=True, type=Path)
    args = parser.parse_args()
    configs = list(args.models_dir.glob("*/config.json"))
    assert len(configs) == 1, "expected exactly one small checkpoint"
    house = Household(configs[0].parent.resolve())
    try:
        # A slow computer that offers a lot starts the household; a fast one
        # and one too small for the model join it. Nobody measures anything.
        house.start_desk(speed=400)
        house.join("fast", 1, speed=4000)
        house.join("small", 0.2, speed=9000)
        ids = house.identities

        def measured(count):
            doc = house.advice()
            split = doc.get("best_split")
            return doc if (doc["status"] == "chosen" and doc["computers_considered"] == count and
                           not doc["unmeasured"] and split and split["estimated_tok_s"]) else None

        # All three are candidates; "small" cannot hold even a part of the model.
        doc = until(lambda: measured(3), "speeds and links were not published on their own", 90)
        assert doc["computers_that_fit_alone"] == 2
        # Measuring a link checks the other computer against the inventory; it
        # leaves no identity behind in anybody's known_hosts.
        for name in ("desk", "fast", "small"):
            known = house.tmp / name / "known"
            rows = [line.split()[0] for line in known.read_text().splitlines()] if known.exists() else []
            assert set(rows) <= {house.tracker}, (name, rows)
        assert doc["selection"] == "automatic" and doc["execution"] == "cpu" and not doc["vram_budget_used"]
        chosen = check_never_slower(doc)
        assert names(chosen) == ["fast"] and doc["why"] == "fastest_single_computer", doc
        assert doc["best_split"]["estimated_tok_s"] < chosen["estimated_tok_s"]
        assert doc["best_split"]["seconds_per_token"]["network"] > 0 and chosen["seconds_per_token"]["network"] == 0
        assert "not faster" in doc["reason"], doc["reason"]
        text = house.run("desk", "models", "--tracker", house.tracker, "--models-dir",
                         str(house.tmp / "desk/.lumabri/models"), "--context", "128", "--speed-plan", "tiny")
        assert text.returncode == 0 and "Estimated speed:" in text.stdout and "Why: Whole model on fast" in text.stdout \
            and "chat host" in text.stdout, text.stdout

        # The page's preview with no computer chosen: the same decision.
        until(lambda: len([n for n in house.json("/api/v1/workspace")["nodes"] if n["workload"] is not None]) == 3,
              "the page does not see the three computers")
        status, automatic, intent = house.preview([])
        speed = automatic["speed_plan"]
        chosen = check_never_slower(speed)
        assert speed["selection"] == "automatic" and names(chosen) == ["fast"], speed
        plan = automatic["plan"]
        assert plan["decode_tok_s"] is None and len(plan["models"]) == 1
        (piece,) = plan["models"][0]["slices"]
        assert plan["nodes"][piece["node"]]["id"] == ids["fast"] and (piece["begin"], piece["end"]) == (0, 4)
        assert plan["models"][0]["edge_node"] == piece["node"]

        # A manual choice stays possible and is respected.
        status, manual, _ = house.preview([ids["desk"]])
        assert manual["speed_plan"]["selection"] == "listed_computers"
        assert names(check_never_slower(manual["speed_plan"])) != ["fast"]
        (piece,) = manual["plan"]["models"][0]["slices"]
        assert manual["plan"]["nodes"][piece["node"]]["id"] == ids["desk"]
        # Listed together, the two computers that hold the model are not split.
        status, pair, _ = house.preview([ids["desk"], ids["fast"]])
        assert names(check_never_slower(pair["speed_plan"])) == ["fast"]
        # More conversation slots: the same rule, and the shared rate is stated.
        status, shared, _ = house.preview([], sessions=2)
        busy = check_never_slower(shared["speed_plan"])
        assert abs(busy["estimated_tok_s_all_sessions_busy"] * 2 - busy["estimated_tok_s"]) < 1e-3 * busy["estimated_tok_s"]

        # Two more computers join, neither able to hold the model alone: one
        # just short of it, one that takes the rest. Sizes come from the plan.
        whole = chosen["reserved_bytes"]
        house.join("nearly", f"{(whole - 600000) / 1e9:.6f}", speed=2000)
        house.join("rest", 0.25, speed=2000)
        status, split, _ = until(lambda: (lambda r: r if r[0] == 200 else None)(
            house.preview([ids["nearly"], ids["rest"]], expected=(200, 422))),
            "two computers that hold the model together were not planned", 60)
        speed = split["speed_plan"]
        chosen_split = check_never_slower(speed)
        assert speed["computers_that_fit_alone"] == 0 and speed["why"] == "no_single_computer_fits", speed
        assert sorted(names(chosen_split)) == ["nearly", "rest"] and "No single computer" in speed["reason"]
        assert len(split["plan"]["models"][0]["slices"]) == 2
        # Their links are measured without anyone asking, so the split gets a number.
        until(lambda: house.preview([ids["nearly"], ids["rest"]])[1]["speed_plan"]["chosen"]["estimated_tok_s"],
              "the newcomers' link was not measured", 60)
        # One of them alone cannot hold it: an explicit list is refused...
        status, refused, _ = house.preview([ids["rest"]], expected=(422,))
        assert "resident_plan_unavailable" in json.dumps(refused), refused
        # ...and the automatic choice is still the single fast computer.
        status, automatic, intent = house.preview([])
        assert names(check_never_slower(automatic["speed_plan"])) == ["fast"]
        assert automatic["speed_plan"]["computers_considered"] == 5

        # Prepare the automatic plan. Only the named computer is asked.
        operation = secrets.token_hex(32)
        for attempt in range(3):
            status, started = house.call("/api/v1/preparation", {"action": "start", "operation": operation,
                                         "review": automatic["review"], **intent})
            if status != 409:
                break
            status, automatic, intent = house.preview([])   # a changed plan needs a new review
        assert status == 202, (status, started)
        pending = until(lambda: house.donor("fast")["phase"] == 1, "the offer did not reach the chosen computer", 120)
        for other in ("desk", "small", "nearly", "rest"):
            assert house.donor(other)["phase"] == 0 and house.donor(other)["model_count"] == 0, other

        def owner(*parts):
            answer = house.run("fast", "donor-policy", *parts, timeout=20)
            assert answer.returncode == 0, (parts, answer.stdout, answer.stderr)
            return json.loads(answer.stdout)
        accepted = owner("accept-current", owner("inspect")["review"])
        assert accepted["state"] == "accepted", accepted
        state = lambda: house.json("/api/v1/preparation", {"action": "status", "operation": operation})
        final = until(lambda: (lambda s: s if s["state"] in ("done", "failed") else None)(state()),
                      "the approved preparation did not end", 240)
        assert final["state"] == "done" and final["allocations"][0]["state"] == "ready_observed", final
        assert house.donor("fast")["model_count"] == 1
        for other in ("desk", "small", "nearly", "rest"):
            assert house.donor(other)["model_count"] == 0, other
        allocation = final["allocations"][0]["id"]
        status, data = house.call("/api/v1/chat", {"model": allocation, "max_tokens": 8,
                                                   "messages": [{"role": "user", "content": "Count from one to ten."}]})
        assert status == 200, (status, data[:400])
        timing = [json.loads(line[6:]) for line in data.splitlines() if line.startswith(b"data: ") and b"timing" in line]
        assert timing and timing[-1]["timing"]["token_notifications"] == 8, data[-600:]

        # Re-planning is advice. With the model on "fast", nothing better exists.
        running = f"{ids['fast']}:0-4"
        kept = house.advice("--current", running)
        assert kept["replan"]["advice"] == "keep" and kept["replan"]["automatic_action"] is False, kept["replan"]
        assert names(kept["chosen"]) == ["fast"]
        # Had it been on the slow computer, the fast one would be proposed, not taken.
        proposal = house.advice("--current", f"{ids['desk']}:0-4")
        assert proposal["replan"]["advice"] == "propose" and names(proposal["chosen"]) == ["fast"], proposal["replan"]
        assert proposal["replan"]["automatic_action"] is False and "proposal" in proposal["replan"]["reason"]
        assert house.donor("fast")["model_count"] == 1 and house.donor("desk")["model_count"] == 0

        # The computer holding the model leaves. The planner reports it and
        # proposes the slow one; it loads nothing on its own.
        house.leave("fast")
        broken = until(lambda: (lambda d: d if d["status"] == "chosen" and d["replan"] and
                                d["replan"]["advice"] == "current_placement_broken" else None)(
                                    house.advice("--current", running)),
                       "the departure was not noticed", 60)
        assert names(broken["chosen"]) != ["fast"] and broken["replan"]["automatic_action"] is False
        assert "approval" in broken["replan"]["reason"]
        check_never_slower(broken)
        time.sleep(2)
        for other in ("desk", "small", "nearly", "rest"):
            assert house.donor(other)["model_count"] == 0 and house.donor(other)["phase"] == 0, other
        status, after, _ = house.preview([])
        assert names(check_never_slower(after["speed_plan"])) != ["fast"]
        house.passed = True
        print("SPEED PLAN FLOW: PASS (automatic selection, never slower than one computer, split only when "
              "required, refusal, approved preparation on the chosen computer only, re-planning as advice)")
    finally:
        house.stop()


if __name__ == "__main__":
    main()
