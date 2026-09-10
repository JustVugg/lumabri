"""Native libc interposition and signed CAS; no engine or Python ML dependency.

Compares bytes through the actual .so/.dylib, then rebuilds a fresh mirror
with the origin offline. All keys, ports and cache files are test-owned.
"""
import os
import json
import struct
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

            # A real virtual checkpoint, not a mocked read counter. Inspection
            # needs the header and a small layout at the far end of a 64-MiB
            # payload. fopen used to fetch the entire payload here, even on a
            # donor assigned only a few layers. Keep the unused middle cold.
            header_source = tmp / "header-source"
            header_source.mkdir()
            payload = 64 * 1024 * 1024
            header = json.dumps({
                "weights": {"dtype": "U8", "shape": [payload-16], "data_offsets": [0,payload-16]},
                "layout": {"dtype": "I64", "shape": [2], "data_offsets": [payload-16,payload]},
            }, separators=(",", ":")).encode()
            middle = 8 + len(header) + payload // 2
            with (header_source / "weights.safetensors").open("wb") as f:
                f.write(struct.pack("<Q", len(header))); f.write(header)
                f.seek(middle); f.write(b"MUST_NOT_BE_FETCHED")
                f.seek(8 + len(header) + payload - 16); f.write(struct.pack("<QQ",17,29))
            header_port = free_port()
            header_origin = launch("header-origin", ["./maintainer", "--root", str(header_source),
                "--port", str(header_port), "--tracker", f"127.0.0.1:{port}",
                "--name", "header-origin", "--model-name", "header-only"])
            ready(header_origin, header_port)
            env = {**environment("header-client"), loader: str(ROOT / library),
                   "LUMABRI_MODEL": "header-only", "LUMABRI_TRACKER": f"127.0.0.1:{port}",
                   "LUMABRI_CAS": str(tmp / "header-cas"), "LUMABRI_BLOCK_MIB": "1",
                   "LUMABRI_PREFETCH": "0", "LUMABRI_CACHE": str(tmp / "header-cache"),
                   "LUMABRI_VROOT": str(tmp / "header-vroot")}
            p = subprocess.run([str(ROOT / "test_planner_io"),
                                str(tmp / "header-vroot/weights.safetensors")],
                               cwd=ROOT, env=env, capture_output=True, text=True, timeout=45)
            assert p.returncode == 0, p.stdout + p.stderr
            mirror = tmp / "header-cache/data/weights.safetensors"
            with mirror.open("rb") as f:
                f.seek(middle)
                assert f.read(19) == bytes(19), "planner materialized unused weight payload"
                f.seek(8 + len(header) + payload - 16)
                assert f.read(16) == struct.pack("<QQ",17,29)
            allocated = mirror.stat().st_blocks * 512
            assert allocated <= 4 * 1024 * 1024, f"header inspection allocated {allocated} bytes"
            print(f"BOUNDED PLANNER SHIM: PASS (64-MiB payload, {allocated} allocated bytes, unused middle cold)")
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
