#!/usr/bin/env python3
"""Generate an upstream Qwen4-Exp fixture with a byte-sized chat vocabulary.

Only synthetic fixture dimensions change. Weights and oracle come from the
original Transformers model, never from Lumabri or an edited Colibri runtime.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out, engine = args.out.resolve(), args.engine.resolve()
    if out.exists():
        parser.error("--out must be new; existing checkpoints are never overwritten")
    spec = importlib.util.spec_from_file_location("upstream_q38_fixture", engine / "tools/make_qwen38_tiny.py")
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    torch = upstream.torch
    torch.set_num_threads(2)
    _, Model, Config = upstream._classes()
    with tempfile.TemporaryDirectory(prefix="lumabri-q38-template-") as tmp:
        upstream.build(Path(tmp), emit_ref=False)
        config = Config.from_pretrained(tmp)
    config.vocab_size = 256
    config.max_position_embeddings = 2048
    torch.manual_seed(upstream.SEED)
    model = Model(config).to(dtype=torch.bfloat16).eval()
    out.mkdir(parents=True)
    model.save_pretrained(str(out), safe_serialization=True)
    reference = upstream._reference(model, [1, 3, 4, 5, 6], 8)
    (out / "ref.json").write_text(json.dumps(reference, indent=2) + "\n")
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(out), "--vocab-size", "256"], check=True)
    print("Qwen3.8 synthetic checkpoint and independent oracle:", out)


if __name__ == "__main__":
    main()
