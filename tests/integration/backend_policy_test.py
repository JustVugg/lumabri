"""A rejected execution policy must fail before identity lookup/model open."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
for binary in ("segment_node", "segment_chat"):
    for policy in ("cuda", "metal", "hip", "vulkan", "CPU", "", "cpu,cuda"):
        result = subprocess.run([str(root / binary)], capture_output=True, text=True,
                                env={**os.environ, "LUMABRI_ENGINE_BACKEND": policy}, timeout=5)
        assert result.returncode == 2, (binary, policy, result)
        assert "unsupported execution backend policy" in result.stderr, (binary, policy, result)
        assert "cannot open" not in result.stderr, (binary, policy, result)
    for policy in ("cpu", "auto"):
        result = subprocess.run([str(root / binary)], capture_output=True, text=True,
                                env={**os.environ, "LUMABRI_ENGINE_BACKEND": policy}, timeout=5)
        assert result.returncode == 2, (binary, policy, result)
        assert "usage:" in result.stderr.lower(), (binary, policy, result)
        assert "unsupported execution backend policy" not in result.stderr, (binary, policy, result)
print("BACKEND POLICY: PASS (unsupported policies rejected before model or network work)")
