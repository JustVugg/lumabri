"""A missing runtime/fixture is an explicit skip, never a passing gate."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Prerequisites(unittest.TestCase):
    def test_missing_inputs_are_not_success(self):
        with tempfile.TemporaryDirectory(prefix="lmb-missing-inputs-") as tmp:
            root = Path(tmp)
            scripts = root / "tests/integration"
            scripts.mkdir(parents=True)
            for name in ("segment_budget_test.sh", "multi_session_test.sh"):
                with self.subTest(script=name):
                    target = scripts / name
                    shutil.copyfile(ROOT / "tests/integration" / name, target)
                    result = subprocess.run(["bash", str(target)], cwd=root,
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
                    self.assertIn(": SKIP", result.stdout)
                    self.assertNotIn(": PASS", result.stdout)

    def test_fixture_interpreter_override(self):
        # Dry run only: no dependency installation or new checkpoint.
        for interpreter in ("python3", "/explicit/venv/bin/python3"):
            with self.subTest(interpreter=interpreter):
                env = {**os.environ}
                env.pop("MAKEFLAGS", None)
                env.pop("MFLAGS", None)
                result = subprocess.run(
                    ["make", "--no-print-directory", "-n", "-B", "fixture", f"PYTHON={interpreter}"],
                    cwd=ROOT, env=env, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"{interpreter} make_tiny_olmoe.py tiny_olmoe", result.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
