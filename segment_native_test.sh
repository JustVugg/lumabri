#!/usr/bin/env bash
# User-facing gate: launch the ordinary serve/chat commands, intentionally ask
# for more context than the tiny origin advertises, and require Segment rather
# than allowing the classic fallback to hide an integration regression.
set -euo pipefail
cd "$(dirname "$0")"

: "${OLMOE_EDGE_MODEL:?set OLMOE_EDGE_MODEL}"

TMP=$(mktemp -d /tmp/lumabri-segment-native.XXXXXX)
SERVER_PID=""
cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep .05
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

wait_port() {
    local port=$1
    for _ in $(seq 1 400); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3<&-; exec 3>&-; return 0
        fi
        sleep 0.025
    done
    return 1
}

mkdir -p "$TMP/server-home" "$TMP/client-home"
HOME="$TMP/server-home" ./lumabri key --out "$TMP/swarm" >/dev/null

HOME="$TMP/server-home" LUMABRI_SEGMENT_CHUNKS=2 LUMABRI_SEGMENT_SESSIONS=8 \
    LUMABRI_SEGMENT_MIN_FREE_MB=1024 OMP_NUM_THREADS=2 \
    stdbuf -oL -eL ./lumabri serve --model "$OLMOE_EDGE_MODEL" --port 7880 \
    --key "$TMP/swarm.key" \
    >"$TMP/server.log" 2>&1 &
SERVER_PID=$!

if ! wait_port 7880 || ! wait_port 7883 || ! wait_port 7884; then
    cat "$TMP/server.log"
    echo "SEGMENT NATIVE: origin did not become ready" >&2
    exit 1
fi

set +e
printf 'hi\n/quit\n' | HOME="$TMP/client-home" OMP_NUM_THREADS=2 \
    LUMABRI_PUBKEY="$TMP/swarm.pub" LUMABRI_SEGMENT_REQUIRED=1 \
    ./lumabri chat --plain --tracker 127.0.0.1:7880 --ctx 2048 \
    --max-new 2 --role chat >"$TMP/chat.log" 2>&1
status=$?
set -e
if (( status != 0 )) || ! grep -q 'pronto via Segment' "$TMP/chat.log" ||
   ! grep -q 'Segment context negotiated to' "$TMP/chat.log" ||
   ! grep -q 'data plane relay (nessuna porta pubblica richiesta)' "$TMP/server.log" ||
   grep -q 'continuo con il percorso expert/CAS' "$TMP/chat.log" ||
   grep -q 'Segment route generation' "$TMP/chat.log"; then
    cat "$TMP/server.log" "$TMP/chat.log"
    echo "SEGMENT NATIVE: ordinary serve/chat gate failed" >&2
    exit 1
fi

# A real PTY is required to exercise the live dock. Type `/exp<Tab>` while a
# deliberately latency-shaped Segment turn is still running, then queue quit.
# The executor panel must appear before the turn's final statistics: menus and
# autocomplete are therefore live, not a code path reached after inference.
mkdir -p "$TMP/client-home-pty"
HOME="$TMP/client-home-pty" OMP_NUM_THREADS=2 \
LUMABRI_PUBKEY="$TMP/swarm.pub" LUMABRI_SEGMENT_REQUIRED=1 \
LUMABRI_RTT_US=100000 python3 - "$TMP/pty.log" <<'PY'
import fcntl, os, pty, select, struct, subprocess, sys, termios, time
log_path = sys.argv[1]
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
p = subprocess.Popen([
    "./lumabri", "chat", "--tracker", "127.0.0.1:7880", "--ctx", "2048",
    "--max-new", "8", "--role", "chat",
], stdin=slave, stdout=slave, stderr=slave, close_fds=True,
   start_new_session=True)
os.close(slave)
out = bytearray()
deadline = time.monotonic() + 45
sent = quit_sent = False
log = open(log_path, "wb")
while time.monotonic() < deadline:
    ready, _, _ = select.select([master], [], [], .1)
    if ready:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            chunk = b""
        if not chunk:
            break
        out.extend(chunk)
        log.write(chunk); log.flush()
    if not sent and "pronto via Segment".encode() in out:
        os.write(master, b"hi\n")
        time.sleep(.15)
        os.write(master, b"/exp\t\n")
        sent = True
    if sent and not quit_sent and "uso degli executor".encode() in out:
        os.write(master, b"/quit\n")
        quit_sent = True
    if p.poll() is not None:
        break
if p.poll() is None:
    os.killpg(p.pid, 15)
    try: p.wait(5)
    except subprocess.TimeoutExpired: os.killpg(p.pid, 9); p.wait()
try: os.close(master)
except OSError: pass
log.close()
panel = "uso degli executor".encode()
stats = b"tok/s"
if (not sent or not quit_sent or p.returncode != 0 or b"\x1b[1;27r" not in out or
        panel not in out or stats not in out or out.index(panel) > out.index(stats) or
        (b"prefill" not in out and b"decode" not in out)):
    sys.stderr.buffer.write(out)
    raise SystemExit("live TUI dock/autocomplete gate failed")

