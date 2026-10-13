#!/usr/bin/env python3
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("swarm_bench", HERE / "swarm_bench.py")
BENCH = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(BENCH)


class SwarmBenchTest(unittest.TestCase):
    def test_valid_sample(self):
        got = BENCH.validate_sample({
            "token_ids": [7, 8], "decode_tokens": 2, "decode_seconds": 0.5,
            "bytes": {"cas": 1, "expert": 2, "segment": 3},
        }, [7, 8])
        self.assertEqual(got["tok_s"], 4.0)

    def test_oracle_mismatch_invalidates_run(self):
        with self.assertRaisesRegex(ValueError, "differ from oracle"):
            BENCH.validate_sample({
                "token_ids": [7], "decode_tokens": 1, "decode_seconds": 1.0,
            }, [8])

    def test_boolean_values_cannot_pass_integer_contract(self):
        sample = {"token_ids": [True], "decode_tokens": 1,
                  "decode_seconds": 1.0, "bytes": {"cas": 0}}
        with self.assertRaisesRegex(ValueError, "token_ids"):
            BENCH.validate_sample(sample, [1])
        sample["token_ids"] = [1]
        sample["decode_tokens"] = True
        with self.assertRaisesRegex(ValueError, "decode_tokens"):
            BENCH.validate_sample(sample, [1])
        sample["decode_tokens"] = 1
        sample["decode_seconds"] = True
        with self.assertRaisesRegex(ValueError, "decode_seconds"):
            BENCH.validate_sample(sample, [1])
        sample["decode_seconds"] = 1.0
        sample["bytes"]["cas"] = True
        with self.assertRaisesRegex(ValueError, "bytes.cas"):
            BENCH.validate_sample(sample, [1])
        with self.assertRaisesRegex(ValueError, "oracle_token_ids"):
            BENCH.validate_oracle([True])

    def test_invalid_byte_counter_object_is_rejected(self):
        sample = {"token_ids": [1], "decode_tokens": 1,
                  "decode_seconds": 1.0, "bytes": None}
        with self.assertRaisesRegex(ValueError, "bytes must be an object"):
            BENCH.validate_sample(sample, [1])

    def test_probe_rejects_invalid_expert_counters(self):
        self.assertEqual(BENCH.validate_peer_counters({"peers": [
            {"name": "a", "expert": {"calls": 2}},
            {"name": "b", "expert": {"calls": 0}}]}), {"a": 2, "b": 0})
        for value in (True, 1.9, "2", -1):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "expert.calls"):
                BENCH.validate_peer_counters({"peers": [
                    {"name": "a", "expert": {"calls": value}}]})
        with self.assertRaisesRegex(ValueError, "unique"):
            BENCH.validate_peer_counters({"peers": [{"name": "a"}, {"name": "a"}]})

    def test_unrepresentable_decode_rate_is_rejected(self):
        sample = {"token_ids": [1], "decode_tokens": 1,
                  "decode_seconds": 5e-324}
        with self.assertRaisesRegex(ValueError, "decode rate"):
            BENCH.validate_sample(sample, [1])
        sample["decode_seconds"] = 10**1000
        with self.assertRaisesRegex(ValueError, "decode_seconds"):
            BENCH.validate_sample(sample, [1])

    def test_command_uses_last_json_line(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "sample.py"
            script.write_text("print('noise')\nprint('{\\\"ok\\\": true}')\n")
            got = BENCH.run_json("local", [sys.executable, str(script)], {}, 5)
            self.assertEqual(got, {"ok": True})

    def test_environment_is_explicit_and_rendered(self):
        got = BENCH.run_json(
            "local",
            [sys.executable, "-c",
             "import json,os; print(json.dumps(dict(v=os.environ['LMB_BENCH_VALUE'])))"],
            {"client": 7}, 5, {"LMB_BENCH_VALUE": "peer-{client}"})
        self.assertEqual(got, {"v": "peer-7"})
        with self.assertRaisesRegex(ValueError, "invalid environment"):
            BENCH.run_json("local", ["true"], {}, 5, {"BAD-NAME": "x"})


if __name__ == "__main__":
    unittest.main()
