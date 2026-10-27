"""Stage only a verified native Linux candidate for a container build.

Does not run Docker, download anything, publish images or read user settings.
The package manifest checks consistency, not publisher authenticity.
"""
import argparse
from pathlib import Path
import shutil

from package_household import verify

ROOT = Path(__file__).resolve().parents[1]


def stage(package, output):
    package = package.resolve(strict=True)
    manifest = verify(package)
    if manifest["system"] != "Linux" or manifest["architecture"] not in ("x86_64", "aarch64"):
        raise ValueError("a native Linux x86_64 or aarch64 package is required")
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to overwrite an existing context")
    if output.resolve().is_relative_to(package):
        raise ValueError("context must be outside the package")
    output.mkdir(mode=0o700, parents=True)
    shutil.copytree(package, output / "runtime")
    # Recheck the bytes actually staged, including modes and absence of extras.
    if verify(output / "runtime") != manifest:
        raise ValueError("package changed while staging")
    shutil.copyfile(ROOT / "tools/package_household.py", output / "package_household.py")
    shutil.copyfile(ROOT / "deploy/kubernetes/Dockerfile.runtime", output / "Dockerfile")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    manifest = stage(args.package, args.output)
    print("Staged native Linux", manifest["architecture"], "package; no models or user state included.")
