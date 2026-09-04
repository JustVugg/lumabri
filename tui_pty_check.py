"""Quit the screen on a real pty and check the terminal came back.

Raw mode clears ICANON and ECHO. A clean exit has to put them back, from the
normal path, from a signal, and from atexit — all three routes land in the
same idempotent restore, and this checks the result rather than the routes."""
import os, pty, sys, termios, time

models = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.execvp("./lumabri", ["./lumabri", "models", "--models-dir", models])
time.sleep(2.0)
try:
    os.write(fd, b"q")
except OSError:
    pass
deadline = time.time() + 10
while time.time() < deadline:
    done, _ = os.waitpid(pid, os.WNOHANG)
    if done == pid:
        break
    time.sleep(0.1)
else:
    os.kill(pid, 9)
    print("the screen did not exit on q", file=sys.stderr)
    sys.exit(1)
after = termios.tcgetattr(fd)
if not (after[3] & termios.ICANON) or not (after[3] & termios.ECHO):
    print("ICANON/ECHO were still cleared after exit", file=sys.stderr)
    sys.exit(1)
sys.exit(0)
