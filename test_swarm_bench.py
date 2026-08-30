#!/usr/bin/env python3
import importlib.util
import json
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

    def test_command_uses_last_json_line(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "sample.py"
            script.write_text("print('noise')\nprint('{\\\"ok\\\": true}')\n")
            got = BENCH.run_json("local", ["python3", str(script)], {}, 5)
            self.assertEqual(got, {"ok": True})


if __name__ == "__main__":
    unittest.main()
