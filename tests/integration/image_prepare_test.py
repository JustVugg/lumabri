"""Compare C PNG/JPEG preprocessing with pinned Colibri's Python reference.
No model download is needed. All images are deterministic test inputs.
"""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

binary, reference = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("qwen_image_reference", reference)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory(prefix="lmb-images-") as directory:
    root = Path(directory)
    for width, height in [(256, 256), (64, 64), (100, 50), (50, 100), (37, 91), (640, 480), (1920, 1080), (33, 17)]:
        y, x = np.indices((height, width))
        pixels = np.stack([(x * 3 + y) % 256, (y * 5 + x) % 256, (x + y * 2) % 256], axis=-1).astype(np.uint8)
        for fmt in ["png", "jpeg"]:
            path = root / f"{width}-{height}.{fmt}"
            Image.fromarray(pixels).save(path)
            raw = root / "patches.f32"
            meta = json.loads(subprocess.check_output([str(binary.resolve()), str(path), fmt, str(raw)], text=True))
            expected, gh, gw = module.preprocess(str(path), max_tokens=256)
            actual = np.fromfile(raw, dtype=np.float32).reshape(expected.shape)
            assert (meta["grid_h"], meta["grid_w"]) == (gh, gw), meta
            assert meta["tokens"] == gh * gw // 4 and meta["values"] == actual.size
            delta = np.abs(actual - expected)
            worst, mean = float(delta.max()), float(delta.mean())
            print(json.dumps({"size": [width, height], "format": fmt, "max_normalized_error": worst, "mean_normalized_error": mean}))
            if fmt == "png" and (width, height) == (256, 256):
                assert worst == 0, "non-resized PNG patch order/normalization changed"
            else:
                assert worst <= .08 and mean <= .004, "decoder/resize difference exceeds the documented tolerance"
print("IMAGE PREPROCESS PASS: exact geometry and non-resized PNG patches; bounded RGB decoder/resize differences")
