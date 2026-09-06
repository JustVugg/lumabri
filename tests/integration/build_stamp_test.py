"""Changing effective engine options invalidates the build; unchanged options do not."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lumabri-build-stamp-") as directory:
    stamp = Path(directory) / "options"
    command = [sys.executable, str(ROOT / "tools/update_build_stamp.py"), str(stamp)]
    subprocess.run(command + ["clang", "", ""], check=True)
    before = stamp.stat().st_mtime_ns
    subprocess.run(command + ["clang", "", ""], check=True)
    assert stamp.stat().st_mtime_ns == before
    subprocess.run(command + ["clang", "-Xclang -fopenmp", "-lomp"], check=True)
    assert "fopenmp" in stamp.read_text()
    subprocess.run(command + ["clang", "", ""], check=True)
    assert "fopenmp" not in stamp.read_text()
print("BUILD STAMP: PASS (stable, OpenMP enabled, OpenMP disabled)")
