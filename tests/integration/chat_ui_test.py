"""PTY + small VT screen oracle for the CLI's exact escape vocabulary.

Unlike a grep of output bytes, this checks text after cursor moves, erases,
scroll regions and scrollback. No model, network or Python UI dependency.
"""
import codecs
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import termios
import time
import unicodedata

ROOT = Path(__file__).resolve().parents[2]

plan = subprocess.check_output(["./test_chat_ui", "plan"], cwd=ROOT, text=True)
assert "Approved Segment plan: 2 compute donors" in plan
assert "layers [0,2)" in plan and "layers [2,4)" in plan
assert "This chat process runs no model layers" in plan
assert "No approved household plan" in plan
assert "\x1b" not in plan, "a peer name injected terminal controls"

advice = subprocess.check_output(["./test_chat_ui", "advice"], cwd=ROOT, text=True)
document, screen_text = advice.split("\n", 1)
models = json.loads(document)["models"]
assert models[0]["advice"] == "lowest RAM reservation"
assert models[1]["advice"] == "largest resident checkpoint"
assert "lowest RAM reservation" in screen_text and "largest resident checkpoint" in screen_text
assert "tok/s" not in screen_text, "resource advice invented a speed"


class Screen:
    def __init__(self, rows=16, cols=90):
        self.rows, self.cols = rows, cols
        self.lines = [[" "] * cols for _ in range(rows)]
        self.history, self.pending = [], ""
        self.x = self.y = 0
        self.saved = (0, 0)
        self.top, self.bottom = 0, rows - 1
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")

    def newline(self):
        if self.y == self.bottom:
            line = self.lines.pop(self.top)
            if self.top == 0 and self.bottom == self.rows - 1:
                self.history.append("".join(line))
            self.lines.insert(self.bottom, [" "] * self.cols)
        else:
            self.y = min(self.y + 1, self.rows - 1)

    def feed(self, data):
        text = self.pending + self.decoder.decode(data)
        self.pending = ""
        i = 0
        while i < len(text):
            c = text[i]
            if c == "\x1b":
                if i + 1 == len(text):
                    self.pending = text[i:]; break
                if text[i + 1] == "[":
                    end = i + 2
                    while end < len(text) and not "@" <= text[end] <= "~":
                        end += 1
                    if end == len(text):
                        self.pending = text[i:]; break
                    raw, op = text[i + 2:end], text[end]
                    if not raw.startswith("?"):
                        values = [int(v or "0") for v in raw.split(";")]
                        n = values[0] or 1
                        if op in "Hf":
                            self.y = min(n - 1, self.rows - 1)
                            self.x = min((values[1] if len(values) > 1 else 1) - 1, self.cols - 1)
                        elif op == "A": self.y = max(0, self.y - n)
                        elif op == "B": self.y = min(self.rows - 1, self.y + n)
                        elif op == "C": self.x = min(self.cols - 1, self.x + n)
                        elif op == "D": self.x = max(0, self.x - n)
                        elif op == "G": self.x = min(n - 1, self.cols - 1)
                        elif op == "K":
                            start = 0 if values[0] == 2 else self.x
                            self.lines[self.y][start:] = [" "] * (self.cols - start)
                        elif op == "r":
                            self.top = n - 1
                            self.bottom = values[1] - 1 if len(values) > 1 else self.rows - 1
                            self.x = self.y = 0
                    i = end + 1; continue
                if text[i + 1] == "7": self.saved = (self.x, self.y)
                elif text[i + 1] == "8": self.x, self.y = self.saved
                i += 2; continue
            if c == "\r": self.x = 0
            elif c == "\n": self.newline()
            elif c >= " ":
                width = 0 if unicodedata.combining(c) else (2 if unicodedata.east_asian_width(c) in "WF" else 1)
                if self.x + width > self.cols:
                    self.x = 0; self.newline()
                if width:
                    self.lines[self.y][self.x] = c
                    if width == 2 and self.x + 1 < self.cols:
                        self.lines[self.y][self.x + 1] = ""
                    self.x += width
            i += 1

    def text(self):
        return "\n".join(self.history + ["".join(line) for line in self.lines])


def run(editor=False):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 16, 90, 0, 0))
    p = subprocess.Popen(["./test_chat_ui"] + (["editor"] if editor else []), cwd=ROOT,
                         stdin=slave, stdout=slave, stderr=slave)
    screen = Screen()
    output = bytearray()

    def drain():
        while select.select([master], [], [], .02)[0]:
            data = os.read(master, 65536)
            if not data: break
            output.extend(data); screen.feed(data)

    def until(check):
        end = time.monotonic() + 15
        while time.monotonic() < end:
            drain()
            if check(): return
            time.sleep(.02)
        raise AssertionError(output.decode(errors="replace")[-3000:])

    try:
        if editor:
            until(lambda: b"USER-PREVIOUS" in output)
            os.write(master, b"/")
            until(lambda: b"Show cluster activity" in output)
            os.write(master, b"\x1b[B\t\n")
            until(lambda: p.poll() is not None)
            assert b"RESULT:/experts" in output
            assert "USER-PREVIOUS" in screen.text() and "ASSISTANT-PREVIOUS" in screen.text()
        else:
            until(lambda: p.poll() is not None)
            transcript = screen.text()
            for turn in range(2):
                assert f"USER-{turn}: keep my message" in transcript, transcript
                for row in range(35):
                    assert f"answer-{turn}-line-{row:02}" in transcript, transcript
            assert transcript.count("caffè 中文") == 2, transcript
        assert p.returncode == 0
        restored = termios.tcgetattr(slave)
        assert restored[3] & termios.ECHO and restored[3] & termios.ICANON
    finally:
        if p.poll() is None: p.terminate()
        p.wait(timeout=5)
        os.close(master); os.close(slave)


run(editor=True)
run()
print("CHAT UI: PASS (slash popup, arrows/Tab, transcript/scrollback, fragmented UTF-8, terminal restore)")
