#!/usr/bin/env python3
"""Known upstream architectures remain visible without granting execution."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='lmb-capabilities-') as tmp:
    root = Path(tmp)
    for name, kind in [('mi', 'mimo_v2'), ('decision', 'laya_modernbert'),
                       ('spans', 'gliner2_span_deberta-v2'), ('old', 'olmoe'),
                       ('dense', 'qwen3'), ('unknown', 'not_a_colibri_arch')]:
        d = root / name
        d.mkdir()
        (d / 'config.json').write_text(json.dumps(dict(model_type=kind,
            hidden_size=64, num_hidden_layers=2)))
        (d / 'model.safetensors').write_bytes(b'\0' * 4096)  # deliberately invalid tensor header
    image = root / 'image'
    image.mkdir()
    (image / 'model_index.json').write_text('{"_class_name":"QwenImage21Pipeline"}')
    (image / 'model.safetensors').write_bytes(b'\0' * 4096)
    g = root / 'linked-gguf'
    g.mkdir()
    (g / 'config.json').write_text('{"model_type":"olmoe","hidden_size":64,"num_hidden_layers":2}')
    (g / 'model.gguf').write_bytes(b'GGUF' + b'\0' * 4092)
    r = subprocess.run([str(ROOT / 'lumabri'), 'models', '--models-dir', str(root), '--json'],
                       text=True, capture_output=True, check=True)
    models = {m['name']: m for m in json.loads(r.stdout)['models']}
    assert set(models) == {'mi', 'decision', 'spans', 'old', 'dense', 'image', 'linked-gguf'}, models
    for name, m in models.items():
        c = m['capabilities']
        assert c['state'] == 'recognized' and not m['sizing_verified'], m
        assert c['verification'] == 'not_recorded' and c['execution_observation'] == 'none', m
        assert c['checkpoint_operations'] == 0 and c['execution_backends'] == [], m
        assert not m['planned'], m
        assert c['format'] == ('gguf' if name == 'linked-gguf' else 'safetensors'), m
    assert models['mi']['adapter'] == '' and models['mi']['capabilities']['family'] == 'mimo'
    assert models['image']['capabilities']['modality'] == 'image'
    assert models['spans']['capabilities']['modality'] == 'decision'
    assert not models['old']['capabilities']['upstream_operations'] & 2
    assert models['dense']['capabilities']['upstream_operations'] & 2
print('CAPABILITY CATALOGUE: PASS (all modalities visible; invalid formats cannot execute)')
