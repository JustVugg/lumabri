#!/usr/bin/env python3
"""Check per-range slot padding independently of the implementation's sums."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from gguf_olmoe_fixture import create

probe = sys.argv[1] if len(sys.argv)>1 else './build/test_gguf_inventory'
with tempfile.TemporaryDirectory(prefix='lmb-gguf-cost-') as tmp:
    root=create(tmp, mixed=True)
    def costs(ok=True):
        p=subprocess.run([probe,'--cost',str(root)],capture_output=True,text=True)
        assert p.returncode==(0 if ok else 1),(p.returncode,p.stdout,p.stderr)
        return {tuple(map(int,line.split()[:2])):list(map(int,line.split()[2:])) for line in p.stdout.splitlines()}
    actual=costs()
    # Common index/bookkeeping stays once per process, not once per layer.
    index=(2*(9+3*2)+3)*512
    fixed=index+2*(8192+2*512)
    dense=4*32*4+4*32*34+2*34
    small_slot=3*32*34
    large_slot=3*32*32*2
    assert actual[(0,1)][0]==fixed+dense+2*small_slot,actual
    assert actual[(1,2)][0]==fixed+dense+2*large_slot,actual
    assert actual[(0,2)][0]==fixed+2*dense+4*large_slot,actual
    assert actual[(0,2)][1]==2*32*8*128,actual
    assert actual[(0,2)][2]>0
    # Same-size but incompatible metadata cannot authorize an executable plan.
    cfgpath=root/'config.json';cfg=json.loads(cfgpath.read_text())
    for key in ('hidden_size','num_hidden_layers','num_experts','intermediate_size','vocab_size'):
        changed=dict(cfg);changed[key]+=1;cfgpath.write_text(json.dumps(changed));costs(False)
    cfgpath.write_text(json.dumps(cfg))
    payload=(root/'weights.gguf').read_bytes()
    (root/'weights.gguf').write_bytes(payload.replace(b'blk.1.attn_q.weight',b'blk.1.wrong_.weight'))
    costs(False)
    (root/'weights.gguf').write_bytes(payload)
    cfgpath.write_text(json.dumps(cfg));costs()
print('GGUF MEMORY: PASS (native blocks, mixed-layer slot padding, metadata/shape/name refusal)')
