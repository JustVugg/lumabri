#!/usr/bin/env python3
"""Run the pinned upstream GLM oracle generator in a new isolated directory."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    engine, out = args.engine.resolve(), args.out.resolve()
    if out.exists():
        parser.error("--out must be new; existing checkpoints are never overwritten")
    out.mkdir(parents=True)
    env = dict(os.environ, OMP_NUM_THREADS="2", MKL_NUM_THREADS="2")
    subprocess.run([sys.executable, str(engine / "tools/make_glm_oracle.py")],
                   cwd=out, env=env, check=True)
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(out / "glm_tiny"), "--vocab-size", "256"], env=env, check=True)


if __name__ == "__main__":
    main()
