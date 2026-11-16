"""Release archive contracts on a stand-in candidate; the real archive is run by release_archive_smoke_test.py."""
import hashlib
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"tools/{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release, packager = load("release_archive"), load("package_household")


class ReleaseArchiveTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lumabri-release-test-")
        self.root = Path(self.temporary.name)
        self.runtime, self.colibri, self.out = self.root / "runtime", self.root / "colibri", self.root / "release"
        self.runtime.mkdir()
        self.colibri.mkdir()
        for name in (*packager.BINARIES, *packager.WINDOWS_BINARIES, "liblumabri.so", "liblumabri.dylib"):
            (self.runtime / name).write_bytes(b"stand-in bytes, not a runtime\n")
            (self.runtime / name).chmod(0o755)
        (self.runtime / "WINDOWS-RUNTIME-NOTICES.txt").write_text("stand-in notices")
        (self.colibri / "LICENSE").write_text("stand-in licence")

    def tearDown(self):
        self.temporary.cleanup()

    def candidate(self, system, machine, **options):
        output = self.root / f"candidate {system} {machine}"
        with patch.object(packager.platform, "system", return_value=system), \
                patch.object(packager.platform, "machine", return_value=machine), \
                patch.object(packager, "dependencies", return_value="libc.so.6"), \
                patch.object(packager, "macos_minimum", return_value=(12, 0, 0)), \
                patch.object(packager, "windows_dependencies", return_value=["KERNEL32.dll"]), \
                patch.object(packager, "revision", return_value={"commit": "a" * 40, "tracked_changes": False}):
            packager.package(self.runtime, self.colibri, output, **options)
        return output

    def test_every_platform_row_keeps_the_verified_candidate_under_one_folder(self):
        rows = {"linux-x86_64": ("Linux", "x86_64"), "linux-arm64": ("Linux", "aarch64"),
                "macos-arm64": ("Darwin", "arm64"), "macos-x86_64": ("Darwin", "x86_64"),
                "windows-x86_64": ("Windows", "AMD64")}
        self.assertEqual(set(rows), set(release.PLATFORMS))
        for row, (system, machine) in rows.items():
            candidate = self.candidate(system, machine)
            archive = release.build(candidate, row, "1.0.0", self.out, require_clean=True)
            self.assertTrue(release.ARCHIVE_NAME.fullmatch(archive.name), archive.name)
            unpacked = self.root / f"unpacked {row}"
            if archive.suffix == ".zip":
                with zipfile.ZipFile(archive) as packed:
                    packed.extractall(unpacked)
            else:
                with tarfile.open(archive) as packed:
                    self.assertTrue(all(member.uid == 0 and member.isfile() for member in packed.getmembers()))
                    packed.extractall(unpacked, filter="data")
            folder, = unpacked.iterdir()
            self.assertEqual(folder.name, f"lumabri-1.0.0-{row}")
            manifest = packager.verify(folder)
            start = release.PLATFORMS[row][3]
            self.assertIn(start, manifest["files"])
            if system != "Windows":
                self.assertIn("SHA256SUMS", manifest["files"])
                self.assertEqual(folder.joinpath(start).stat().st_mode & 0o777, 0o755)
                self.assertEqual(folder.joinpath("bin/lumabri-fetch").stat().st_mode & 0o777, 0o755)
            line = (self.out / (archive.name + ".sha256")).read_text()
            self.assertEqual(line, f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")
        self.assertEqual(len(release.checksums(self.out)), 5)
        self.assertEqual(len((self.out / "SHA256SUMS").read_text().splitlines()), 5)
        victim = next(self.out.glob("*.zip"))
        victim.write_bytes(victim.read_bytes() + b"x")
        with self.assertRaisesRegex(ValueError, "does not match"):
            release.checksums(self.out)

    def test_wrong_row_changed_candidate_and_existing_archive_are_refused(self):
        candidate = self.candidate("Linux", "x86_64")
        with self.assertRaisesRegex(ValueError, "not linux-arm64"):
            release.build(candidate, "linux-arm64", "1.0.0", self.out)
        with self.assertRaisesRegex(ValueError, "invalid version"):
            release.build(candidate, "linux-x86_64", "../1", self.out)
        release.build(candidate, "linux-x86_64", "1.0.0", self.out)
        with self.assertRaisesRegex(ValueError, "already exists"):
            release.build(candidate, "linux-x86_64", "1.0.0", self.out)
        (candidate / "bin/lumabri").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "file changed"):
            release.build(candidate, "linux-x86_64", "1.0.1", self.out)

    def test_linux_release_requires_openmp_inside_the_binaries(self):
        manifest_path = self.candidate("Linux", "x86_64", static_openmp=True) / "manifest.json"
        self.assertIn("licenses/Linux-runtime-NOTICES.txt", manifest_path.read_text())
        self.assertIn('"linkage": "static"', manifest_path.read_text())
        with patch.object(packager.platform, "system", return_value="Linux"), \
                patch.object(packager, "dependencies", return_value="libgomp.so.1 => /usr/lib/libgomp.so.1"), \
                patch.object(packager, "revision", return_value={"commit": "a" * 40, "tracked_changes": False}):
            with self.assertRaisesRegex(ValueError, "not statically linked"):
                packager.package(self.runtime, self.colibri, self.root / "dynamic", static_openmp=True)
        self.assertFalse((self.root / "dynamic").exists())
        with patch.object(packager.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(ValueError, "Linux packaging option"):
                packager.package(self.runtime, self.colibri, self.root / "mac", static_openmp=True)


if __name__ == "__main__":
    unittest.main()
