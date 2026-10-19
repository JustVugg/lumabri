"""Upgrade admission contracts; fake manifests never execute a binary."""
from copy import deepcopy
from pathlib import Path
import platform
import tempfile
import unittest
from unittest.mock import patch

from installed_gateway_update_test import candidate_pair


class InstalledGatewayContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lmb-gateway-contract-")
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.paths = [root / "previous", root / "candidate"]
        for path in self.paths:
            path.mkdir()
        launcher = "Lumabri.command" if platform.system() == "Darwin" else "start-lumabri"
        old = {"system": platform.system(), "architecture": platform.machine(),
               "lumabri": {"commit": "a" * 40, "tracked_changes": False},
               "colibri_checkout": {"commit": "c" * 40, "tracked_changes": False},
               "files": {launcher: {}, "bin/lumabri": {"sha256": "1" * 64}}}
        new = deepcopy(old)
        new["lumabri"]["commit"] = "b" * 40
        new["files"]["bin/lumabri"]["sha256"] = "2" * 64
        self.manifests = [old, new]

    def validate(self):
        with patch("installed_gateway_update_test.native_package.verify", side_effect=self.manifests) as verify:
            result = candidate_pair(self.paths)
            self.assertEqual(verify.call_count, 2)
            return result

    def test_two_different_native_clean_revisions(self):
        self.assertEqual([c[0] for c in self.validate()], self.paths)

    def test_copying_same_bytes_is_not_version_upgrade(self):
        self.manifests[1]["files"]["bin/lumabri"]["sha256"] = "1" * 64
        with self.assertRaisesRegex(ValueError, "identical"):
            self.validate()

    def test_different_build_of_same_revision_is_not_version_upgrade(self):
        self.manifests[1]["lumabri"]["commit"] = "a" * 40
        with self.assertRaisesRegex(ValueError, "identical"):
            self.validate()

    def test_reject_non_native_or_dirty_or_engine_upgrade(self):
        cases = [("system", "other", "native"), ("architecture", "other", "native")]
        for key, value, error in cases:
            with self.subTest(key=key):
                previous = self.manifests[1][key]
                self.manifests[1][key] = value
                with self.assertRaisesRegex(ValueError, error):
                    self.validate()
                self.manifests[1][key] = previous
        for key in ("lumabri", "colibri_checkout"):
            self.manifests[1][key]["tracked_changes"] = True
            with self.assertRaisesRegex(ValueError, "clean"):
                self.validate()
            self.manifests[1][key]["tracked_changes"] = False
        self.manifests[1]["colibri_checkout"]["commit"] = "d" * 40
        with self.assertRaisesRegex(ValueError, "same Colibri"):
            self.validate()

    def test_missing_package_entrypoint(self):
        self.manifests[1]["files"].pop("bin/lumabri")
        with self.assertRaisesRegex(ValueError, "launcher and gateway"):
            self.validate()

    def test_manifest_verification_failure_prevents_execution(self):
        with patch("installed_gateway_update_test.native_package.verify", side_effect=ValueError("changed hash")), \
             patch("installed_gateway_update_test.subprocess.run") as run:
            with self.assertRaisesRegex(ValueError, "changed hash"):
                candidate_pair(self.paths)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
