"""Installed gate selection contracts; not simulated inference evidence."""
import copy
from pathlib import Path
import platform
import tempfile
import unittest
from unittest.mock import patch

from installed_managed_test import checked_package, native_package


class InstalledContractTest(unittest.TestCase):
    def test_native_clean_complete_package_only(self):
        launcher = "Lumabri.command" if platform.system() == "Darwin" else "start-lumabri"
        manifest = {"system": platform.system(), "architecture": platform.machine(),
            "lumabri": {"commit": "a" * 40, "tracked_changes": False},
            "colibri_checkout": {"commit": "b" * 40, "tracked_changes": False},
            "files": {name: {} for name in [launcher, *["bin/" + n for n in native_package.BINARIES]]}}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary).resolve()
            with patch.object(native_package, "verify", return_value=manifest) as verifier:
                self.assertEqual(checked_package(directory), (directory, manifest))
                verifier.assert_called_once_with(directory)
            invalid = []
            for key in ("system", "architecture"):
                record = copy.deepcopy(manifest); record[key] = "wrong-native-target"; invalid.append(record)
            for key in ("lumabri", "colibri_checkout"):
                record = copy.deepcopy(manifest); record[key]["tracked_changes"] = True; invalid.append(record)
            for name in (launcher, "bin/lumabri", "bin/segment_node", "bin/segment_chat"):
                record = copy.deepcopy(manifest); del record["files"][name]; invalid.append(record)
            for name in ("bin/test_chat_ui", "bin/segment_chat.real", "tests/helper.py", "build/runtime"):
                record = copy.deepcopy(manifest); record["files"][name] = {}; invalid.append(record)
            for record in invalid:
                with self.subTest(record=record), patch.object(native_package, "verify", return_value=record):
                    with self.assertRaises(ValueError):
                        checked_package(directory)
            with patch.object(native_package, "verify", side_effect=ValueError("altered package")):
                with self.assertRaisesRegex(ValueError, "altered"):
                    checked_package(directory)


if __name__ == "__main__":
    unittest.main()
