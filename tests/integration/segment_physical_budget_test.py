"""Real node preflight: physical headroom cannot be bypassed by a CLI budget.

No weights are loaded and no network listener is opened. The deliberately
missing model path verifies that refusal happens before adapter open. Native
CI runners have less than 256 GiB RAM (the supported maximum reserve).
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PhysicalBudget(unittest.TestCase):
    def run_node(self, arguments, reserve):
        with tempfile.TemporaryDirectory(prefix="lmb-physical-budget-") as tmp:
            env = {**os.environ, "HOME": tmp, "LUMABRI_PEER_KEY": tmp + "/peer.key",
                   "LUMABRI_KNOWN_HOSTS": tmp + "/known.hosts", "LUMABRI_ENCRYPT": "1",
                   "LUMABRI_ENGINE_BACKEND": "cpu", "OMP_NUM_THREADS": "1",
                   "LUMABRI_SEGMENT_RAM_RESERVE_MB": str(reserve)}
            for key in ("LUMABRI_READY_FD", "LUMABRI_NODE_CONTROL_FD",
                        "LUMABRI_SEGMENT_CLIENT_PK", "LUMABRI_COMPUTE_SOCKET"):
                env.pop(key, None)
            reader, writer = os.pipe()
            try:
                result = subprocess.run(
                    [str(ROOT / "segment_node"), "--engine", "olmoe",
                     "--model-dir", tmp + "/not-a-checkpoint", "--model", "preflight-only",
                     "--range", "0:1", "--port", "47302", "--tracker", "127.0.0.1:1",
                     "--advertise", "127.0.0.1:47302", "--name", "physical-budget",
                     "--model-root", "01" * 32, "--tokenizer-root", "02" * 32,
                     "--context", "64", "--threads", "1", "--sessions", "1",
                     "--preflight-fd", str(writer), *arguments],
                    env=env, pass_fds=(writer,), capture_output=True, text=True, timeout=15)
                os.close(writer)
                writer = -1
                signal = os.read(reader, 16)
            finally:
                os.close(reader)
                if writer >= 0:
                    os.close(writer)
            self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
            self.assertEqual(signal, b"F", "must refuse, never authorize loading with P")
            self.assertNotIn("accepting sessions", result.stderr)
            self.assertNotIn("[resident] layer", result.stderr)
            return result.stderr

    def test_explicit_budget_cannot_consume_reserve(self):
        log = self.run_node(["--memory-limit-mb", "8700"], 262144)
        self.assertIn("no verified available RAM", log)
        self.assertIn("before loading weights", log)

    def test_automatic_budget_does_not_become_unlimited(self):
        self.assertIn("no verified available RAM", self.run_node([], 262144))

    def test_explicit_budget_is_capped_before_checkpoint_guard(self):
        log = self.run_node(["--memory-limit-mb", "1048576",
                             "--model-bytes", "18446744073709551614", "--model-layers", "1"], 256)
        self.assertIn("process budget capped", log)
        self.assertIn("needs about", log)


if __name__ == "__main__":
    unittest.main()
