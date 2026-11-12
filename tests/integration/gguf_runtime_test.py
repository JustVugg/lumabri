#!/usr/bin/env python3
"""Monolithic upstream versus resident Edge + one/two/fresh Segment sessions."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from gguf_olmoe_fixture import create

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--upstream',type=Path,required=True)
p.add_argument('--segment',type=Path,required=True)
a=p.parse_args()
env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off',
     'LUMABRI_RESIDENT_REQUIRED':'1','LUMABRI_ENGINE_BACKEND':'cpu'}
with tempfile.TemporaryDirectory(prefix='lmb-gguf-runtime-') as tmp:
    tmp=Path(tmp)
    for mixed in (False,True):
        expected=None
        for split in (False,True):
            root=create(tmp/f'mixed-{mixed}-split-{split}',mixed=mixed,split=split,inter=64 if mixed else 32)
            oracle=root/'oracle.json'
            subprocess.run([str(a.upstream.resolve()),str(root),str(oracle)],env=env,check=True,timeout=60)
            tokens=json.loads(oracle.read_text())
            if expected is not None:assert tokens==expected, 'splitting a file changed the local oracle'
            expected=tokens
            subprocess.run([str(a.segment.resolve()),'olmoe',str(root),str(oracle)],env=env,check=True,timeout=60)
print('GGUF RUNTIME: PASS (Q8/mixed F16, single/split files, independent greedy oracle, fresh sessions)')
