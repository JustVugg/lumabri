"""Source-only build preparation must not copy or delete original checkpoints."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("engine_source", ROOT / "tools/prepare_engine_source.py")
engine_source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(engine_source)


class Preparation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="lumabri-source-test-")
        self.root = Path(self.tmp.name)
        self.source = self.root / "source with spaces"
        self.output = self.root / "generated build"
        self.source.mkdir()
        for name in ("Makefile", "segment_runtime.h", "edge_runtime.h", "model.c"):
            (self.source / name).write_text("/* build input */\n")

    def tearDown(self):
        self.tmp.cleanup()

    def prepare(self):
        return engine_source.prepare(self.source, self.output)

    def assert_no_temporary_copy(self):
        self.assertFalse(list(self.root.glob(".lumabri-source-*")))
        self.assertFalse(list(self.root.glob(".lumabri-old-source-*")))

    def test_archive_sources_only(self):
        (self.source / "include").mkdir()
        (self.source / "include/kernel.inc").write_text("kernel source\n")
        (self.source / "tools").mkdir()
        tool = self.source / "tools/generate.py"
        tool.write_text("pass\n")
        tool.chmod(0o755)
        (self.source / "Makefile.units").write_text("# required build rules\n")
        (self.source / "LICENSE").write_text("source notice\n")
        for name in ("checkpoint.safetensors", "model.gguf", "model.bin", "model.pt",
                     "tokenizer.json", "config.json", "engine.o", "engine.a", "engine.so",
                     "Makefile.safetensors", "Makefile.GGUF", "Makefile.json", "Makefile.o"):
            (self.source / name).write_bytes(b"not a build input")
        for directory in ("build", ".git", ".venv", "models", "checkpoints"):
            (self.source / directory).mkdir()
            (self.source / directory / "should_not_copy.c").write_text("excluded\n")
        count, size = self.prepare()
        self.assertEqual(count, 8)
        self.assertGreater(size, 0)
        self.assertEqual((self.output / "include/kernel.inc").read_text(), "kernel source\n")
        self.assertEqual((self.output / "tools/generate.py").stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.output / "Makefile.units").read_text(), "# required build rules\n")
        self.assertFalse(list(self.output.rglob("*.safetensors")))
        self.assertFalse(list(self.output.rglob("*.json")))
        self.assertFalse((self.output / "Makefile.GGUF").exists())
        self.assertFalse((self.output / "Makefile.o").exists())
        for directory in ("build", ".git", ".venv", "models", "checkpoints"):
            self.assertFalse((self.output / directory).exists())
        self.assertEqual((self.source / "checkpoint.safetensors").read_bytes(), b"not a build input")
        self.assert_no_temporary_copy()

    def test_reprepare_only_generated_copy(self):
        self.prepare()
        (self.output / "obsolete.c").write_text("old generated copy")
        (self.output / "old-model.safetensors").write_bytes(b"duplicated old cache")
        (self.source / "model.safetensors").write_bytes(b"original weights")
        (self.source / "model.c").write_text("updated source\n")
        self.prepare()
        self.assertEqual((self.output / "model.c").read_text(), "updated source\n")
        self.assertFalse((self.output / "obsolete.c").exists())
        self.assertFalse((self.output / "old-model.safetensors").exists())
        self.assertEqual((self.source / "model.safetensors").read_bytes(), b"original weights")
        self.assert_no_temporary_copy()

    def test_refuse_unowned_directory(self):
        self.output.mkdir()
        (self.output / "keep.txt").write_text("user file")
        with self.assertRaisesRegex(ValueError, "not owned"):
            self.prepare()
        self.assertEqual((self.output / "keep.txt").read_text(), "user file")
        self.assert_no_temporary_copy()

    def test_overlap_is_never_allowed(self):
        for output in (self.source, self.source / "nested", self.root):
            with self.subTest(output=output), self.assertRaisesRegex(ValueError, "overlap"):
                engine_source.prepare(self.source, output)
        self.assertTrue((self.source / "model.c").exists())

    def test_symlink_destination(self):
        real = self.root / "keep"
        real.mkdir()
        self.output.symlink_to(real, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink destination"):
            self.prepare()
        self.assertEqual(list(real.iterdir()), [])

    def test_source_symlink_preserves_previous_build(self):
        self.prepare()
        external = self.root / "private"
        external.write_text("not a source input")
        (self.source / "escape.h").symlink_to(external)
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            self.prepare()
        self.assertFalse((self.output / "escape.h").exists())
        self.assertTrue((self.output / "model.c").exists())
        self.assertEqual(external.read_text(), "not a source input")
        self.assert_no_temporary_copy()

    def test_directory_symlink_is_not_followed(self):
        external = self.root / "outside"
        external.mkdir()
        (self.source / "include").symlink_to(external, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "directory symlink"):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.assert_no_temporary_copy()

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX special file test")
    def test_fifo_source_never_blocks(self):
        os.mkfifo(self.source / "blocked.c")
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            self.prepare()
        self.assert_no_temporary_copy()

    def test_limit_and_copy_error_preserve_previous_build(self):
        self.prepare()
        for fault in (mock.patch.object(engine_source, "MAX_BYTES", 1),
                      mock.patch.object(engine_source, "MAX_ENTRIES", 1),
                      mock.patch.object(engine_source, "copy_source", side_effect=OSError("disk full"))):
            with fault, self.assertRaises((ValueError, OSError)):
                self.prepare()
            self.assertTrue((self.output / "model.c").exists())
            self.assert_no_temporary_copy()

    def test_failed_publish_restores_previous_copy(self):
        self.prepare()
        original = Path.rename
        def rename(path, target):
            if path.name.startswith(".lumabri-source-"):
                raise OSError("simulated publish failure")
            return original(path, target)
        with mock.patch.object(Path, "rename", rename), self.assertRaisesRegex(OSError, "publish"):
            self.prepare()
        self.assertTrue((self.output / "model.c").exists())
        self.assert_no_temporary_copy()

    def test_legacy_migration_only_at_known_generated_path(self):
        self.output.mkdir()
        for name in (".prepared", "segment_runtime.h", "edge_runtime.h"):
            (self.output / name).write_text("old generated input")
        with self.assertRaisesRegex(ValueError, "not owned"):
            self.prepare()
        with mock.patch.object(engine_source, "LEGACY_OUTPUT", self.output):
            self.prepare()
        self.assertEqual((self.output / engine_source.MARKER).read_text(), engine_source.MAGIC)
        self.assert_no_temporary_copy()


if __name__ == "__main__":
    unittest.main()
