#!/usr/bin/env python3
"""Build an isolated synthetic Inkling fixture with the pinned engine's tag.

Transformers exports `inkling_text`; the pinned Colibri registry accepts
`inkling`/`inkling_mm_model`. Only this synthetic fixture is retagged. Runtime
model identification remains exact and does not guess aliases for user models.
No Colibri source or user checkpoint is modified.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    engine = args.engine.resolve()
    out = args.out.resolve()
    if out.exists():
        parser.error("--out must be a new directory; existing fixtures are never overwritten")
    out.mkdir(parents=True)
    env = dict(os.environ, OMP_NUM_THREADS="2", MKL_NUM_THREADS="2")
    source = out / "float" / "inkling-tiny"
    subprocess.run([sys.executable, str(engine / "tools/make_tiny_inkling.py"),
                    str(source)], check=True, env=env)
    config_path = source / "config.json"
    config = json.loads(config_path.read_text())
    if config["model_type"] != "inkling_text":
        raise RuntimeError("upstream fixture changed; review its registry mapping")
    config["model_type"] = "inkling"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(source), "--vocab-size", "256"], check=True, env=env)
    subprocess.run([sys.executable, str(engine / "tools/convert_inkling_int4.py"),
                    "--indir", str(source), "--outdir", str(out / "int4" / "inkling-tiny"),
                    "--xbits", "4"], check=True, env=env)


if __name__ == "__main__":
    main()
