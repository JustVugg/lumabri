#!/usr/bin/env python3
"""Header-only admission failures for each actual Qwen-family container."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def check_contract(model,probe):
    cfg=json.loads((model/'config.json').read_text())
    meta=json.loads((model/'qwen36_meta.json').read_text())
    original={};payload=0
    for file in sorted(model.glob('*.safetensors')):
        with file.open('rb') as f:
            n=struct.unpack('<Q',f.read(8))[0];header=json.loads(f.read(n))
        for name,item in header.items():
            if name=='__metadata__':continue
            item['data_offsets']=[x+payload for x in item['data_offsets']]
            assert name not in original
            original[name]=item
        payload+=file.stat().st_size-8-n
    layers=cfg['num_hidden_layers']
    with tempfile.TemporaryDirectory(prefix='lmb-qwen-headers-') as tmp:
        root=Path(tmp)
        (root/'config.json').write_text(json.dumps(cfg))

        def check(header,metadata,ok,label,context=128):
            (root/'qwen36_meta.json').write_text(json.dumps(metadata))
            raw=json.dumps(header).encode()
            with (root/'model.safetensors').open('wb') as f:
                f.write(struct.pack('<Q',len(raw)));f.write(raw);f.truncate(8+len(raw)+payload)
            r=subprocess.run([str(probe),str(root),'0',str(layers),str(context),'1'],capture_output=True,text=True)
            assert (r.returncode==0)==ok,(label,r.stdout,r.stderr)

        check(original,meta,True,'original inventory')
        required=['model.norm.weight','model.layers.0.input_layernorm.weight']
        for suffix in ('.mlp.down_proj.weight','.mlp.shared_expert.down_proj.weight',
                       '.mlp.experts.0.qs','.conv.in_proj.weight','.linear_attn.in_proj_qkv.weight',
                       '.self_attn.softmax_offset','.post_operator_norm.weight'):
            key=next((k for k in original if k.endswith(suffix)),None)
            if key:required.append(key)
        for key in required:
            h=copy.deepcopy(original);del h[key]
            check(h,meta,False,'missing '+key)
        wrong=copy.deepcopy(meta);wrong['layer_types'][0]='not_a_layer'
        check(original,wrong,False,'unknown layer kind')
        wrong=copy.deepcopy(meta);wrong['layer_types'].pop()
        check(original,wrong,False,'incomplete layer map')
        wrong=copy.deepcopy(meta);wrong['num_experts']+=1
        check(original,wrong,False,'expert count disagrees with config')
        if any(x=='linear_attention' for x in meta['layer_types']):
            wrong=copy.deepcopy(meta);wrong['dn_conv_dim']+=1
            check(original,wrong,False,'recurrence shape mismatch')
        if meta.get('mlp_only_layers'):
            wrong=copy.deepcopy(meta);wrong['mlp_only_layers'].append(layers)
            check(original,wrong,False,'dense layer outside range')
        if meta.get('lfm3'):
            wrong=copy.deepcopy(meta);wrong['lfm3']=False
            check(original,wrong,False,'LFM3 cannot silently use DeltaNet')
        check(original,meta,False,'declared context exceeded',max(262145,cfg.get('max_position_embeddings',0)+1))
    print(f'{model.name}: malformed headers and metadata rejected',flush=True)
