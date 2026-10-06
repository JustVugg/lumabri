#!/usr/bin/env python3
"""TEST ONLY: interrupt real Segment codec output, never synthesize inference.

Install as segment_chat beside segment_chat.real in a disposable candidate.
Only the isolated test's explicit control file activates a fault. Full real
output is captured as an oracle before an injected mid-stream transport/error
boundary. This avoids timing races with a tiny model completing in milliseconds.
"""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def install(target):
    """Pin the test interpreter, avoiding Apple's arm64e /usr/bin/env loader.

    The real native engine is arm64 and its injected weight loader is too.
    Routing that injection through an arm64e system executable fails before
    Python can run. CI's setup-python interpreter matches its native build.
    Only install beside an explicitly saved real engine, never overwrite it.
    """
    target = Path(target)
    interpreter = str(Path(sys.executable).resolve())
    if any(c.isspace() for c in interpreter) or len(os.fsencode(interpreter)) > 120:
        raise ValueError("test interpreter path cannot be represented by a portable shebang")
    if target.name != "segment_chat" or not target.with_name("segment_chat.real").is_file():
        raise ValueError("save the real engine as segment_chat.real in a disposable test runtime first")
    source = Path(__file__).read_bytes().split(b"\n", 1)[1]
    with target.open("xb") as installed:
        installed.write(b"#!" + os.fsencode(interpreter) + b"\n" + source)
        os.fchmod(installed.fileno(), 0o755)


def main():
    real = str(Path(__file__).with_name("segment_chat.real"))
    if "--serve" not in sys.argv or not os.environ.get("LUMABRI_TEST_RECOVERY_CONTROL"):
        os.execv(real, [real, *sys.argv[1:]])
    model = sys.argv[sys.argv.index("--model") + 1]
    control = Path(os.environ["LUMABRI_TEST_RECOVERY_CONTROL"])
    child_env = dict(os.environ)
    if sys.platform == "darwin":
        # Interpreter boundaries can strip DYLD_*.
        # Restore only the disposable candidate's own loader for its real
        # Segment child, as the normal native Lumabri launcher already does.
        directory = Path(real).parent
        shim = next((p for p in (directory / "liblumabri.dylib",
            directory.parent / "lib/lumabri/liblumabri.dylib") if p.is_file()), None)
        assert shim is not None, "candidate weight loader missing"
        child_env["DYLD_INSERT_LIBRARIES"] = str(shim.resolve())
    child = subprocess.Popen([real, *sys.argv[1:]], env=child_env, stdin=sys.stdin.buffer, stdout=subprocess.PIPE)
    seen = set()
    fault = None
    generated = bytearray()
    visible = b""
    out = sys.stdout.buffer
    try:
        while True:
            line = child.stdout.readline()
            if not line:
                break
            if line.startswith(b"DATA "):
                _, request, size = line.split()
                size = int(size)
                assert 0 <= size <= 8 << 20
                data = child.stdout.read(size)
                assert len(data) == size and child.stdout.read(1) == b"\n"
                if not fault:
                    try:
                        settings = json.loads(control.read_text())
                        selected = settings["models"].get(model)
                        if selected and settings["nonce"] not in seen:
                            fault = {**selected, "nonce": settings["nonce"]}
                            seen.add(fault["nonce"])
                            generated.clear()
                            visible = b""
                    except (FileNotFoundError, json.JSONDecodeError):
                        pass
                if fault:
                    generated.extend(data)
                    if fault["mode"] == "observe":
                        out.write(line + data + b"\n"); out.flush()
                        continue
                    if not visible and data and fault["mode"] != "short":
                        visible = data[:1]
                        if fault["mode"] == "diverge":
                            visible = bytes([visible[0] ^ 1])
                        out.write(b"DATA " + request + b" 1\n" + visible + b"\n")
                        out.flush()
                else:
                    out.write(line + data + b"\n"); out.flush()
            elif fault and line.startswith(b"DONE "):
                nonce, mode = fault["nonce"], fault["mode"]
                record = control.parent / (nonce + "-" + model + ".json")
                pending = record.with_suffix(".pending")
                pending.write_text(json.dumps({"mode": mode, "model": model,
                    "oracle": base64.b64encode(generated).decode(), "visible": base64.b64encode(visible).decode()}))
                pending.replace(record)
                if fault.get("wait"):
                    deadline = time.monotonic() + 15
                    while not (control.parent / (nonce + ".release")).exists():
                        if time.monotonic() >= deadline:
                            raise TimeoutError("fault release missing")
                        time.sleep(.02)
                if mode == "truncate":
                    return 0
                if mode in ("short", "observe"):
                    out.write(line)
                else:
                    reason = b"invalid request fixture" if mode == "nonretryable" else b"LMB_REPLICA_UNAVAILABLE injected real-output boundary"
                    out.write(b"ERROR " + line.split()[1] + b" " + reason + b"\n")
                out.flush(); fault = None
            elif fault and line.startswith(b"ERROR "):
                # A genuine engine failure/cancellation is not a fixture's
                # successful oracle and must never be swallowed by the proxy.
                out.write(line); out.flush(); fault = None
            elif not fault:
                out.write(line); out.flush()
        return child.wait(timeout=10)
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait(timeout=5)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--install":
        install(sys.argv[2])
    else:
        raise SystemExit(main())
