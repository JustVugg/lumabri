#!/usr/bin/env python3
"""Run a release archive the way a person does: unpack it, start it, open the page.

Only the unpacked archive is used: its launchers, its binaries, a fresh home
and a private port. No model is downloaded or loaded. Python is test tooling;
the archive itself needs none.

  release_archive_smoke_test.py --archive build/release/lumabri-<v>-<row>.tar.gz
"""
import argparse
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"tools/{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def unpack(archive, destination):
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as packed:
            for name in packed.namelist():
                assert not name.startswith(("/", "\\")) and ".." not in Path(name).parts, name
            packed.extractall(destination)
    else:
        with tarfile.open(archive, "r:gz") as packed:
            packed.extractall(destination, filter="data")
    entries = list(destination.iterdir())
    assert len(entries) == 1 and entries[0].is_dir(), f"one top-level folder expected: {entries}"
    assert archive.name.startswith(entries[0].name + "."), (archive.name, entries[0].name)
    return entries[0]


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--port", type=int, help="local page port (default: a free one)")
    parser.add_argument("--work-dir", type=Path, help="parent of the temporary unpack and home folders")
    args = parser.parse_args()
    release, packager = load("release_archive"), load("package_household")
    archive = args.archive.resolve(strict=True)
    match = release.ARCHIVE_NAME.fullmatch(archive.name)
    assert match, f"not a release archive name: {archive.name}"
    system, _, _, start, launcher = release.PLATFORMS[match.group(1)]
    recorded = (archive.parent / (archive.name + ".sha256")).read_text(encoding="utf-8")
    assert recorded == f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n", "archive checksum"

    work = Path(tempfile.mkdtemp(prefix="lmb-smoke-", dir=args.work_dir))
    home = work / "home"
    home.mkdir()
    folder = unpack(archive, work / "download folder")
    manifest = packager.verify(folder)
    for name in (start, launcher):
        text = (folder / name).read_text(encoding="utf-8")
        # The program picks the address and opens the browser, not the launcher.
        assert "://" not in text and "xdg-open" not in text, f"{name} must leave the page to the program"
        assert system == "Windows" or os.access(folder / name, os.X_OK), f"{name} is not executable"

    env = {key: value for key, value in os.environ.items() if key != "LUMABRI_TOKEN"}
    env["HOME"] = str(home)
    if system == "Windows":
        env.update(USERPROFILE=str(home), LOCALAPPDATA=str(home / "AppData/Local"), APPDATA=str(home / "AppData/Roaming"))
        for key in ("LOCALAPPDATA", "APPDATA"):
            Path(env[key]).mkdir(parents=True)

    def run(name, *arguments, timeout=180):
        # cmd's /s /c expects an outer quote pair around a quoted executable path.
        command = (f'cmd.exe /d /s /c ""{folder / name}" {" ".join(arguments)}"' if system == "Windows"
                   else [str(folder / name), *arguments])
        return subprocess.run(command, env=env, cwd=work, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=timeout)

    if system == "Windows":
        # The check a person is told to run first; it executes no packaged program.
        subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                        str(folder / "Verify-Lumabri.ps1"), "-Directory", str(folder)], check=True, timeout=120)

    # The Windows program gains `start` and the page with its web gateway;
    # until its manifest says so, the Windows archive is checked by its doctor.
    page = system != "Windows" or manifest.get("capabilities", {}).get("http_api_server") is True
    port = args.port or free_port()
    started = False
    try:
        if not page:
            report = run(launcher, "doctor", "--household", "--json")
            assert report.returncode == 0, report.stdout + report.stderr
            json.loads(report.stdout)
            print(f"{folder.name}: runtime doctor PASS (this archive has no local page yet)")
        else:
            started = True
            first = run(start, "--no-browser", "--port", str(port))
            assert first.returncode == 0, first.stdout + first.stderr
            prefix = f"http://127.0.0.1:{port}/#token="
            links = [line.strip() for line in first.stdout.splitlines() if line.strip().startswith(prefix)]
            assert len(links) == 1, first.stdout
            token = links[0][len(prefix):]

            def request(path, bearer=None):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
                try:
                    connection.request("GET", path, headers={"Authorization": "Bearer " + bearer} if bearer else {})
                    answer = connection.getresponse()
                    return answer.status, answer.read()
                finally:
                    connection.close()

            status, body = request("/")
            # The page compiled into the binary, with the first-run card.
            assert status == 200 and b"<title>" in body and b"Get a model" in body, (status, body[:200])
            assert request("/api/v1/models")[0] == 401
            status, body = request("/api/v1/session", token)
            assert status == 200 and json.loads(body)["operator"] is True, (status, body)
            status, body = request("/api/v1/models", token)
            assert status == 200 and json.loads(body)["models"] == [], (status, body)
            # A new computer has no model: the page must be able to offer one.
            status, body = request("/api/v1/downloads", token)
            offer = json.loads(body)
            assert status == 200 and offer["catalogue_ok"] is True and offer["models"], (status, body[:400])
            if system != "Windows":
                assert offer["downloader_available"] is True, "bin/lumabri-fetch does not run from the archive"
            print(f"{folder.name}: start, page, sign-in and first-model offer PASS")
        if page:
            stopped = run(launcher, "service", "stop")
            assert stopped.returncode == 0, stopped.stdout + stopped.stderr
            started = False
            status = run(launcher, "service", "status", "--json")
            live = [s["role"] for s in json.loads(status.stdout)["services"] if s["live"]]
            assert not live, f"still running after service stop: {live}"
            with socket.socket() as probe:
                assert probe.connect_ex(("127.0.0.1", port)) != 0, "the page still answers after service stop"
        assert packager.verify(folder) == manifest, "running the archive changed its folder"
        print(f"{folder.name}: {'service stop' if page else 'unchanged folder'} PASS")
    except BaseException:
        for log in sorted(home.rglob("*.log"))[:12]:
            print(f"--- {log}\n{log.read_text(errors='replace')[-2000:]}", file=sys.stderr)
        raise
    finally:
        if started:
            run(launcher, "service", "stop")
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
