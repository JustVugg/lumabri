"""Packaging contracts; real installed inference is a separate native CI gate."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("package_household", ROOT / "tools/package_household.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lumabri-package-test-")
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        for name in (*module.BINARIES, "liblumabri.so", "liblumabri.dylib"):
            path = self.runtime / name
            path.write_bytes(b"test packaging bytes, not a model runtime\n")
            path.chmod(0o755)
        (self.runtime / "private.key").write_text("must never ship")
        (self.runtime / "weights.safetensors").write_text("must never ship")
        self.colibri = self.root / "colibri"
        self.colibri.mkdir()
        (self.colibri / "LICENSE").write_text("test upstream licence")
        (self.colibri / "THIRD_PARTY_NOTICES.md").write_text("test upstream notices")
        self.output = self.root / "candidate with spaces"

    def tearDown(self):
        self.temporary.cleanup()

    def build(self, system="Linux"):
        with patch.object(module.platform, "system", return_value=system), \
                patch.object(module, "dependencies", return_value="test dependencies"), \
                patch.object(module, "macos_minimum", return_value=(12, 0, 0)), \
                patch.object(module, "revision", return_value={"commit": "a" * 40, "tracked_changes": False}):
            return module.package(self.runtime, self.colibri, self.output)

    def test_exact_allowlist_manifest_and_modes(self):
        result = self.build()
        self.assertEqual(json.loads((self.output / "manifest.json").read_text()), result)
        self.assertEqual(module.verify(self.output), result)
        names = {str(p.relative_to(self.output)) for p in self.output.rglob("*") if p.is_file()}
        self.assertEqual(names, set(result["files"]) | {"manifest.json"})
        self.assertEqual(set(p.name for p in (self.output / "bin").iterdir()), set(module.BINARIES))
        self.assertNotIn("private.key", " ".join(names))
        self.assertNotIn("weights.safetensors", " ".join(names))
        for name, info in result["files"].items():
            path = self.output / name
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), info["sha256"])
            self.assertEqual(path.stat().st_size, info["bytes"])
            self.assertEqual(path.stat().st_mode & 0o777, int(info["mode"], 8))

    def test_existing_output_is_never_overwritten(self):
        self.build()
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.build()

    def test_verify_rejects_mutation_extra_file_and_symlink(self):
        self.build()
        binary = self.output / "bin/lumabri"
        original = binary.read_bytes()
        binary.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "file changed"):
            module.verify(self.output)
        binary.write_bytes(original)
        extra = self.output / "private.key"
        extra.write_text("must never ship")
        with self.assertRaisesRegex(ValueError, "extra files"):
            module.verify(self.output)
        extra.unlink()
        binary.unlink()
        binary.symlink_to(self.runtime / "lumabri")
        with self.assertRaisesRegex(ValueError, "symlink"):
            module.verify(self.output)

    def test_incomplete_runtime_does_not_create_output(self):
        (self.runtime / "segment_chat").unlink()
        with self.assertRaises(FileNotFoundError):
            self.build()
        self.assertFalse(self.output.exists())

    def test_symlink_runtime_or_destination_is_rejected(self):
        target = self.runtime / "segment_chat"
        target.unlink()
        target.symlink_to(self.runtime / "segment_node")
        with self.assertRaisesRegex(ValueError, "not a regular"):
            self.build()
        self.assertFalse(self.output.exists())
        self.output.symlink_to(self.root / "absent")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.build()

    def test_macos_launcher_and_upstream_notices(self):
        result = self.build("Darwin")
        self.assertIn("Lumabri.command", result["files"])
        self.assertIn("lib/lumabri/liblumabri.dylib", result["files"])
        self.assertEqual((self.output / "licenses/Colibri-THIRD_PARTY_NOTICES.md").read_text(), "test upstream notices")

    def test_windows_is_not_silently_linux(self):
        with self.assertRaisesRegex(ValueError, "Windows household packaging is not implemented"):
            self.build("Windows")

    def test_non_system_macos_dependency_refused(self):
        bad = "segment_node:\n\t/opt/homebrew/opt/libomp/lib/libomp.dylib (compatibility version 5.0.0)\n"
        with patch.object(module.subprocess, "check_output", return_value=bad):
            with self.assertRaisesRegex(ValueError, "non-system macOS dependency"):
                module.dependencies(self.runtime / "segment_node", "Darwin")
        good = "liblumabri.dylib:\n\tliblumabri.dylib (compatibility version 0.0.0)\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
        with patch.object(module.subprocess, "check_output", return_value=good):
            self.assertEqual(module.dependencies(self.runtime / "liblumabri.dylib", "Darwin"), good)

    def test_missing_linux_library_refused(self):
        with patch.object(module.subprocess, "check_output", return_value="libgomp.so.1 => not found\n"):
            with self.assertRaisesRegex(ValueError, "unresolved runtime dependency"):
                module.dependencies(self.runtime / "segment_node", "Linux")

    def test_macos_minimum_comes_from_load_commands(self):
        report = "cmd LC_BUILD_VERSION\nplatform 1\nminos 12.0\nsdk 15.0\ncmd LC_SOURCE_VERSION\nversion 99.0\n"
        with patch.object(module.subprocess, "check_output", return_value=report):
            self.assertEqual(module.macos_minimum(self.runtime / "segment_node"), (12, 0, 0))
        with patch.object(module.subprocess, "check_output", return_value="cmd LC_VERSION_MIN_MACOSX\nversion 10.15\nsdk 15.0\n"):
            self.assertEqual(module.macos_minimum(self.runtime / "segment_node"), (10, 15, 0))
        with patch.object(module.subprocess, "check_output", return_value="cmd LC_SOURCE_VERSION\nversion 99.0\n"):
            with self.assertRaisesRegex(ValueError, "no macOS deployment"):
                module.macos_minimum(self.runtime / "segment_node")

    def test_candidate_cannot_exceed_requested_macos_target(self):
        with patch.dict(os.environ, {"MACOSX_DEPLOYMENT_TARGET": "11.0"}):
            with self.assertRaisesRegex(ValueError, "above the requested target"):
                self.build("Darwin")
        self.assertFalse(self.output.exists())
        with patch.dict(os.environ, {"MACOSX_DEPLOYMENT_TARGET": "12.0"}):
            self.assertEqual(self.build("Darwin")["minimum_macos"], "12.0.0")

    def test_only_explicit_openmp_dependency_is_allowed(self):
        lib = (self.runtime / "libomp.dylib").resolve()
        binary = self.runtime / "segment_node"
        for name in (str(lib), "@rpath/libomp.dylib"):
            report = f"segment_node:\n\t{name} (compatibility version 5.0.0)\n"
            with patch.object(module.subprocess, "check_output", return_value=report):
                self.assertEqual(module.dependencies(binary, "Darwin", lib), report)
        for name in ("/tmp/unapproved/libomp.dylib", "@rpath/libother.dylib", "@rpath/libomp.dylib"):
            report = f"segment_node:\n\t{name} (compatibility version 5.0.0)\n"
            with patch.object(module.subprocess, "check_output", return_value=report):
                with self.assertRaisesRegex(ValueError, "non-system"):
                    module.dependencies(binary, "Darwin", lib, packaged=True)

    def test_packaged_openmp_link_is_loader_relative(self):
        for filename, link in (("segment_node", "@loader_path/../lib/lumabri/libomp.dylib"),
                               ("liblumabri.dylib", "@loader_path/libomp.dylib")):
            report = filename + ":\n"
            if filename.endswith(".dylib"):
                report += f"\t@rpath/{filename} (compatibility version 0.0.0)\n"
            report += f"\t{link} (compatibility version 5.0.0)\n"
            with patch.object(module.subprocess, "check_output", return_value=report):
                self.assertEqual(module.dependencies(self.runtime / filename, "Darwin",
                    self.runtime / "libomp.dylib", packaged=True), report)

    def test_openmp_licence_and_provenance_required(self):
        prefix = self.root / "omp"
        (prefix / "lib").mkdir(parents=True)
        (prefix / "lib/libomp.dylib").write_bytes(b"test runtime")
        with self.assertRaises(FileNotFoundError):
            module.openmp_files(prefix)
        licences = prefix / "share/licenses/libomp"
        licences.mkdir(parents=True)
        (licences / "LICENSE.TXT").write_text("test licence")
        (licences / "SOURCE.json").write_text('{"version":"unverified"}')
        with self.assertRaisesRegex(ValueError, "provenance"):
            module.openmp_files(prefix)
        source = {"repository": "https://github.com/llvm/llvm-project",
                  "commit": "87f0227cb60147a26a1eeb4fb06e3b505e9c7261", "version": "20.1.8"}
        (licences / "SOURCE.json").write_text(json.dumps(source))
        self.assertEqual(module.openmp_files(prefix)[3], source)

    def test_relocation_changes_copies_and_resigns(self):
        report = {"segment_node": "segment_node:\n\t@rpath/libomp.dylib (version 5)\n",
                  "libomp.dylib": "libomp.dylib:\n\t@rpath/libomp.dylib (version 5)\n\t/usr/lib/libSystem.B.dylib (version 1)\n"}
        with patch.object(module, "macos_rpaths", return_value=["/build/private path"]), \
                patch.object(module, "dependencies", return_value="verified"), \
                patch.object(module.subprocess, "check_call") as call:
            module.relocate_openmp(self.output, report)
        commands = [c.args[0] for c in call.call_args_list]
        self.assertIn(["install_name_tool", "-change", "@rpath/libomp.dylib",
            "@loader_path/../lib/lumabri/libomp.dylib", str(self.output / "bin/segment_node")], commands)
        self.assertIn(["codesign", "--verify", "--strict", str(self.output / "lib/lumabri/libomp.dylib")], commands)
        self.assertTrue(all(str(self.output) in c[-1] for c in commands))


if __name__ == "__main__":
    unittest.main()
