"""Native libc interposition and signed CAS; no engine or Python ML dependency.

Compares bytes through the actual .so/.dylib, then rebuilds a fresh mirror
with the origin offline. All keys, ports and cache files are test-owned.
"""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="lumabri-native-shim-") as directory:
        tmp = Path(directory)
        source = tmp / "source"
        source.mkdir()
        (source / "weights.bin").write_bytes(os.urandom(2 * 1024 * 1024 + 777))
        (source / "config.json").write_text('{"model":"native-cas"}\n')
        children = []

        def environment(name):
            home = tmp / name
            home.mkdir(exist_ok=True)
            return {**os.environ, "HOME": str(home), "LUMABRI_ENCRYPT": "1",
                    "LUMABRI_TOKEN": "native-shim-test", "LUMABRI_PEER_KEY": str(home / "identity"),
                    "LUMABRI_KNOWN_HOSTS": str(home / "known"),
                    "LUMABRI_PEER_BINDINGS": str(tmp / "bindings")}

        def launch(name, arguments):
            with open(tmp / f"{name}.log", "wb") as log:
                child = subprocess.Popen(arguments, cwd=ROOT, env=environment(name),
                                         stdout=log, stderr=subprocess.STDOUT)
            children.append(child)
            return child

        def free_port():
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                return probe.getsockname()[1]

        def ready(child, port):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and child.poll() is None:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.2):
                        return
                except OSError:
                    time.sleep(.05)
            raise AssertionError("test service did not listen")

        try:
            port = free_port()
            tracker = launch("tracker", ["./tracker", "--port", str(port), "--token", "native-shim-test"])
            ready(tracker, port)
            origin_port = free_port()
            origin = launch("origin", ["./maintainer", "--root", str(source), "--port", str(origin_port),
                "--tracker", f"127.0.0.1:{port}", "--name", "native-origin", "--model-name", "native-cas"])
            ready(origin, origin_port)
            loader, library = (("DYLD_INSERT_LIBRARIES", "liblumabri.dylib") if sys.platform == "darwin"
                               else ("LD_PRELOAD", "liblumabri.so"))
            for name in ("cold", "offline-origin"):
                if name == "offline-origin":
                    origin.terminate(); origin.wait(timeout=10)
                env = {**environment(name), loader: str(ROOT / library),
                       "LUMABRI_MODEL": "native-cas", "LUMABRI_TRACKER": f"127.0.0.1:{port}",
                       "LUMABRI_CAS": str(tmp / "cas"), "LUMABRI_BLOCK_MIB": "1",
                       "LUMABRI_CACHE": str(tmp / f"cache-{name}"),
                       "LUMABRI_VROOT": str(tmp / f"vroot-{name}")}
                with subprocess.Popen([str(ROOT / "test_shim"), env["LUMABRI_VROOT"], str(source)],
                                      cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) as client:
                    try:
                        out, err = client.communicate(timeout=30)
                    except subprocess.TimeoutExpired:
                        if sys.platform == "darwin":
                            sample = tmp / "blocked-reader.log"
                            subprocess.run(["/usr/bin/sample", str(client.pid), "1", "1", "-file", str(sample)],
                                           capture_output=True, timeout=10)
                        client.kill()
                        out, err = client.communicate(timeout=5)
                        raise AssertionError(f"{name} reader blocked: {out}\n{err}")
                    if client.returncode:
                        raise AssertionError(f"{name}: {out}\n{err}")
            assert any((tmp / "cas").rglob("*")), "CAS was not populated"
            print("NATIVE SHIM: PASS (encrypted byte-exact libc reads, cold transfer, fresh mirror with origin offline)")
        except Exception:
            for log in tmp.glob("*.log"):
                print(log.name, log.read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
            for child in children:
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait()


if __name__ == "__main__":
    main()
