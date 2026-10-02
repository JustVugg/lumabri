"""Malformed oracle files must fail before any checkpoint can be opened."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
valid = b'{"prompt_ids":[1],"full_ids":[1,2]}'
with tempfile.TemporaryDirectory(prefix="lmb-oracle-input-") as temporary:
    path = Path(temporary) / "oracle.json"
    for payload in (b"", valid[:-1], valid + b" trailing", valid + b"\x00hidden",
                    b'{"prompt_ids":[1],"full_ids":[1,2,]}'):
        path.write_bytes(payload)
        result = subprocess.run([str(binary), "qwen38", str(Path(temporary) / "no-model"), str(path)],
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == 1, (payload, result.returncode, result.stderr)
        assert "invalid oracle" in result.stderr, (payload, result.stderr)
print("EDGE ORACLE INPUT: PASS (malformed, truncated and embedded-NUL inputs refused)")
