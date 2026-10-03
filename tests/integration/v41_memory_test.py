"""Checkpoint admission regressions; only private copies of the tiny fixture."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--fixture", type=Path, required=True)
    p.add_argument("--probe", type=Path, required=True)
    args = p.parse_args()
    probe = args.probe.resolve()
    if sum(f.stat().st_size for f in args.fixture.iterdir() if f.is_file()) > 16 * 1024**2:
        p.error("only a tiny fixture up to 16 MiB is permitted")
    def run(root, valid):
        r = subprocess.run([str(probe), str(root)], capture_output=True, text=True, timeout=30)
        assert (r.returncode == 0) == valid, (root.name, r.returncode, r.stdout, r.stderr)
    run(args.fixture, True)
    cases = {
        "missing-sidecar": lambda d: (d / "dsv41_engram.json").unlink(),
        "missing-tokenizer": lambda d: (d / "tokenizer.json").unlink(),
        "bad-source": ("config.json", lambda j: j["text_config"].update(kv_source_layers=[63])),
        "zero-ratio": ("config.json", lambda j: j["text_config"].update(compress_ratios=[0]*6)),
        "conflicting-alias": ("config.json", lambda j: j["text_config"].update(hidden_size=1024)),
        "oversized-saved-state": ("config.json", lambda j: j["text_config"].update(dspark_block_size=2147483647)),
        "too-many-tables": ("config.json", lambda j: j["text_config"].update(engram_layer_ids=[0,1,2,3,4])),
        "missing-hashes": ("dsv41_engram.json", lambda j: j.pop("primes")),
        "short-hashes": ("dsv41_engram.json", lambda j: j.update(primes=[])),
        "short-token-map": ("dsv41_engram.json", lambda j: j.update(token_map=[0])),
        "negative-token": ("dsv41_engram.json", lambda j: j["token_map"].__setitem__(0,-1)),
        "rounded-multiplier": ("dsv41_engram.json", lambda j: j["multipliers"][0].__setitem__(0,123.0)),
        "overflow-multiplier": ("dsv41_engram.json", lambda j: j["multipliers"][0].__setitem__(0,"9223372036854775807")),
        "row-out-of-bounds": ("dsv41_engram.json", lambda j: j["offsets"][0].__setitem__(0,1000000)),
        "wrong-engram-dimension": ("dsv41_engram.json", lambda j: j.update(head_dim=64)),
        "unexpected-table": ("dsv41_engram.json", lambda j: j.update(layer_ids=[1,3])),
    }
    with tempfile.TemporaryDirectory(prefix="lmb-v41-memory-") as tmp:
        for name, mutate in cases.items():
            root = Path(tmp) / name
            shutil.copytree(args.fixture, root)
            if callable(mutate):
                mutate(root)
            else:
                path = root / mutate[0]
                data = json.loads(path.read_text())
                mutate[1](data)
                path.write_text(json.dumps(data))
            run(root, False)
        for name in ("missing-tensor", "wrong-fp8-shape", "duplicate-expert"):
            root = Path(tmp) / name
            shutil.copytree(args.fixture, root)
            path = root / "model.safetensors"
            data = path.read_bytes()
            n, = struct.unpack_from("<Q", data)
            header = json.loads(data[8:8+n])
            if name == "missing-tensor":
                del header["layers.1.engram.embed.scale"]
            elif name == "wrong-fp8-shape":
                shape = header["layers.0.attn.wq_a.weight"]["shape"]
                shape[0], shape[1] = shape[1], shape[0]
            else:
                # The runtime cannot distinguish two same-named tensors in shards.
                extra = json.dumps({"layers.0.ffn.experts.0.w1.weight":
                    header["layers.0.ffn.experts.0.w1.weight"]}).encode()
                (root / "duplicate.safetensors").write_bytes(struct.pack("<Q",len(extra))+extra+data[8+n:])
            h = json.dumps(header).encode()
            path.write_bytes(struct.pack("<Q", len(h))+h+data[8+n:])
            run(root, False)
    print("V41 MEMORY PASS: valid resident range, context/session growth, 19 malformed checkpoints refused")


if __name__ == "__main__":
    main()