def control_case(name, action):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack("HHHH", 30, 100, 0, 0))
    original = termios.tcgetattr(slave)
    probe = os.dup(slave)
    process = subprocess.Popen([
        "./lumabri", "chat", "--tracker", "127.0.0.1:7880", "--ctx", "2048",
        "--max-new", "64", "--role", "chat",
    ], stdin=slave, stdout=slave, stderr=slave, close_fds=True,
       start_new_session=True)
    os.close(slave)
    transcript = bytearray()

    def pump_until(needles, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], .05)
            if ready:
                try: chunk = os.read(master, 65536)
                except OSError: chunk = b""
                if chunk: transcript.extend(chunk)
            if all(needle in transcript for needle in needles): return True
            if process.poll() is not None: return False
        return False

    if not pump_until(["pronto via Segment".encode()], 30):
        sys.stderr.buffer.write(transcript)
        raise SystemExit(name + ": chat did not become ready")
    os.write(master, b"hi\n")
    if (not pump_until([b"\x1b[1;27r"], 10) or
            not pump_until([b"prefill"], 10)):
        sys.stderr.buffer.write(transcript)
        raise SystemExit(name + ": inference dock did not become active")
    current = termios.tcgetattr(probe)
    if current[3] & (termios.ICANON | termios.ECHO | termios.ISIG):
        raise SystemExit(name + ": dock did not own raw input")

    started = time.monotonic()
    if isinstance(action, bytes):
        os.write(master, action)
    else:
        os.kill(process.pid, action)

    try: process.wait(5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL); process.wait()
        raise SystemExit(name + ": shutdown waited for the active turn")
    elapsed = time.monotonic() - started
    restored = termios.tcgetattr(probe) == original
    try: os.close(master)
    except OSError: pass
    os.close(probe)
    if process.returncode != 0 or elapsed >= 5 or not restored:
        sys.stderr.buffer.write(transcript)
        raise SystemExit(name + ": process/termios cleanup failed")

import signal
control_case("Ctrl-C", b"\x03")
control_case("Ctrl-backslash", b"\x1c")
control_case("external SIGTERM", signal.SIGTERM)

# Job-control stop signals are discarded for an orphan process group.  Put an
# interactive shell in charge of the PTY and run Lumabri as its foreground job,
# matching a real terminal: Ctrl-Z must hand canonical mode back to the shell,
# and `fg` must reconstruct the raw dock before Ctrl-C exits.
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
original = termios.tcgetattr(slave)
probe = os.dup(slave)
def shell_session():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
shell = subprocess.Popen(["bash", "--noprofile", "--norc", "-i"],
    stdin=slave, stdout=slave, stderr=slave, close_fds=True,
    preexec_fn=shell_session)
os.close(slave)
transcript = bytearray()
def shell_pump(predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], .05)
        if ready:
            try: chunk = os.read(master, 65536)
            except OSError: chunk = b""
            if chunk: transcript.extend(chunk)
        if predicate(): return True
        if shell.poll() is not None: return False
    return False
if not shell_pump(lambda: transcript.endswith((b"# ", b"$ ")), 5):
    raise SystemExit("Ctrl-Z: job-control shell did not become ready")
shell_term = termios.tcgetattr(probe)
command = ("./lumabri chat --tracker 127.0.0.1:7880 --ctx 2048 "
           "--max-new 64 --role chat\n").encode()
os.write(master, command)
if not shell_pump(lambda: "pronto via Segment".encode() in transcript, 30):
    raise SystemExit("Ctrl-Z: chat did not become ready under job-control shell")
os.write(master, b"hi\n")
if not shell_pump(lambda: b"prefill" in transcript, 10):
    raise SystemExit("Ctrl-Z: inference dock did not become active")
os.write(master, b"\x1a")
if not shell_pump(lambda: b"Stopped" in transcript, 5):
    sys.stderr.buffer.write(transcript)
    raise SystemExit("Ctrl-Z: foreground job was not suspended")
deadline = time.monotonic() + 2
while time.monotonic() < deadline:
    stopped_term = termios.tcgetattr(probe)
    if stopped_term == shell_term: break
    time.sleep(.02)
else:
    sys.stderr.write("Ctrl-Z flags: stopped=%#x shell=%#x pty-original=%#x\n" %
                     (stopped_term[3], shell_term[3], original[3]))
    sys.stderr.buffer.write(transcript)
    raise SystemExit("Ctrl-Z: terminal stayed raw while the shell had control")
os.write(master, b"fg\n")
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    current = termios.tcgetattr(probe)
    if not current[3] & (termios.ICANON | termios.ECHO | termios.ISIG): break
    time.sleep(.02)
else:
    raise SystemExit("Ctrl-Z: dock did not return to raw mode after fg")
os.write(master, b"\x03")
if not shell_pump(lambda: b"inferenza interrotta" in transcript, 5):
    raise SystemExit("Ctrl-Z: resumed chat did not accept Ctrl-C")
os.write(master, b"exit\n")
try: shell.wait(5)
except subprocess.TimeoutExpired:
    os.kill(shell.pid, signal.SIGKILL); shell.wait()
    raise SystemExit("Ctrl-Z: job-control shell did not exit")
restored = termios.tcgetattr(probe)
if (shell.returncode != 0 or
        not restored[3] & termios.ICANON or
        not restored[3] & termios.ECHO or
        not restored[3] & termios.ISIG):
    raise SystemExit("Ctrl-Z: terminal was not restored after resume and exit")
try: os.close(master)
except OSError: pass
os.close(probe)
PY

echo "SEGMENT NATIVE: PASS (serve + live TUI, signals/termios, NAT relay + context negotiation)"
