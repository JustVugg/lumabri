"""Prepare a bounded build-source copy, never a copy of local model data.

Works with source archives too: Git metadata is not required. The destination
is replaced only after all source inputs have been copied successfully.
"""
import argparse
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile

SOURCE_SUFFIXES = {".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx",
                   ".inc", ".cu", ".cuh", ".s", ".S", ".m", ".mm", ".metal",
                   ".mk", ".cmake", ".py", ".sh", ".pl", ".awk", ".ps1", ".bat"}
SKIP_DIRS = {".git", "build", ".cache", "__pycache__", ".venv", "venv",
             "node_modules", "models", "checkpoints"}
DATA_SUFFIXES = {".safetensors", ".gguf", ".ggml", ".bin", ".pt", ".pth", ".ckpt",
                 ".onnx", ".npy", ".npz", ".json", ".o", ".a", ".so", ".dylib",
                 ".dll", ".exe", ".pdb", ".pyc"}
MARKER = ".lumabri-source-copy"
MAGIC = "lumabri-source-copy-v1\n"
MAX_BYTES = 256 * 1024 * 1024
MAX_ENTRIES = 100000
LEGACY_OUTPUT = Path(__file__).resolve().parents[1] / "build/segment-hybrid-colibri"


def is_source(name):
    if Path(name).suffix.lower() in DATA_SUFFIXES:
        return False
    return (Path(name).suffix in SOURCE_SUFFIXES or name == "CMakeLists.txt" or
            name == "Makefile" or name.startswith("Makefile.") or
            name in {"LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "NOTICE", "NOTICE.txt"})


def regular(path):
    try:
        return stat.S_ISREG(path.lstat().st_mode)
    except FileNotFoundError:
        return False


def owned_output(path):
    marker = path / MARKER
    if regular(marker) and marker.stat().st_size == len(MAGIC):
        return marker.read_text() == MAGIC
    # Migrate only the exact historical generated directory, never an arbitrary
    # folder containing a file coincidentally named .prepared.
    return (path == LEGACY_OUTPUT.resolve() and regular(path / ".prepared") and
            regular(path / "segment_runtime.h") and regular(path / "edge_runtime.h"))


def copy_source(source, target, remaining):
    before = source.lstat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError(f"source input is not a regular file: {source}")
    if before.st_size > remaining:
        raise ValueError("source copy exceeds the 256 MiB build-input limit")
    fd = os.open(source, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(fd, "rb") as inp:
        opened = os.fstat(inp.fileno())
        if ((opened.st_dev, opened.st_ino, opened.st_size) !=
                (before.st_dev, before.st_ino, before.st_size) or not stat.S_ISREG(opened.st_mode)):
            raise ValueError(f"source changed before copying: {source}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as out:
            left = opened.st_size
            while left:
                block = inp.read(min(left, 1024 * 1024))
                if not block:
                    raise ValueError(f"source shrank during copying: {source}")
                out.write(block)
                left -= len(block)
            after = os.fstat(inp.fileno())
            if (inp.read(1) or after.st_size != opened.st_size or
                    after.st_mtime_ns != opened.st_mtime_ns or after.st_ctime_ns != opened.st_ctime_ns):
                raise ValueError(f"source changed during copying: {source}")
    target.chmod(stat.S_IMODE(opened.st_mode) & 0o777)
    os.utime(target, ns=(opened.st_atime_ns, opened.st_mtime_ns))
    return opened.st_size


def prepare(source, output):
    source = Path(source).resolve(strict=True)
    raw_output = Path(output).absolute()
    if raw_output.is_symlink():
        raise ValueError("refusing a symlink destination")
    output = raw_output.resolve()
    if source == output or source in output.parents or output in source.parents:
        raise ValueError("source and destination must not overlap")
    for name in ("Makefile", "segment_runtime.h", "edge_runtime.h"):
        if not regular(source / name):
            raise ValueError(f"missing regular Colibri build input: {name}")
    if output.exists() and (not output.is_dir() or not owned_output(output)):
        raise ValueError("refusing to replace a directory not owned by this build")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".lumabri-source-", dir=output.parent))
    backup = None
    count = total = visited = 0
    try:
        def walk_error(error):
            raise error
        for parent, dirs, files in os.walk(source, followlinks=False, onerror=walk_error):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            visited += len(dirs) + len(files)
            if visited > MAX_ENTRIES:
                raise ValueError("too many entries in source tree")
            for name in dirs:
                if (Path(parent) / name).is_symlink():
                    raise ValueError(f"source directory symlink is not allowed: {name}")
            for name in sorted(files):
                if not is_source(name):
                    continue
                path = Path(parent) / name
                total += copy_source(path, stage / path.relative_to(source), MAX_BYTES - total)
                count += 1
        (stage / MARKER).write_text(MAGIC)
        if output.exists():
            if output.is_symlink() or not owned_output(output):
                raise ValueError("destination ownership changed during preparation")
            backup = Path(tempfile.mkdtemp(prefix=".lumabri-old-source-", dir=output.parent))
            backup.rmdir()  # Only the empty temporary directory just created above.
            output.rename(backup)
        try:
            stage.rename(output)
        except OSError:
            if backup is not None:
                backup.rename(output)
                backup = None
            raise
        if backup is not None:
            shutil.rmtree(backup)  # Validated generated copy, never the input tree.
        return count, total
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        count, size = prepare(args.source, args.output)
    except (OSError, ValueError) as error:
        print(f"Cannot prepare engine source: {error}", file=sys.stderr)
        return 1
    print(f"Engine source: {count} files, {size} bytes; checkpoints and build outputs excluded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
