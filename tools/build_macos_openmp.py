#!/usr/bin/env python3
"""Build the pinned LLVM OpenMP runtime natively, including macOS 12 support.

Build tooling only; users of the candidate do not need Python, CMake or brew.
Source is an explicit read-only checkout; build/install paths must be new.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

REVISION = "87f0227cb60147a26a1eeb4fb06e3b505e9c7261"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--prefix", type=Path, required=True)
    args = p.parse_args()
    if platform.system() != "Darwin":
        p.error("this is a native macOS build")
    source = args.source.resolve(strict=True)
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
    if revision != REVISION or dirty:
        p.error("expected a clean pinned LLVM 20.1.8 checkout")
    if any(path.exists() or path.is_symlink() for path in (args.build, args.prefix)):
        p.error("build and prefix must be new paths")
    target = os.environ.get("MACOSX_DEPLOYMENT_TARGET", "12.0")
    subprocess.check_call(["cmake", "-S", str(source / "openmp"), "-B", str(args.build),
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={args.prefix.resolve()}",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={target}", f"-DCMAKE_OSX_ARCHITECTURES={platform.machine()}",
        "-DCMAKE_INSTALL_NAME_DIR=@rpath", "-DOPENMP_ENABLE_LIBOMPTARGET=OFF",
        "-DOPENMP_ENABLE_OMPT_TOOLS=OFF", "-DLIBOMP_OMPT_SUPPORT=OFF", "-DLIBOMP_INSTALL_ALIASES=OFF"])
    subprocess.check_call(["cmake", "--build", str(args.build), "--parallel", "3"])
    subprocess.check_call(["cmake", "--install", str(args.build)])
    licence_dir = args.prefix / "share/licenses/libomp"
    licence_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / "openmp/LICENSE.TXT", licence_dir / "LICENSE.TXT")
    (licence_dir / "SOURCE.json").write_text(json.dumps({
        "repository": "https://github.com/llvm/llvm-project", "commit": REVISION, "version": "20.1.8"}, indent=2) + "\n")


if __name__ == "__main__":
    main()
