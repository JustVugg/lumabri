#!/usr/bin/env python3
"""Compare the C importer with the pinned upstream Python importer as an oracle."""
import json
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
from gguf_olmoe_fixture import create, string

engine=Path(sys.argv[1]).resolve()
probe=sys.argv[2] if len(sys.argv)>2 else './build/test_gguf_inventory'
up=runpy.run_path(str(engine/'tools/gguf_import.py'))
with tempfile.TemporaryDirectory(prefix='lmb-gguf-metadata-') as tmp:
    root=Path(tmp)
    def run(extra,ok=True):
        create(root,extra_metadata=extra)
        p=subprocess.run([probe,'--metadata',str(root)],capture_output=True,text=True)
        assert p.returncode==(0 if ok else 1),(p.returncode,p.stdout,p.stderr)
        return json.loads(p.stdout) if ok else None
    for pre in ['',*up['PRE_SPLITS']]:
        specials=['α','bé','\n','<|im_end|>',*[chr(65+i) for i in range(4,30)],'<bos>','<eos>']
        extra=[('tokenizer.ggml.pre',8,string(pre)),
               ('tokenizer.ggml.tokens',9,struct.pack('<IQ',8,len(specials))+b''.join(map(string,specials))),
               ('tokenizer.ggml.token_type',9,struct.pack('<IQ',4,32)+struct.pack('<32I',*([1]*30+[3,4]))),
               ('tokenizer.ggml.bos_token_id',4,struct.pack('<I',30)),
               ('tokenizer.ggml.eos_token_id',4,struct.pack('<I',31)),
               ('tokenizer.ggml.add_bos_token',7,b'\1'),
               ('tokenizer.chat_template',8,string('{{ messages[0]["content"] }}\n'))]
        actual=run(extra)
        reader=up['GGUFReader'](str(root/'weights.gguf'),header_only=True)
        cfg,_=up['olmoe_config'](reader.metadata,{t['name']:t for t in reader.tensors})
        tok,tcfg,gen=up['tokenizer_files'](reader.metadata)
        gen['eos_token_id'].sort()
        assert actual==dict(config=cfg,tokenizer=tok,tokenizer_config=tcfg,generation_config=gen), (pre,actual)
    for extra in [
        [('tokenizer.ggml.model',8,string('llama'))],
        [('tokenizer.ggml.tokens',9,struct.pack('<IQ',8,2)+string('x')+string('x'))],
        [('tokenizer.ggml.bos_token_id',4,struct.pack('<I',32))],
        [('tokenizer.ggml.add_bos_token',7,b'\2')],
        [('olmoe.rope.freq_base',6,struct.pack('<f',float('nan')))],
        [('tokenizer.ggml.eog_token_ids',9,struct.pack('<IQI',4,1,32))],
    ]:run(extra,False)
print('GGUF METADATA: PASS (all upstream pre-tokenizers, special tokens, config parity, malformed metadata refused)')
