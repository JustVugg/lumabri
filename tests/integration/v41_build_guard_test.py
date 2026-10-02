#!/usr/bin/env python3
"""Build-copy hooks fail closed on source drift and may not edit upstream."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[2] / "tools/prepare_v41_range.py"
    spec = importlib.util.spec_from_file_location("v41_hooks", script)
    hooks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hooks)
    original = args.source.read_bytes()
    result = hooks.prepare(original.decode())
    assert "resident Engram weights" in result
    assert "boundary_input" in result
    try:
        hooks.prepare(original.decode() + "\n/* new source */\n")
    except ValueError:
        pass
    else:
        raise AssertionError("source drift was silently accepted")
    with tempfile.TemporaryDirectory(prefix="lmb-v41-build-guard-") as tmp:
        root = Path(tmp)
        upstream = root / "upstream"
        upstream.mkdir()
        source = upstream / "deepseek_v41.c"
        source.write_bytes(original)
        forbidden = [source, upstream / "new.c"]
        link = root / "aliased-output.c"
        link.symlink_to(source)
        forbidden.append(link)
        for output in forbidden:
            p = subprocess.run(["python3", str(script), "--source", str(source),
                                "--output", str(output)], capture_output=True, text=True)
            assert p.returncode != 0, (output, p.stdout, p.stderr)
            assert source.read_bytes() == original
        output = root / "build/core.c"
        subprocess.run(["python3", str(script), "--source", str(source),
                        "--output", str(output)], check=True)
        assert output.read_text() == result
        assert source.read_bytes() == original
    assert args.source.read_bytes() == original
    print("V41 build guard: PASS (source pin, immutable upstream, symlink refusal)")


if __name__ == "__main__":
    main()
