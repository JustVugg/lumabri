"""Installed gate selection contracts; not simulated inference evidence."""
import copy
import contextlib
import io
from pathlib import Path
import platform
import tempfile
import unittest
from unittest.mock import patch

from household_service_flow_test import parse_args
from installed_managed_test import checked_package, native_package, verify_installed_managed


class InstalledContractTest(unittest.TestCase):
    def test_installed_smoke_and_sustained_duration(self):
        argv = ["--models-dir", "/unused-models", "--installed-package", "/unused-package",
                "--joint", "complete", "--api-prepare", "--donor-ram-gb", "1.5", "--sessions", "2"]
        directory = Path("/unused-package")
        with patch.dict("os.environ", {"LUMABRI_RESIDENCY_POLICY": "engine-locked"}), \
                patch("installed_managed_test.checked_package", return_value=(directory, {})) as checked:
            for seconds in (None, 0, 10, 11, 1800, 3600):
                with self.subTest(seconds=seconds):
                    args, manifest = parse_args(argv + ([] if seconds is None else ["--soak-seconds", str(seconds)]))
                    self.assertEqual(args.soak_seconds, seconds or 0)
                    self.assertEqual(args.runtime_dir, directory / "bin")
                    self.assertEqual(manifest, {})
            self.assertEqual(checked.call_count, 6)
            checked.reset_mock()
            for extra in (["--soak-seconds", "-1"], ["--soak-seconds", "9"],
                          ["--soak-seconds", "3601"], ["--soak-seconds", "nan"],
                          ["--soak-seconds", "10.5"], ["--runtime-dir", "/replacement"],
                          ["--donor-policy"], ["--placement-comparison"], ["--api"],
                          ["--sessions", "4"], ["--joint", "reject"]):
                with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        parse_args(argv + extra)
                    self.assertEqual(error.exception.code, 2)
            checked.assert_not_called()
            with patch.dict("os.environ", {"LUMABRI_RESIDENCY_POLICY": "observe"}), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    parse_args(argv + ["--soak-seconds", "1800"])
            checked.assert_not_called()

    def test_direct_invalid_duration_fails_before_service_access(self):
        for seconds in (None, True, 9, 3601, 10.0, "1800"):
            with self.subTest(seconds=seconds), self.assertRaisesRegex(ValueError, "duration"):
                verify_installed_managed(None, None, None, None, None, None, None, soak_seconds=seconds)

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
