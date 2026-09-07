#!/usr/bin/env python3
"""Generate a native GLM5.3 fixture and normalize its oracle field names only."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def normalize_reference(model):
    original = json.loads((model / "ref.json").read_text())
    # Token values are produced by upstream Transformers, never by Lumabri.
    oracle = {"prompt_ids": original["prompt_ids"],
              "full_ids": original["prompt_ids"] + original["greedy_new_ids"]}
    (model / "edge_ref.json").write_text(json.dumps(oracle, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    engine, out = args.engine.resolve(), args.out.resolve()
    if out.exists():
        parser.error("--out must be new; existing checkpoints are never overwritten")
    subprocess.run([sys.executable, str(engine / "tools/make_glm53_tiny.py"),
                    "--output", str(out)], check=True,
                   env=dict(os.environ, OMP_NUM_THREADS="2", MKL_NUM_THREADS="2"))
    normalize_reference(out)
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(out), "--vocab-size", "128"], check=True)


if __name__ == "__main__":
    main()
