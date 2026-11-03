"""Installed native package, launcher, update/rollback and integrity gates.

The C driver creates and removes its own private Windows profile, checkpoints
and encrypted household. No installed user service or firewall is modified.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, *, success=True, timeout=60):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    text = result.stdout.decode("utf-8", errors="replace")
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Unexpected status {result.returncode}: {text}")
    return text


def verify(directory, success=True):
    return run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File",
                str(directory / "Verify-Lumabri.ps1"), "-Directory", str(directory)], success=success)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--previous", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    args = parser.parse_args()
    candidate, previous, driver, fixture = (p.resolve(strict=True) for p in
                                           (args.candidate, args.previous, args.driver, args.fixture))
    for package in (candidate, previous):
        print(verify(package), flush=True)
    launcher = candidate / "Lumabri.cmd"
    # cmd's /s /c expects an outer quote pair around a quoted executable path.
    output = run(f'cmd.exe /d /s /c ""{launcher}" doctor --household --json"')
    report = json.loads(output)
    assert report["ok"] and len(report["checks"]) == 4
    print("WINDOWS LAUNCHER: PASS (bounded runtime startup, no model loaded)", flush=True)
    with tempfile.TemporaryDirectory(prefix="lumabri-package-check-") as temporary:
        broken = Path(temporary) / "changed package"
        shutil.copytree(candidate, broken)
        target = broken / "bin/segment_chat.exe"
        original = target.read_bytes()
        target.write_bytes(b"changed")
        verify(broken, success=False)
        target.write_bytes(original)
        extra = broken / "extra.txt"
        extra.write_text("not declared", encoding="utf-8")
        verify(broken, success=False)
        extra.unlink()
        target.unlink()
        verify(broken, success=False)
        output = run([str(broken / "bin/lumabri.exe"), "doctor", "--household", "--json"], success=False)
        assert not json.loads(output)["ok"]
    print("WINDOWS PACKAGE INTEGRITY: PASS (changed, extra and missing files rejected)", flush=True)
    print(run([str(driver), str(previous / "bin/lumabri.exe"), str(fixture),
               str(candidate / "bin/lumabri.exe")], timeout=360), flush=True)
    for package in (candidate, previous):
        print(verify(package), flush=True)
    print("WINDOWS INSTALLED PACKAGE: PASS", flush=True)


if __name__ == "__main__":
    main()
