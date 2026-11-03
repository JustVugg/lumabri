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
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CORE_BINARIES = ("lumabri", "tracker", "maintainer", "segment_node", "segment_chat", "swarm_probe")
BINARIES = (*CORE_BINARIES, "lumabri-agent")
WINDOWS_BINARIES = ("lumabri.exe", "tracker.exe", "maintainer.exe", "segment_node.exe", "segment_chat.exe")


def regular(path, executable=False):
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400 or
            (executable and not os.access(path, os.X_OK))):
        raise ValueError(f"not a regular {'executable' if executable else 'file'}: {path}")
    return path


def build_tool_output(command):
    try:
        return subprocess.check_output(command, text=True)
    except FileNotFoundError as error:
        raise ValueError(f"required packaging tool not found: {command[0]}; install it in the build environment PATH") from error


def revision(root):
    commit = build_tool_output(["git", "-C", str(root), "rev-parse", "HEAD"]).strip()
    dirty = bool(build_tool_output(
        ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"]).strip())
    return {"commit": commit, "tracked_changes": dirty}


def checksum(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def macos_links(report, binary):
    links = [line.strip().split(" (", 1)[0] for line in report.splitlines()[1:]]
    # LC_ID_DYLIB is the first entry for a library, not a dependency.
    if binary.suffix == ".dylib" and links and Path(links[0]).name == binary.name:
        links = links[1:]
    return links


def dependencies(binary, system, openmp_library=None, packaged=False):
    # Inspection happens only on our just-built native binaries. Do not invoke
    # ldd on downloads supplied by third parties.
    command = ["otool", "-L", str(binary)] if system == "Darwin" else ["ldd", str(binary)]
    report = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    if "not found" in report:
        raise ValueError(f"unresolved runtime dependency: {binary}\n{report}")
    if system == "Darwin":
        for name in macos_links(report, binary):
            if openmp_library is not None and (
                    (not packaged and (name == "@rpath/libomp.dylib" or
                     (name.startswith("/") and Path(name).resolve() == openmp_library))) or
                    (packaged and name == ("@loader_path/libomp.dylib" if binary.suffix == ".dylib"
                                           else "@loader_path/../lib/lumabri/libomp.dylib"))):
                continue
            if not name.startswith(("/usr/lib/", "/System/Library/")):
                raise ValueError(f"non-system macOS dependency {name}; build the candidate with OMP_FLAGS= OMP_LIBS=")
    return report


def macos_rpaths(binary):
    report = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    command, paths = "", []
    for line in report.splitlines():
        fields = line.strip().split()
        if len(fields) == 2 and fields[0] == "cmd":
            command = fields[1]
        if command == "LC_RPATH" and line.strip().startswith("path "):
            paths.append(line.strip()[5:].rsplit(" (offset ", 1)[0])
    return paths


def relocate_openmp(output, report):
    """Rewrite only our copied binaries. Never mutate a build or Homebrew install."""
    for name, original in report.items():
        binary = output / (f"lib/lumabri/{name}" if name.endswith(".dylib") else f"bin/{name}")
        replacement = "@loader_path/libomp.dylib" if name.endswith(".dylib") else "@loader_path/../lib/lumabri/libomp.dylib"
        for link in macos_links(original, binary):
            if Path(link).name == "libomp.dylib":
                subprocess.check_call(["install_name_tool", "-change", link, replacement, str(binary)])
        # No search path into the builder's machine may survive relocation.
        for path in macos_rpaths(binary):
            subprocess.check_call(["install_name_tool", "-delete_rpath", path, str(binary)])
        if name == "libomp.dylib":
            subprocess.check_call(["install_name_tool", "-id", "@rpath/libomp.dylib", str(binary)])
        # Load-command edits invalidate existing signatures, including arm64
        # linker signatures. Ad-hoc signing is NOT Developer ID/notarization.
        subprocess.check_call(["codesign", "--force", "--sign", "-", str(binary)])
        subprocess.check_call(["codesign", "--verify", "--strict", str(binary)])
        dependencies(binary, "Darwin", output / "lib/lumabri/libomp.dylib", packaged=True)


def openmp_files(prefix):
    prefix = prefix.resolve(strict=True)
    library = regular(prefix / "lib/libomp.dylib").resolve()
    licence = regular(prefix / "share/licenses/libomp/LICENSE.TXT")
    provenance = regular(prefix / "share/licenses/libomp/SOURCE.json")
    if provenance.stat().st_size > 4096:
        raise ValueError("oversized OpenMP provenance")
    source = json.loads(provenance.read_text())
    if source != {"repository": "https://github.com/llvm/llvm-project",
                  "commit": "87f0227cb60147a26a1eeb4fb06e3b505e9c7261",
                  "version": "20.1.8"}:
        raise ValueError("unrecognized OpenMP build provenance")
    return library, licence, provenance, source


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


def windows_dependencies(binary, objdump="objdump"):
    """Inspect our built PE, refusing dependencies on the build environment."""
    header = build_tool_output([objdump, "-f", str(binary)])
    if "file format pei-x86-64" not in header:
        raise ValueError(f"Windows candidate requires native x86-64 PE binaries: {binary}")
    report = build_tool_output([objdump, "-p", str(binary)])
    names = re.findall(r"DLL Name:\s*(\S+)", report)
    system = {"advapi32.dll", "bcrypt.dll", "dxgi.dll", "gdi32.dll", "iphlpapi.dll", "kernel32.dll",
              "msvcrt.dll", "ntdll.dll", "ole32.dll", "psapi.dll", "secur32.dll", "shell32.dll",
              "shlwapi.dll", "ucrtbase.dll", "user32.dll", "userenv.dll", "version.dll", "ws2_32.dll"}
    if not names or any(name.lower() not in system and
                        not re.fullmatch(r"api-ms-win-crt-[a-z0-9-]+\.dll", name.lower()) for name in names):
        raise ValueError(f"non-system Windows dependency in {binary}: {names}")
    return sorted(set(names), key=str.lower)


def package_windows(runtime, colibri, output, repository=ROOT, objdump="objdump", runtime_repository=None):
    runtime, colibri, repository = (p.resolve(strict=True) for p in (runtime, colibri, repository))
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    files = [(regular(runtime / name, True), f"bin/{name}") for name in WINDOWS_BINARIES]
    files += [(regular(repository / "deploy/household-launcher.cmd"), "Lumabri.cmd"),
              (regular(repository / "deploy/verify-windows-package.ps1"), "Verify-Lumabri.ps1"),
              (regular(repository / "docs/WINDOWS_PACKAGE.md"), "START_HERE.md"),
              (regular(runtime / "WINDOWS-RUNTIME-NOTICES.txt"), "licenses/Windows-runtime-NOTICES.txt"),
              (regular(repository / "LICENSE"), "licenses/Lumabri-LICENSE"),
              (regular(colibri / "LICENSE"), "licenses/Colibri-LICENSE")]
    for name in ("NOTICE", "THIRD_PARTY_NOTICES.md"):
        if (colibri / name).exists():
            files.append((regular(colibri / name), f"licenses/Colibri-{name}"))
    report = {name: windows_dependencies(runtime / name, objdump) for name in WINDOWS_BINARIES}
    manifest = {"schema": "LMB-NATIVE-CANDIDATE-1", "status": "requires-native-and-physical-acceptance",
                "system": "Windows", "architecture": "x86_64", "build_os": platform.platform(),
                "minimum_windows_build": 18362, "lumabri": revision(runtime_repository or repository),
                "colibri_checkout": revision(colibri), "dependencies": report,
                "capabilities": {"local_adapters": ["olmoe"], "backend": "cpu",
                                 "household": True, "http_api_server": False},
                "signature": "unsigned; manifest hashes are consistency checks", "files": {}}
    output.mkdir(mode=0o755)
    for source, relative in files:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        # POSIX mode bits do not describe Windows ACLs or executable trust.
        manifest["files"][relative] = {"sha256": checksum(target), "bytes": target.stat().st_size}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def package(runtime, colibri, output, repository=ROOT, openmp_prefix=None, pe_objdump="objdump", runtime_repository=None):
    system = platform.system()
    if system == "Windows":
        if openmp_prefix is not None:
            raise ValueError("bundled OpenMP is a native macOS packaging option")
        return package_windows(runtime, colibri, output, repository, pe_objdump, runtime_repository)
    if runtime_repository is not None:
        raise ValueError("--runtime-repository applies only to independently built Windows runtime acceptance")
    if system not in ("Linux", "Darwin"):
        raise ValueError(f"unsupported package system: {system}")
    if openmp_prefix is not None and system != "Darwin":
        raise ValueError("bundled OpenMP is a native macOS packaging option")
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
              (regular(repository / ("docs/MACOS_PACKAGE.md" if system == "Darwin"
                                     else "docs/NATIVE_CANDIDATES.md")), "START_HERE.md", 0o644),
              (regular(repository / "deploy/household-launcher.sh"),
               "Lumabri.command" if system == "Darwin" else "start-lumabri", 0o755)]
    # Keep the upstream notices with the exact checkout used by this build.
    for name in ("NOTICE", "THIRD_PARTY_NOTICES.md"):
        if (colibri / name).exists():
            files.append((regular(colibri / name), f"licenses/Colibri-{name}", 0o644))
    omp = None
    if openmp_prefix is not None:
        library, licence, provenance, omp = openmp_files(openmp_prefix)
        files.extend([(library, "lib/lumabri/libomp.dylib", 0o644),
                      (licence, "licenses/OpenMP-LICENSE.TXT", 0o644),
                      (provenance, "licenses/OpenMP-SOURCE.json", 0o644)])
    report = {name: dependencies(runtime / name, system, library) if omp else dependencies(runtime / name, system)
              for name in (*BINARIES, shim)}
    if omp:
        report["libomp.dylib"] = dependencies(library, system)
        if not any(Path(link).name == "libomp.dylib" for link in macos_links(report["segment_node"], runtime / "segment_node")):
            raise ValueError("OpenMP package requested but Segment does not link OpenMP")
        for source in [runtime / name for name in (*BINARIES, shim)] + [library]:
            archs = subprocess.check_output(["lipo", "-archs", str(source)], text=True).split()
            if platform.machine() not in archs:
                raise ValueError(f"wrong architecture for this native candidate: {source}")
    minimum_macos = None
    if system == "Darwin":
        required = max(macos_minimum(runtime / name) for name in (*BINARIES, shim))
        if omp:
            required = max(required, macos_minimum(library))
        minimum_macos = ".".join(str(part) for part in required)
        target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
        if target and required > version_tuple(target):
            raise ValueError(f"a binary requires macOS {minimum_macos}, above the requested target {target}")
    manifest = {"schema": "LMB-NATIVE-CANDIDATE-1", "status": "requires-native-and-physical-acceptance",
                "system": system, "architecture": platform.machine(), "build_os": platform.platform(),
                "minimum_macos": minimum_macos,
                "lumabri": revision(repository), "colibri_checkout": revision(colibri),
                "dependencies": report, "files": {}}
    if omp:
        manifest["openmp"] = {**omp, "signature": "ad-hoc, not notarized"}
    # No model-runtime provenance is inferred from a directory name: retain
    # hashes of the actual shipped bytes as well as source checkout metadata.
    output.mkdir(mode=0o755)
    for source, relative, mode in files:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(mode)
    if omp:
        relocate_openmp(output, report)
        manifest["dependencies"] = {name: dependencies(
            output / (f"lib/lumabri/{name}" if name.endswith(".dylib") else f"bin/{name}"),
            system, output / "lib/lumabri/libomp.dylib", packaged=True) for name in report}
    if system == "Darwin":
        # Verify payloads with macOS's shipped shasum before executing an
        # unsigned candidate; Python remains build tooling only. This does
        # not authenticate the publisher or replace Gatekeeper approval.
        sums = output / "SHA256SUMS"
        sums.write_text(''.join(f"{checksum(output / relative)}  {relative}\n"
                                for _, relative, _ in files), encoding="utf-8")
        sums.chmod(0o644)
        files.append((sums, "SHA256SUMS", 0o644))
    for _, relative, mode in files:
        target = output / relative
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
        if index >= 64 or path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("unexpected entry or symlink in candidate")
        if path.is_file():
            actual.add(path.relative_to(directory).as_posix())
        elif not path.is_dir():
            raise ValueError("non-regular entry in candidate")
    if actual != set(files) | {"manifest.json"}:
        raise ValueError("candidate has missing or extra files")
    for name, expected in files.items():
        relative = PurePosixPath(name)
        if (relative.is_absolute() or ".." in relative.parts or str(relative) != name or
                any(c in name for c in '\\:<>"|?*') or any(p.endswith((".", " ")) for p in relative.parts)):
            raise ValueError("unsafe manifest path")
        path = regular(directory / name)
        if (not isinstance(expected, dict) or expected.get("sha256") != checksum(path) or
                expected.get("bytes") != path.stat().st_size or
                (manifest.get("system") != "Windows" and expected.get("mode") != oct(path.stat().st_mode & 0o777))):
            raise ValueError(f"candidate file changed: {name}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", type=Path, default=ROOT)
    parser.add_argument("--colibri-root", type=Path,
                        help="read-only source checkout used for the native build, including LICENSE")
    parser.add_argument("--openmp-prefix", type=Path,
                        help="native LLVM OpenMP installation built by tools/build_macos_openmp.py")
    parser.add_argument("--pe-objdump", default="objdump", help="PE dependency inspector for the Windows build")
    parser.add_argument("--runtime-repository", type=Path,
                        help="exact source checkout of a separately built previous Windows runtime")
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
        manifest = package(args.runtime_dir, args.colibri_root, args.output, openmp_prefix=args.openmp_prefix,
                           pe_objdump=args.pe_objdump, runtime_repository=args.runtime_repository)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Household candidate was not completed: {error}", file=sys.stderr)
        return 1
    print(f"Native {manifest['system']} {manifest['architecture']} candidate: {args.output}")
    print("Run the household acceptance flow from its bin directory before distributing it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
