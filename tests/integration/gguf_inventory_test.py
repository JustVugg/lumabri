#!/usr/bin/env python3
"""The inventory only reads metadata; malformed container geometry is refused."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

PROBE = sys.argv[1] if len(sys.argv) > 1 else './build/test_gguf_inventory'

def string(v):
    v = v.encode() if isinstance(v, str) else v
    return struct.pack('<Q',len(v)) + v

def scalar(k,t,v):
    return string(k)+struct.pack('<I',t)+v

def container(*,duplicate=False,offset=0,kind=8,rank=2,columns=32,rows=4,alignment=32,name="token_embd.weight"):
    kv=[scalar('general.architecture',8,string('olmoe')),
        scalar('general.alignment',4,struct.pack('<I',alignment)),
        scalar('tokenizer.ggml.tokens',9,struct.pack('<IQ',8,3)+string('a')+string('é')+string('')),
        scalar('tokenizer.ggml.token_type',9,struct.pack('<IQIII',4,3,1,1,3))]
    if duplicate:kv.append(kv[0])
    ti=string(name)+struct.pack('<I',rank)+struct.pack('<QQ',columns,rows)+struct.pack('<IQ',kind,offset)
    head=b'GGUF'+struct.pack('<IQQ',3,1,len(kv))+b''.join(kv)+ti
    data=(len(head)+31)//32*32
    return head+b'\0'*(data-len(head))+b'\xff'*136,len(head),data

with tempfile.TemporaryDirectory(prefix='lmb-gguf-read-') as tmp:
    path=Path(tmp)/'model.gguf'
    def run(data,valid=False):
        path.write_bytes(data)
        p=subprocess.run([PROBE,str(path)],capture_output=True,text=True)
        assert p.returncode==(0 if valid else 1),(p.returncode,p.stderr,p.stdout)
        return p.stdout
    good,header,data=container()
    out=run(good,True).splitlines()
    assert out==[f'olmoe 4 1 {data}',f'token_embd.weight 8 {data} 136'],out
    # Every truncated header is tested; no short read can become metadata.
    for end in range(header):run(good[:end])
    for opts in [dict(duplicate=True),dict(offset=1),dict(offset=2**64-32),dict(kind=65),
                 dict(rank=0),dict(rank=5),dict(columns=31),dict(rows=2**63),
                 dict(alignment=0),dict(alignment=3),dict(alignment=2**21)]:
        run(container(**opts)[0])
    for pos in (8,16):
        bad=bytearray(good);struct.pack_into('<Q',bad,pos,2**64-1);run(bad)
    run(good[:-1])
    # Sparse huge payload doesn't increase metadata memory or reading cost.
    path.write_bytes(good)
    with path.open('r+b') as f:f.truncate(1<<34)
    p=subprocess.run([PROBE,str(path)],capture_output=True,text=True,check=True)
    assert p.stdout.splitlines()==out
    # Split containers retain exact per-file offsets; traversal, missing parts,
    # duplicates and architecture disagreement never become an inventory.
    first=Path(tmp)/'split-00001-of-00002.gguf'
    second=Path(tmp)/'split-00002-of-00002.gguf'
    first.write_bytes(container()[0]);second.write_bytes(container(name='output.weight')[0])
    sidecar=Path(tmp)/'gguf.json'
    def model(ref,valid=False):
        sidecar.write_text(json.dumps({'format':'colibri-gguf-v1','gguf':ref}))
        p=subprocess.run([PROBE,'--model',tmp],capture_output=True,text=True)
        assert p.returncode==(0 if valid else 1),(p.returncode,p.stderr,p.stdout)
        return p.stdout
    assert 'olmoe 4 2 ' in model(first.name,True)
    model(str(first))
    model('../'+first.name)
    model('./'+first.name)
    model(second.name)
    second.write_bytes(container()[0]);model(first.name) # duplicate tensor
    second.write_bytes(container(name='output.weight')[0].replace(b'olmoe',b'other'));model(first.name)
    second.unlink();model(first.name)
print(f'GGUF INVENTORY: PASS ({header} truncated headers, overflow/duplicate/type/shape controls, 16 GiB sparse file)')
