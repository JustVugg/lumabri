"""Bare app/chat entry points share the workspace; no implicit donation."""
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


def check(args, compact, apple=False):
    with tempfile.TemporaryDirectory(prefix="lumabri-workspace-") as home:
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 12 if compact else 38, 50 if compact else 104, 0, 0))
        env = {**os.environ, "HOME": home, "LUMABRI_ENCRYPT": "1",
               "LUMABRI_PEER_KEY": home + "/peer.key"}
        if apple:
            env.update(TERM_PROGRAM="Apple_Terminal", TERM="xterm-256color", COLORTERM="truecolor")
            env.pop("LUMABRI_COLOR", None)
            env.pop("NO_COLOR", None)
        p = subprocess.Popen([str(ROOT / "lumabri"), *args], env=env, cwd=ROOT,
                             stdin=slave, stdout=slave, stderr=slave)
        output = bytearray()

        def until(predicate):
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                while select.select([master], [], [], .02)[0]:
                    data = os.read(master, 65536)
                    if not data: break
                    output.extend(data)
                if predicate(): return
                time.sleep(.02)
            raise AssertionError(output.decode(errors="replace")[-3000:])

        try:
            until(lambda: (b"Resize the terminal" if compact else b"What would you like to do?") in output)
            if apple:
                assert b"\x1b[38;2;" not in output and b"\x1b[48;2;" not in output
                assert b"\x1b[48;5;234m" in output
                assert b"\x1b[48;5;238m" in output, "selection must have a contrasting background"
            if not compact:
                os.write(master, b"\r")
                until(lambda: b"Create or join a household first" in output)
                os.write(master, b"/")
                until(lambda: b"/create" in output and b"/join" in output and b"/settings" in output)
                os.write(master, b"\x1b\x1b")  # close actions, then exit; never swallow the second Esc
            else:
                os.write(master, b"\x1b")
            until(lambda: p.poll() is not None)
            assert p.returncode == 0
            after = termios.tcgetattr(slave)
            assert (after[3] & (termios.ICANON | termios.ECHO)) == (before[3] & (termios.ICANON | termios.ECHO))
            assert b"how do you want to join the swarm" not in output
            assert b"which swarm do you want to join" not in output
            assert not list(Path(home).rglob("*.safetensors"))
        finally:
            if p.poll() is None: p.terminate()
            p.wait(timeout=5)
            os.close(master); os.close(slave)


for args in ([], ["chat"]):
    for compact in (False, True):
        check(args, compact)
check([], False, apple=True)
print("WORKSPACE NAVIGATION: PASS (app/chat, actions, Apple palette/selection, no implicit sharing, compact view, terminal restore)")
