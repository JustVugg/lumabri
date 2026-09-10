"""The real workspace storage action: approval, busy refusal and scoped clearing."""
import fcntl
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="lumabri-storage-ui-") as temp:
    home = Path(temp)
    cache = home / ".lumabri/home"
    chunk = cache / "cas/aa/chunk"
    mirror = cache / "mirrors/olmoe/cache/model.bin"
    for file in (chunk, mirror):
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(b"cached weights")
    source = home / "source-model.bin"
    source.write_bytes(b"original model")
    log = cache / "engines.log"
    log.write_bytes(b"keep log")
    lease = open(cache / "weights.lock", "w")
    fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 120, 0, 0))
    env = {**os.environ, "HOME": temp, "LUMABRI_ENCRYPT": "1",
           "LUMABRI_PEER_KEY": str(home / "peer.key")}
    app = subprocess.Popen([str(ROOT / "lumabri")], cwd=ROOT, env=env,
                           stdin=slave, stdout=slave, stderr=slave)
    output = bytearray()

    def until(predicate):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            while select.select([master], [], [], .02)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if not data:
                    break
                output.extend(data)
            if predicate():
                return
            time.sleep(.02)
        raise AssertionError(output.decode(errors="replace")[-4000:])

    def wait_for(text):
        until(lambda: text.encode() in output)

    def send(keys):
        output.clear()
        os.write(master, keys)

    try:
        wait_for("What would you like to do?")
        send(b"/"); wait_for("/storage")
        send(b"\x1b[A\r"); wait_for("Household weight storage")
        wait_for("Weights are in use")
        send(b"\x1b[B\r"); wait_for("Cleanup is unavailable")
        assert chunk.exists() and mirror.exists()
        fcntl.flock(lease, fcntl.LOCK_UN)
        send(b"\x1b[A\r"); wait_for("Only household cas/ and mirrors/")
        send(b"\x1b[B\r"); wait_for("Clear unused household weights?")
        send(b"\r"); wait_for("Household weight storage")
        assert chunk.exists() and mirror.exists(), "default confirmation deleted weights"
        send(b"\x1b[B\r"); wait_for("Clear unused household weights?")
        send(b"\x1b[B\r"); wait_for("Household weights cleared")
        assert not chunk.exists() and not mirror.exists()
        assert source.read_bytes() == b"original model" and log.read_bytes() == b"keep log"
        assert (cache / "weights.lock").exists(), "cleanup removed the lease inode"
        send(b"\x1b"); wait_for("What would you like to do?")
        # Keep consuming terminal output while waiting for exit. Darwin PTYs
        # have a smaller output queue; blocking in wait() can strand the app
        # in its final repaint before it reads Escape or restores termios.
        send(b"\x1b"); until(lambda: app.poll() is not None)
        assert app.returncode == 0
        after = termios.tcgetattr(slave)
        mask = termios.ICANON | termios.ECHO
        assert (before[3] & mask) == (after[3] & mask)
    finally:
        lease.close()
        if app.poll() is None:
            app.terminate()
            try:
                until(lambda: app.poll() is not None)
            except AssertionError:
                app.kill()
        app.wait(timeout=5)
        os.close(master); os.close(slave)

print("STORAGE UI: PASS (busy refusal, safe default, confirmed cleanup, sources/logs preserved, terminal restored)")
