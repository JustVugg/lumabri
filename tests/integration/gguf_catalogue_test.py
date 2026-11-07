#!/usr/bin/env python3
"""Raw single/split GGUF reaches the actual catalog without a Python importer."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from gguf_olmoe_fixture import create

binary=Path(sys.argv[1] if len(sys.argv)>1 else './lumabri').resolve()
with tempfile.TemporaryDirectory(prefix='lmb-gguf-catalogue-') as tmp:
    tmp=Path(tmp); home=tmp/'home';home.mkdir()
    env={**os.environ,'HOME':str(home),'LUMABRI_NO_DISK_PROBE':'1','LUMABRI_RAM_RESERVE_MB':'256'}
    def catalogue(path,cwd=None):
        p=subprocess.run([str(binary),'models','--models-dir',str(path),'--context','128','--json'],
                         cwd=cwd,env=env,capture_output=True,text=True,check=True)
        return json.loads(p.stdout)['models']
    for split in (False,True):
        fixture=create(tmp/('fixture-split' if split else 'fixture-single'),mixed=True,byte_vocab=True,split=split)
        raw=tmp/('raw-split' if split else 'raw-single');raw.mkdir()
        for part in fixture.glob('*.gguf'):shutil.copy2(part,raw/part.name)
        first=raw/('weights-00001-of-00002.gguf' if split else 'weights.gguf')
        originals={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in raw.glob('*.gguf')}
        for root,cwd in ((raw,None),(first,None),(first.name,raw)):
            models=catalogue(root,cwd)
            assert len(models)==1,models
            m=models[0]
            assert m['name']==first.name and m['sizing_verified'] and m['checkpoint_inventory_ok'],m
            assert m['capabilities']['format']=='gguf' and m['capabilities']['state']=='executable',m
        wrappers=list(raw.glob('.lumabri-gguf-*'));assert len(wrappers)==1,wrappers
        wrapper=wrappers[0]
        for part in raw.glob('*.gguf'):
            assert (wrapper/part.name).stat().st_ino==part.stat().st_ino, 'weight copy instead of link'
            assert hashlib.sha256(part.read_bytes()).hexdigest()==originals[part.name]
        assert set(p.name for p in wrapper.iterdir())==set(originals)|{
            'config.json','tokenizer.json','tokenizer_config.json','generation_config.json','gguf.json'}
        # Broken local metadata must never become an admitted checkpoint.
        cfg=wrapper/'config.json';saved=cfg.read_bytes();cfg.write_text('{}')
        models=catalogue(first)
        assert len(models)==1 and not models[0]['planned'] and 'import unavailable' in models[0]['capabilities']['reason'],models
        cfg.write_bytes(saved)
        assert catalogue(first)[0]['sizing_verified']
        if split:
            second=raw/'weights-00002-of-00002.gguf';hold=raw/'held.part';second.rename(hold)
            assert not catalogue(first)[0]['planned'], 'missing part accepted from an old cache'
            hold.rename(second)
        # An abandoned private import cannot poison the next valid import.
        before=list(raw.glob('.lumabri-gguf-*'))
        orphan=raw/'.lumabri-gguf-interrupted-123.tmp';orphan.mkdir();(orphan/'config.json').write_text('{}')
        st=first.stat();os.utime(first,ns=(st.st_atime_ns,st.st_mtime_ns+1000000))
        assert catalogue(first)[0]['sizing_verified']
        assert len(list(raw.glob('.lumabri-gguf-*')))==len(before)+2
    # User-owned symlink sources (e.g. local model snapshots) are resolved into
    # hard links; the resulting signed checkpoint contains no escaping paths.
    links=tmp/'links';links.mkdir();(links/'weights.gguf').symlink_to(tmp/'raw-single/weights.gguf')
    assert catalogue(links)[0]['sizing_verified']
    wrapper=next(links.glob('.lumabri-gguf-*'))
    assert not (wrapper/'weights.gguf').is_symlink()
print('GGUF CATALOGUE: PASS (raw files, splits, relative path, source identity, incomplete/corrupt import refusal)')
