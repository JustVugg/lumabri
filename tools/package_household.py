#!/usr/bin/env python3
"""Assemble a native test candidate, never a cross-platform support claim.

Python is build tooling only. The delivered UI/runtime remain C; the launcher
uses the system shell. No checkpoint, key, cache or developer binary is swept
into the package. Build each platform natively, then test this exact directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BINARIES = ("lumabri", "tracker", "maintainer", "segment_node", "segment_chat", "swarm_probe")


def regular(path, executable=False):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or (executable and not os.access(path, os.X_OK)):
        raise ValueError(f"not a regular {'executable' if executable else 'file'}: {path}")
    return path


def revision(root):
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    dirty = bool(subprocess.check_output(
        ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"], text=True).strip())
    return {"commit": commit, "tracked_changes": dirty}


def checksum(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def dependencies(binary, system):
    # Inspection happens only on our just-built native binaries. Do not invoke
    # ldd on downloads supplied by third parties.
    command = ["otool", "-L", str(binary)] if system == "Darwin" else ["ldd", str(binary)]
    report = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    if "not found" in report:
        raise ValueError(f"unresolved runtime dependency: {binary}\n{report}")
    if system == "Darwin":
        for line in report.splitlines()[1:]:
            name = line.strip().split(" (", 1)[0]
            # A dylib's first entry is its own install name, not a dependency.
            if binary.suffix == ".dylib" and name in (binary.name, str(binary)):
                continue
            if not name.startswith(("/usr/lib/", "/System/Library/")):
                raise ValueError(f"non-system macOS dependency {name}; build the candidate with OMP_FLAGS= OMP_LIBS=")
    return report


def version_tuple(text):
    value = tuple(int(part) for part in text.split("."))
    if not 1 <= len(value) <= 3 or any(part < 0 for part in value):
        raise ValueError(f"invalid OS version: {text}")
    return value + (0,) * (3 - len(value))


def macos_minimum(binary):
    report = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    command, versions = "", []
    for line in report.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == "cmd":
            command = fields[1]
        if len(fields) == 2 and ((command == "LC_BUILD_VERSION" and fields[0] == "minos") or
                                 (command == "LC_VERSION_MIN_MACOSX" and fields[0] == "version")):
            versions.append(version_tuple(fields[1]))
    if not versions:
        raise ValueError(f"no macOS deployment version in {binary}")
    return max(versions)


def package(runtime, colibri, output, repository=ROOT):
    system = platform.system()
    if system not in ("Linux", "Darwin"):
        raise ValueError("native Windows household packaging is not implemented; WSL is a Linux build")
    runtime, colibri, repository = runtime.resolve(strict=True), colibri.resolve(strict=True), repository.resolve(strict=True)
    # Reject existing paths (including dangling symlinks); never overwrite an
    # installation or reuse a directory containing another build's binaries.
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    shim = "liblumabri.dylib" if system == "Darwin" else "liblumabri.so"
    files = [(regular(runtime / name, True), f"bin/{name}", 0o755) for name in BINARIES]
    files += [(regular(runtime / shim), f"lib/lumabri/{shim}", 0o644),
              (regular(repository / "tools/setup-household-firewall.ps1"), "lib/lumabri/setup-household-firewall.ps1", 0o644),
              (regular(repository / "LICENSE"), "licenses/Lumabri-LICENSE", 0o644),
              (regular(colibri / "LICENSE"), "licenses/Colibri-LICENSE", 0o644),
              (regular(repository / "docs/NATIVE_CANDIDATES.md"), "START_HERE.md", 0o644),
              (regular(repository / "deploy/household-launcher.sh"),
               "Lumabri.command" if system == "Darwin" else "start-lumabri", 0o755)]
    # Keep the upstream notices with the exact checkout used by this build.
    for name in ("NOTICE", "THIRD_PARTY_NOTICES.md"):
        if (colibri / name).exists():
            files.append((regular(colibri / name), f"licenses/Colibri-{name}", 0o644))
    report = {name: dependencies(runtime / name, system) for name in (*BINARIES, shim)}
    minimum_macos = None
    if system == "Darwin":
        required = max(macos_minimum(runtime / name) for name in (*BINARIES, shim))
        minimum_macos = ".".join(str(part) for part in required)
        target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
        if target and required > version_tuple(target):
            raise ValueError(f"a binary requires macOS {minimum_macos}, above the requested target {target}")
    manifest = {"schema": "LMB-NATIVE-CANDIDATE-1", "status": "requires-native-and-physical-acceptance",
                "system": system, "architecture": platform.machine(), "build_os": platform.platform(),
                "minimum_macos": minimum_macos,
                "lumabri": revision(repository), "colibri_checkout": revision(colibri),
                "dependencies": report, "files": {}}
    # No model-runtime provenance is inferred from a directory name: retain
    # hashes of the actual shipped bytes as well as source checkout metadata.
    output.mkdir(mode=0o755)
    for source, relative, mode in files:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(mode)
        manifest["files"][relative] = {"sha256": checksum(target),
                                      "bytes": target.stat().st_size, "mode": oct(mode)}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def verify(directory):
    """Consistency check, not a signature or independent trust attestation."""
    directory = directory.resolve(strict=True)
    manifest_path = regular(directory / "manifest.json")
    if manifest_path.stat().st_size > 128 * 1024:
        raise ValueError("oversized candidate manifest")
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict) or manifest.get("schema") != "LMB-NATIVE-CANDIDATE-1":
        raise ValueError("unknown candidate manifest")
    files = manifest.get("files")
    if not isinstance(files, dict) or not 1 <= len(files) <= 32:
        raise ValueError("invalid manifest file list")
    actual = set()
    # Inspect entries before resolving declared paths; a symlinked directory
    # must not make the verifier read outside the candidate.
    for index, path in enumerate(directory.rglob("*")):
        if index >= 64 or path.is_symlink():
            raise ValueError("unexpected entry or symlink in candidate")
        if path.is_file():
            actual.add(str(path.relative_to(directory)))
        elif not path.is_dir():
            raise ValueError("non-regular entry in candidate")
    if actual != set(files) | {"manifest.json"}:
        raise ValueError("candidate has missing or extra files")
    for name, expected in files.items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or str(relative) != name:
            raise ValueError("unsafe manifest path")
        path = regular(directory / name)
        if (not isinstance(expected, dict) or expected.get("sha256") != checksum(path) or
                expected.get("bytes") != path.stat().st_size or
                expected.get("mode") != oct(path.stat().st_mode & 0o777)):
            raise ValueError(f"candidate file changed: {name}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", type=Path, default=ROOT)
    parser.add_argument("--colibri-root", type=Path,
                        help="read-only source checkout used for the native build, including LICENSE")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--output", type=Path, help="new, empty destination (must not exist)")
    action.add_argument("--verify", type=Path, help="check exact file hashes and contents after native tests")
    args = parser.parse_args()
    if args.output and not args.colibri_root:
        parser.error("--output requires --colibri-root")
    try:
        if args.verify:
            verify(args.verify)
            print("Native candidate contents and hashes: PASS")
            return 0
        manifest = package(args.runtime_dir, args.colibri_root, args.output)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Household candidate was not completed: {error}", file=sys.stderr)
        return 1
    print(f"Native {manifest['system']} {manifest['architecture']} candidate: {args.output}")
    print("Run the household acceptance flow from its bin directory before distributing it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
