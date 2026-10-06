"""Check the disposable fault proxy installer, not model arithmetic."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

proxy = Path(__file__).with_name("recovery_codec_proxy.py")
with tempfile.TemporaryDirectory(prefix="lmb-recovery-proxy-") as directory:
    target = Path(directory) / "segment_chat"
    command = [sys.executable, str(proxy), "--install", str(target)]
    missing = subprocess.run(command, capture_output=True)
    assert missing.returncode and not target.exists(), "installed without saved real engine"
    real = target.with_name("segment_chat.real")
    # The pass-through branch is enough to test installation and exec; the
    # household gate separately runs and faults real Segment arithmetic.
    shutil.copyfile(shutil.which("true"), real)
    real.chmod(0o755)
    before = real.read_bytes()
    subprocess.run(command, check=True)
    assert target.read_bytes().splitlines()[0] == b"#!" + os.fsencode(Path(sys.executable).resolve())
    assert real.read_bytes() == before
    subprocess.run([str(target), "--installer-pass-through"], check=True)
    installed = target.read_bytes()
    again = subprocess.run(command, capture_output=True)
    assert again.returncode and target.read_bytes() == installed and real.read_bytes() == before
print("RECOVERY PROXY INSTALL: PASS (native interpreter, pass-through, no engine overwrite)")
