#!/usr/bin/env python3
"""Turn a verified native candidate into the archive a person downloads.

Build tooling only. The archive holds exactly the candidate's files under one
top-level folder, so the unpacked folder still passes
`package_household.py --verify`. Archives are unsigned: the `.sha256` file and
the release `SHA256SUMS` detect a damaged or mixed download, not a publisher.
"""
import argparse
import gzip
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import sys
import tarfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# platform row -> (manifest system, manifest architectures, archive suffix,
# the file to double-click, the launcher that forwards other commands).
# CPU only. A GPU backend, when one exists, is a further row whose name ends
# in the backend ("linux-x86_64-<backend>"); nothing here builds one.
PLATFORMS = {
    "linux-x86_64": ("Linux", ("x86_64",), ".tar.gz", "start-here.sh", "start-lumabri"),
    "linux-arm64": ("Linux", ("aarch64", "arm64"), ".tar.gz", "start-here.sh", "start-lumabri"),
    "macos-arm64": ("Darwin", ("arm64",), ".tar.gz", "START-HERE.command", "Lumabri.command"),
    "macos-x86_64": ("Darwin", ("x86_64",), ".tar.gz", "START-HERE.command", "Lumabri.command"),
    "windows-x86_64": ("Windows", ("x86_64",), ".zip", "START-HERE.bat", "Lumabri.cmd"),
}
ARCHIVE_NAME = re.compile(r"lumabri-[0-9A-Za-z][0-9A-Za-z.+_-]{0,47}-(%s)(\.tar\.gz|\.zip)" % "|".join(PLATFORMS))


def packager():
    spec = importlib.util.spec_from_file_location("package_household", ROOT / "tools/package_household.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def checksum(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build(candidate, row, version, output_dir, require_clean=False):
    if row not in PLATFORMS:
        raise ValueError(f"unknown platform row: {row}")
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,47}", version):
        raise ValueError(f"invalid version: {version}")
    system, architectures, suffix, start, launcher = PLATFORMS[row]
    manifest = packager().verify(candidate)
    if manifest.get("system") != system or manifest.get("architecture") not in architectures:
        raise ValueError(f"candidate is {manifest.get('system')} {manifest.get('architecture')}, not {row}")
    files = manifest["files"]
    for name in (start, launcher, "licenses/Lumabri-LICENSE", "licenses/Colibri-LICENSE"):
        if name not in files:
            raise ValueError(f"candidate lacks {name}")
    if require_clean and (manifest["lumabri"].get("tracked_changes") or
                          manifest["colibri_checkout"].get("tracked_changes")):
        raise ValueError("a release archive is built from clean checkouts only")
    folder = f"lumabri-{version}-{row}"
    output_dir.mkdir(parents=True, exist_ok=True)
    archive = output_dir / (folder + suffix)
    if archive.exists() or archive.is_symlink():
        raise ValueError(f"output already exists: {archive}")
    names = sorted([*files, "manifest.json"])
    stamp = int(os.environ.get("SOURCE_DATE_EPOCH") or time.time())
    if suffix == ".zip":
        date = time.gmtime(max(stamp, 315532800))[:6]
        with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as out:
            for name in names:
                info = zipfile.ZipInfo(f"{folder}/{name}", date_time=date)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                out.writestr(info, (candidate / name).read_bytes())
    else:
        # Owner, order and modes come from the manifest, not from the builder.
        with archive.open("xb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as packed, \
                tarfile.open(fileobj=packed, mode="w", format=tarfile.PAX_FORMAT) as out:
            for name in names:
                info = tarfile.TarInfo(f"{folder}/{name}")
                info.size = (candidate / name).stat().st_size
                info.mode = int(files[name]["mode"], 8) if name in files else 0o644
                info.mtime, info.uid, info.gid, info.uname, info.gname = stamp, 0, 0, "", ""
                with (candidate / name).open("rb") as stream:
                    out.addfile(info, stream)
    # Same line format as sha256sum and shasum -a 256, so either checks it.
    (output_dir / (archive.name + ".sha256")).write_text(f"{checksum(archive)}  {archive.name}\n", encoding="utf-8")
    return archive


def checksums(directory):
    """One SHA256SUMS for every archive of a release, checked against each .sha256."""
    archives = sorted(path for path in directory.iterdir() if ARCHIVE_NAME.fullmatch(path.name))
    if not archives:
        raise ValueError(f"no release archives in {directory}")
    lines = []
    for archive in archives:
        if archive.is_symlink() or not archive.is_file():
            raise ValueError(f"not a regular file: {archive}")
        line = f"{checksum(archive)}  {archive.name}\n"
        recorded = directory / (archive.name + ".sha256")
        if not recorded.is_file() or recorded.read_text(encoding="utf-8") != line:
            raise ValueError(f"{archive.name} does not match the checksum written when it was built")
        lines.append(line)
    (directory / "SHA256SUMS").write_text("".join(lines), encoding="utf-8")
    return archives


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, help="directory written by package_household.py --output")
    parser.add_argument("--platform", choices=sorted(PLATFORMS))
    parser.add_argument("--version", help="release version without the leading v")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--require-clean", action="store_true", help="refuse candidates built from changed checkouts")
    parser.add_argument("--checksums", type=Path, help="write SHA256SUMS for the archives in this directory")
    args = parser.parse_args()
    try:
        if args.checksums:
            for archive in checksums(args.checksums):
                print(archive.name)
            return 0
        if not (args.candidate and args.platform and args.version and args.output_dir):
            parser.error("--candidate, --platform, --version and --output-dir are required")
        archive = build(args.candidate, args.platform, args.version, args.output_dir, args.require_clean)
    except (OSError, ValueError) as error:
        print(f"Release archive was not completed: {error}", file=sys.stderr)
        return 1
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
