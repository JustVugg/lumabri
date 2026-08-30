#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent


class SwarmSoakTest(unittest.TestCase):
    def test_repeats_exact_oracle_and_writes_result(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            chatter = root / "chatter.py"
            chatter.write_text(
                "import json,os\n"
                "assert os.environ['LMB_SOAK_MODE'] == 'swarm'\n"
                "print(json.dumps(dict(token_ids=[7,8], decode_tokens=2, "
                "decode_seconds=0.5, bytes=dict(cas=1,expert=2,segment=3))))\n",
                encoding="utf-8")
            spec = root / "spec.json"
            spec.write_text(json.dumps({
                "model": "tiny", "prompt": "hello", "oracle_token_ids": [7, 8],
                "chatter": {"host": "local",
                            "command": [sys.executable, str(chatter)],
                            "env": {"LMB_SOAK_MODE": "{mode}"}},
            }), encoding="utf-8")
            output = root / "soak.json"
            proc = subprocess.run(
                [sys.executable, str(HERE / "swarm_soak.py"), "--spec", str(spec),
                 "--output", str(output), "--seconds", "1", "--interval", "0.1"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=10, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(result["ok"])
            self.assertGreater(result["runs"], 1)
            self.assertEqual(result["tok_s_median"], 4.0)


if __name__ == "__main__":
    unittest.main()
