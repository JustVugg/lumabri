"""Visual-study regression tests. No model or network is involved."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build/tui-preview"


def snapshots():
    for view in ("home", "chat", "models", "computers", "share"):
        for width, height in ((104, 38), (60, 28), (180, 70)):
            result = subprocess.check_output([str(BIN), "--view", view, "--snapshot",
                                              "--width", str(width), "--height", str(height)], text=True)
            assert "DESIGN PREVIEW" in result
            assert "\x1b" not in result
            assert len(result.splitlines()) == height
            assert all(len(line) == width for line in result.splitlines())
    original = (ROOT / "lumabri.c").read_text().split("static const char *WORDMARK[6] = {", 1)[1].split("};", 1)[0]
    home = subprocess.check_output([str(BIN), "--snapshot"], text=True)
    for row in re.findall(r'"([^"]+)"', original):
        assert row in home, "existing Lumabri wordmark changed"
    assert subprocess.run([str(BIN), "--view", "bogus"], capture_output=True).returncode == 2


def terminal():
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 104, 0, 0))
    before = termios.tcgetattr(slave)
    with tempfile.TemporaryDirectory(prefix="lumabri-preview-test-") as home:
        p = subprocess.Popen([str(BIN)], stdin=slave, stdout=slave, stderr=slave,
                             env={**os.environ, "HOME": home})
        data = bytearray()

        def until(text):
            end = time.monotonic() + 10
            while time.monotonic() < end:
                if text.encode() in data:
                    return
                if select.select([master], [], [], .05)[0]:
                    data.extend(os.read(master, 65536))
            raise AssertionError(f"missing {text!r}: {bytes(data)[-2000:]!r}")

        def send(keys):
            data.clear()
            os.write(master, keys)

        try:
            until("What would you like to do?")
            send(b"\x1b[B\r")
            until("Choose what you want to run.")
            send(b"\x1b[B\r")
            until("DeepSeek V4 Flash")
            until("no active session")
            send(b"hello preview\r")
            until("hello preview")
            until("your message was not sent to a model.")
            send(b"/")
            until("Actions")
            send(b"\x1b[B\x1b[B\r")
            until("Your cluster. Your choice.")
            send(b"\r")
            until("[✓]")
            send(b"\x1b")
            until("What would you like to do?")
            send(b"\x1b")
            p.wait(timeout=5)
            assert p.returncode == 0
            assert termios.tcgetattr(slave) == before
            assert not list(Path(home).iterdir()), "prototype wrote runtime state"
        finally:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=5)
            os.close(master)
            os.close(slave)


snapshots()
terminal()
print("TUI PREVIEW: PASS (layouts, original logo, arrows/Enter, sample chat, actions, terminal restore)")
