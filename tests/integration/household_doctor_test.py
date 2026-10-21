#!/usr/bin/env python3
"""Bounded installed runtime diagnostics, without models or shared services."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--runtime-dir", type=Path, default=Path("."))
options, remaining = parser.parse_known_args()
RUNTIME = options.runtime_dir.resolve(strict=True)
SHIM = "liblumabri.dylib" if platform.system() == "Darwin" else "liblumabri.so"


class HouseholdDoctor(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lmb-doctor-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), OMP_NUM_THREADS="1",
                        LUMABRI_PEER_KEY=str(self.root / "peer.key"),
                        LUMABRI_KNOWN_HOSTS=str(self.root / "known.hosts"),
                        LUMABRI_NO_DISK_PROBE="1")
        self.env.pop("OMP_THREAD_LIMIT", None)

    def check(self, runtime, household=True, env=None):
        result = subprocess.run([str(runtime / "lumabri"), "doctor",
                                 *(["--household"] if household else []), "--json"],
                                env=env or self.env, cwd=self.root, capture_output=True,
                                text=True, timeout=12)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["schema"], 1)
        self.assertEqual(data["scope"], "household-runtime" if household else "legacy-installation")
        self.assertEqual(data["ok"], result.returncode == 0)
        checks = {item["name"]: item for item in data["checks"]}
        return data, checks

    def isolated_layout(self):
        runtime = self.root / "incomplete runtime"
        runtime.mkdir()
        # Copy only the UI so exe_dir resolves here; never rename or edit the
        # caller's installed services. Helpers are not executed by doctor.
        shutil.copy2(RUNTIME / "lumabri", runtime / "lumabri")
        for name in ("tracker", "maintainer", "swarm_probe"):
            (runtime / name).symlink_to(RUNTIME / name)
        shim = RUNTIME / SHIM
        if not shim.exists():
            shim = RUNTIME / ".." / "lib" / "lumabri" / SHIM
        (runtime / SHIM).symlink_to(shim.resolve(strict=True))
        return runtime

    def test_real_runtime_starts_without_models_or_services(self):
        data, checks = self.check(RUNTIME)
        self.assertTrue(data["ok"], data)
        for name in ("segment_node", "segment_chat"):
            check = checks["runtime-" + name]
            self.assertTrue(check["ok"] and check["required"], check)
            self.assertIn("thread capacity 1", check["detail"])
            self.assertIn("no model tested", check["detail"])
        self.assertFalse((self.home / ".lumabri" / "service").exists())
        self.assertFalse((self.home / ".lumabri" / "home").exists())
        # Both executables must honour the runtime's actual OpenMP ceiling.
        env = dict(self.env, OMP_NUM_THREADS="4", OMP_THREAD_LIMIT="1")
        for name in ("segment_node", "segment_chat"):
            value = subprocess.check_output([str(RUNTIME / name), "--thread-capacity"],
                                            env=env, text=True, timeout=5)
            self.assertEqual(value, "1\n")

    def test_missing_engines_fail_only_the_strict_mode(self):
        runtime = self.isolated_layout()
        data, checks = self.check(runtime)
        self.assertFalse(data["ok"])
        for name in ("segment_node", "segment_chat"):
            self.assertTrue(checks["runtime-" + name]["required"])
            self.assertFalse(checks["runtime-" + name]["ok"])
        legacy, checks = self.check(runtime, household=False)
        self.assertTrue(legacy["ok"], legacy)
        self.assertFalse(checks["optional-segment_node"]["required"])
        self.assertFalse(checks["optional-segment_chat"]["required"])

    def test_executable_presence_is_not_startup_success(self):
        runtime = self.isolated_layout()
        for name in ("segment_node", "segment_chat"):
            broken = runtime / name
            broken.write_text("#!/bin/sh\nexit 23\n", encoding="utf-8")
            broken.chmod(0o755)
        data, checks = self.check(runtime)
        self.assertFalse(data["ok"])
        for name in ("segment_node", "segment_chat"):
            self.assertFalse(checks["runtime-" + name]["ok"])
        legacy, _ = self.check(runtime, household=False)
        self.assertTrue(legacy["ok"], legacy)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
