#!/usr/bin/env python3
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from production_check import check


def result(rate, peers=3, calls=None, mode="swarm", model="tiny", family="olmoe"):
    return {
        "mode": mode, "model": model, "family": family,
        "tokens_match_oracle": True,
        "tok_s_single": rate,
        "tok_s_aggregate": {"1": rate, "2": rate * 1.8,
                            "4": rate * 3.0, "8": rate * 4.0},
        "peers": peers,
        "expert_calls_per_peer": ({"donor-a": 10, "donor-b": 0, "donor-c": 0}
                                  if calls is None else calls),
    }


def check_reports(local, single, swarm, **kwargs):
    local["mode"] = "baseline-local"
    single["mode"] = "baseline-single"
    return check(local, single, swarm, **kwargs)


class ProductionCheckTest(unittest.TestCase):
    def test_accepts_two_x_and_active_donor(self):
        got = check_reports(result(1), result(1.2), result(2.5))
        self.assertGreater(got["speedup"], 2)

    def test_rejects_speed_or_oracle_or_idle_donors(self):
        with self.assertRaisesRegex(ValueError, "ratio"):
            check_reports(result(1), result(1), result(1.9))
        broken = result(3)
        broken["tokens_match_oracle"] = False
        with self.assertRaisesRegex(ValueError, "oracle"):
            check_reports(result(1), result(1), broken)
        with self.assertRaisesRegex(ValueError, "no active"):
            check_reports(result(1), result(1), result(3, calls={"a": 0, "b": 0, "c": 0}))

    def test_rejects_nonfinite_rates_and_thresholds(self):
        for invalid in (math.nan, math.inf, -math.inf, True):
            with self.subTest(rate=invalid), self.assertRaisesRegex(ValueError, "tok_s_single"):
                check_reports(result(1), result(1.2), result(invalid))
        invalid_aggregate = result(3)
        invalid_aggregate["tok_s_aggregate"]["2"] = math.nan
        with self.assertRaisesRegex(ValueError, "metric B"):
            check_reports(result(1), result(1.2), invalid_aggregate)
        with self.assertRaisesRegex(ValueError, "ratio is not finite"):
            check_reports(result(1e-308), result(1e-308), result(1e308))
        with self.assertRaisesRegex(ValueError, "thresholds"):
            check_reports(result(1), result(1.2), result(3), minimum_ratio=math.nan)

    def test_rejects_invalid_peer_counts_and_expert_counters(self):
        for peers in (True, 3.9, "3", 2):
            with self.subTest(peers=peers), self.assertRaisesRegex(ValueError, "measured peers"):
                check_reports(result(1), result(1.2), result(3, peers=peers))
        for calls in ({"a": True, "b": 0, "c": 0},
                      {"a": 1.9, "b": 0, "c": 0},
                      {"a": "2", "b": 0, "c": 0},
                      {"a": -1, "b": 0, "c": 0}):
            with self.subTest(calls=calls), self.assertRaisesRegex(ValueError, "integer counters"):
                check_reports(result(1), result(1.2), result(3, calls=calls))
        with self.assertRaisesRegex(ValueError, "peer count differs"):
            check_reports(result(1), result(1.2), result(3, calls={"a": 2}))

    def test_rejects_mixed_report_identities(self):
        for field, value, message in (("mode", "baseline-local", "mode"),
                                      ("model", "another", "model or family"),
                                      ("family", "glm", "model or family")):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, message):
                local = result(1, mode="baseline-local")
                single = result(1.2, mode="baseline-single")
                swarm = result(3)
                swarm[field] = value
                check(local, single, swarm)
        with self.assertRaisesRegex(ValueError, "missing model or family"):
            check(result(1, mode="baseline-local"),
                  result(1.2, mode="baseline-single"), result(3, model=""))

    def test_cli_fails_closed_on_nonfinite_json_metric(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = [root / name for name in ("local.json", "single.json", "swarm.json")]
            for path, report in zip(paths, (result(1, mode="baseline-local"),
                                            result(1.2, mode="baseline-single"),
                                            result(math.nan))):
                path.write_text(json.dumps(report), encoding="utf-8")
            output = root / "decision.json"
            proc = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("production_check.py")),
                 "--local", str(paths[0]), "--single", str(paths[1]),
                 "--swarm", str(paths[2]), "--output", str(output)],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(proc.returncode, 1)
            decision = json.loads(output.read_text(encoding="utf-8"))
            self.assertFalse(decision["ok"])
            self.assertIn("tok_s_single", decision["error"])


if __name__ == "__main__":
    unittest.main()
