"""Same-computer weight source: a hint, verified like any peer block.

A resident loader whose user also publishes the model on this computer reads
the blocks from the files instead of the encrypted loopback transport. This
proves the three properties that make that safe: the local files are used
only on a private record owned by the same user, a local file that differs
from the authenticated truth is refused and the peers serve instead, and a
loader without a record behaves exactly as before.
"""
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_shim_test import wait_registered  # noqa: E402

MODEL = "local-source"
PUBLISHER = r"""
#include "src/runtime/lumabri_local_source.h"
int main(int argc, char **argv) {
    return argc == 4 && !lmb_local_source_publish(argv[1], argv[2], argv[3]) ? 0 : 1;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="lumabri-local-source-") as directory:
        tmp = Path(directory).resolve()
        source = tmp / "source"
        source.mkdir()
        size = 2 * 1024 * 1024 + 777
        (source / "weights.safetensors").write_bytes(os.urandom(size))
        (source / "weights.safetensors.other.safetensors").write_bytes(b"z" * size)
        (source / "config.json").write_text('{"model":"local-source"}\n')
        children = []

        def environment(name):
            home = tmp / name
            home.mkdir(exist_ok=True)
            return {**os.environ, "HOME": str(home), "LUMABRI_ENCRYPT": "1",
                    "LUMABRI_TOKEN": "local-source-test", "LUMABRI_PEER_KEY": str(home / "identity"),
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
            tracker = launch("tracker", ["./tracker", "--port", str(port), "--token", "local-source-test"])
            ready(tracker, port)
            origin_port = free_port()
            origin = launch("origin", ["./maintainer", "--root", str(source), "--port", str(origin_port),
                "--tracker", f"127.0.0.1:{port}", "--name", "local-origin", "--model-name", MODEL])
            ready(origin, origin_port)
            wait_registered(origin, f"127.0.0.1:{port}", MODEL, "local-origin", 3,
                            {**environment("observer"), "LUMABRI_IO_TIMEOUT_MS": "1000"})
            loader, library = (("DYLD_INSERT_LIBRARIES", "liblumabri.dylib") if sys.platform == "darwin"
                               else ("LD_PRELOAD", "liblumabri.so"))
            compiler = os.environ.get("CC", "cc")
            client = tmp / "resident-client"
            subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/c/test_resident_input.c"), "-o", str(client),
                            *([] if sys.platform == "darwin" else ["-ldl"])], check=True, timeout=30)
            (tmp / "publisher.c").write_text(PUBLISHER)
            publisher = tmp / "publisher"
            subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                            str(tmp / "publisher.c"), "-o", str(publisher)], check=True, timeout=30)
            runs = iter(range(1000))

            def publish(records, root, model=MODEL):
                return subprocess.run([str(publisher), str(records), model, str(root)], timeout=10).returncode

            def load(records, scalar=False):
                """One resident preparation against the true source bytes."""
                tag = f"run{next(runs)}"
                env = {**environment(tag), loader: os.environ.get("LUMABRI_TEST_SHIM_LIBRARY", str(ROOT / library)),
                       "LUMABRI_MODEL": MODEL, "LUMABRI_TRACKER": f"127.0.0.1:{port}",
                       "LUMABRI_CAS": str(tmp / tag / "cas"), "LUMABRI_BLOCK_MIB": "1",
                       "LUMABRI_RESIDENT_REQUIRED": "1", "LUMABRI_PREFETCH": "0",
                       "LUMABRI_RESIDENCY_POLICY": "observe",
                       "LUMABRI_CACHE": str(tmp / tag / "cache"), "LUMABRI_VROOT": str(tmp / tag / "vroot")}
                if records is not None:
                    env["LUMABRI_LOCAL_SOURCES"] = str(records)
                if scalar:
                    env["LUMABRI_SHA_SCALAR"] = "1"
                done = subprocess.run([str(client), str(tmp / tag / "vroot/weights.safetensors"),
                                       str(source / "weights.safetensors")],
                                      env=env, capture_output=True, text=True, timeout=45)
                assert done.returncode == 0, done.stdout + done.stderr
                assert "preparation block cache: 9 fetches," in done.stderr, done.stderr
                # resident input never becomes a disk copy, whichever way it arrived
                assert (tmp / tag / "cache/data/weights.safetensors").stat().st_blocks == 0
                counts = re.search(r"preparation source: (\d+) verified block\(s\) from this "
                                   r"computer's own files, (\d+) from peers", done.stderr)
                return done.stderr, (tuple(map(int, counts.groups())) if counts else None)

            def served():
                """Blocks the origin has shipped over the encrypted transport."""
                probe = subprocess.run([str(ROOT / "swarm_probe"), "--tracker", f"127.0.0.1:{port}",
                                        "--model", MODEL], cwd=ROOT, env=environment("observer"),
                                       capture_output=True, text=True, timeout=10)
                assert probe.returncode == 0, probe.stderr
                return probe.stdout

            # 1. No record: the network path, unchanged, in both hash implementations.
            for scalar in (False, True):
                log, counts = load(None, scalar)
                assert counts is None and "this computer also holds" not in log, log
            log, counts = load(tmp / "absent-records")
            assert counts is None and "this computer also holds" not in log, log
            print("no record: network path unchanged")

            # 2. A private record of the true directory: every block is read locally.
            records = tmp / "records"
            assert publish(records, source) == 0
            assert (records.stat().st_mode & 0o777) == 0o700
            assert ((records / MODEL).stat().st_mode & 0o777) == 0o600
            assert (records / MODEL).read_text() == str(source)
            for scalar in (False, True):
                log, counts = load(records, scalar)
                assert "this computer also holds the model" in log, log
                assert counts == (9, 0), log
            print("private record: 9 verified blocks from local files, 0 from peers")

            # 3. A local file that is not the published one is refused block by
            #    block; the peers supply what it cannot, and the bytes are right.
            for where, local_ok in ((size - 5, 2), (3, 0)):
                forged = tmp / f"forged-{where}"
                shutil.copytree(source, forged)
                data = bytearray((forged / "weights.safetensors").read_bytes())
                data[where] ^= 1
                (forged / "weights.safetensors").write_bytes(data)
                forged_records = tmp / f"forged-records-{where}"
                assert publish(forged_records, forged) == 0
                log, counts = load(forged_records)
                assert "local copy of weights.safetensors does NOT match the authenticated truth" in log, log
                assert log.count("does NOT match") == 1, log  # retired after the first mismatch
                # the untouched shard still comes from its three local blocks
                assert counts is not None and counts[0] == 3 + local_ok and counts[1] == 6 - local_ok, log
            print("forged local file: refused, peers used, bytes exact")

            # 4. Same size is required before a single byte is read.
            short = tmp / "short"
            shutil.copytree(source, short)
            with open(short / "weights.safetensors", "ab") as handle:
                handle.write(b"x")
            (short / "weights.safetensors.other.safetensors").unlink()
            short_records = tmp / "short-records"
            assert publish(short_records, short) == 0
            log, counts = load(short_records)
            assert "local copy of weights.safetensors is not the announced file" in log, log
            assert counts == (0, 9), log
            print("wrong size or missing local file: peers used")

            # 5. A record anyone else could have written is not a record.
            loose = tmp / "loose-records"
            assert publish(loose, source) == 0
            loose.chmod(0o777)
            log, counts = load(loose)
            assert counts is None and "this computer also holds" not in log, log
            loose.chmod(0o700)
            (loose / MODEL).chmod(0o666)
            log, counts = load(loose)
            assert counts is None and "this computer also holds" not in log, log
            linked = tmp / "linked-records"
            linked.mkdir(mode=0o700)
            (linked / MODEL).symlink_to(records / MODEL)
            log, counts = load(linked)
            assert counts is None and "this computer also holds" not in log, log
            relative = tmp / "relative-records"
            relative.mkdir(mode=0o700)
            (relative / MODEL).write_text("source")
            (relative / MODEL).chmod(0o600)
            log, counts = load(relative)
            assert counts is None and "this computer also holds" not in log, log
            assert publish(loose.parent / "loose-records-2", source, "../escape") != 0
            assert publish(tmp / "missing-parent" / "records", source) != 0
            world = tmp / "world-records"
            world.mkdir()
            world.chmod(0o777)
            assert publish(world, source) != 0
            print("loose, linked or relative records: ignored")
            assert served()
        finally:
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()
            for child in children:
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
    print("local weight source: ok")


if __name__ == "__main__":
    main()
