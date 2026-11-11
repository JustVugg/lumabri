#!/usr/bin/env python3
"""Native GGUF: real upstream fixtures, imported metadata, resident ABI oracle."""
import argparse
import importlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def fixture_contents(path):
    from gguf_reader import GGUFReader
    def value(v):
        if isinstance(v,bool):return (7,v)
        if isinstance(v,int):return (4,v)
        if isinstance(v,float):return (6,v)
        if isinstance(v,str):return (8,v)
        assert isinstance(v,list)
        return (9,(value(v[0])[0] if v else 8,v))
    with GGUFReader(str(path)) as reader:
        kv=[(k,*value(v)) for k,v in reader.metadata.items()]
        tensors=[]
        for t in reader.tensors:
            reader.f.seek(t['abs_offset'])
            tensors.append((t['name'],t['ggml_type'],t['dims'],reader.f.read(t['nbytes'])))
    return kv,tensors


def byte_tokens(kv):
    # Fixture-only replacement of synthetic <0xNN> strings with real byte
    # tokens, so the household test exercises text tokenization as well.
    bs=list(range(33,127))+list(range(161,173))+list(range(174,256))
    table=dict(zip(bs,bs));extra=256
    for b in range(256):
        if b not in table:table[b]=extra;extra+=1
    vocab=[chr(table[b]) for b in range(256)]+[f'<fixture_{i}>' for i in range(256,320)]
    return [(k,9,(8,vocab)) if k=='tokenizer.ggml.tokens' else (k,t,v) for k,t,v in kv]


def contract(root,kv,tensors,env):
    from make_flint_tiny import _gguf_bytes
    with tempfile.TemporaryDirectory(prefix='lmb-qwen-gguf-contract-') as tmp:
        tmp=Path(tmp)
        (tmp/'config.json').write_bytes((root/'config.json').read_bytes())
        (tmp/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        def check(ts,metadata=kv):
            (tmp/'model.gguf').write_bytes(_gguf_bytes(metadata,ts))
            p=subprocess.run(['build/test_gguf_inventory','--cost',str(tmp)],env=env,capture_output=True)
            assert p.returncode!=0,'invalid GGUF was admitted'
        for name in ('output_norm.weight','blk.0.attn_norm.weight'):
            check([t for t in tensors if t[0]!=name])
        # Same element count, wrong matrix orientation is not a valid layer.
        key=next(t[0] for t in tensors if t[0].endswith('attn_q.weight'))
        check([(n,t,[d[1],d[0]],v) if n==key else (n,t,d,v) for n,t,d,v in tensors])
        check(tensors,[(k,t,999) if k.endswith('.block_count') else (k,t,v) for k,t,v in kv])
        check(tensors+[('blk.0.unknown.weight',0,[1],b'\0'*4)])
        check(tensors+[('blk.0.unknown.scale',0,[1],b'\0'*4)])
        check([t for t in tensors if t[0]!='output.scale']+[('output.scale',0,[2],b'\0'*8)])
        if any(k=='general.architecture' and v=='muse-spark' for k,_,v in kv):
            for suffix in ('ffn_pre_exp_norm.weight','ffn_latent_down.weight','attn_resid_beta.weight','attn_q_norm.weight'):
                check([t for t in tensors if t[0]!='blk.0.'+suffix])
            for key,value in [('moe_latent_size',64),('expert_gating_func',0),('logit_scale',0.),
                              ('expert_weights_scale',.5),('post_norm_epsilon',-1.)]:
                check(tensors,[(k,t,value) if k=='muse-spark.'+key else (k,t,v) for k,t,v in kv])


def run(argv, env, timeout=180):
    p=subprocess.run(list(map(str,argv)),env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                     text=True,timeout=timeout)
    if p.returncode:raise RuntimeError(f'{argv}: exit {p.returncode}\n{p.stdout[-16000:]}')
    return p.stdout


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',type=Path,required=True)
    families=('qwen3','qwen3moe','qwen35','qwen35moe','flint','lfm3','muse-spark')
    p.add_argument('--only',choices=families)
    p.add_argument('--household',action='store_true')
    p.add_argument('--packed',action='store_true',help='also validate upstream expert-packed-v1 containers')
    args=p.parse_args();engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    dest=Path('build/qwen-native-gguf').resolve();dest.mkdir(parents=True,exist_ok=True)
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','COLI_DENSE_I8':'0',
         'LUMABRI_RESIDENT_REQUIRED':'1','LUMABRI_ENGINE_BACKEND':'cpu','PILOT':'0'}
    for name in families:
        if args.only and name!=args.only:continue
        if name in ('flint','lfm3','muse-spark'):
            module=importlib.import_module('make_'+('muse' if name=='muse-spark' else name)+'_tiny')
            config=module.tiny_config(vocab=320);weights=module.random_weights(config,20261029)
        for kind in ('F32','Q8_0'):
            root=dest/(name+'-'+kind);root.mkdir(exist_ok=True)
            path=root/'model.gguf'
            if name in ('flint','muse-spark'):module.write_gguf(path,config,weights,expert_type='Q8_0',dense_type=kind)
            elif name=='lfm3':module.write_gguf(path,config,weights,dense_type=kind)
            else:
                from gguf_qwen_fixture import create
                create(engine,root,name,kind=='Q8_0')
            from make_flint_tiny import _gguf_bytes
            kv,tensors=fixture_contents(path)
            if name in ('flint','lfm3','muse-spark'):kv=byte_tokens(kv)
            if kind=='Q8_0':
                scales=[]
                for field,_,dims,_ in tensors:
                    if field.endswith('.weight') and field!='rope_freqs.weight':
                        count=dims[2] if len(dims)==3 else 1
                        values=[.8+.4*i/max(1,count) for i in range(count)]
                        scales.append((field[:-7]+'.scale',0,[count],struct.pack('<'+'f'*count,*values)))
                tensors+=scales
            path.write_bytes(_gguf_bytes(kv,tensors))
            (root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':path.name}))
            metadata=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
            for key,value in metadata.items():(root/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',root],env)
            contract(root,kv,tensors,env)
            run(['build/qwen-upstream-oracle',root,root/'oracle.json'],env)
            print(run(['build/gguf-segment-oracle','qwen36',root,root/'oracle.json'],env),end='',flush=True)
            layers=metadata['config']['num_hidden_layers']
            run(['build/qwen-state-oracle','qwen36',root,0,layers//2,layers,128],env)
            print(f'{name} {kind}: native GGUF metadata, memory, local/split/fresh and zero inference reads PASS',flush=True)
            if kind=='Q8_0':
                if args.packed and name in ('qwen3moe','qwen35moe','flint','muse-spark'):
                    packed=root/'packed'
                    run([sys.executable,engine/'tools/gguf_import.py',path,packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
                    meta=json.loads(run(['build/test_gguf_inventory','--metadata',packed],env))
                    for key,value in meta.items():(packed/(key+'.json')).write_text(json.dumps(value))
                    run(['build/test_gguf_inventory','--cost',packed],env)
                    print(run(['build/gguf-segment-oracle','qwen36',packed,root/'oracle.json'],env),end='',flush=True)
                    print(f'{name}: expert-packed-v1 matches the original GGUF PASS',flush=True)
                split=root/'split';split.mkdir(exist_ok=True)
                for i in range(2):
                    meta=kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(tensors))]
                    (split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(meta,tensors[i::2]))
                (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
                metadata=json.loads(run(['build/test_gguf_inventory','--metadata',split],env))
                for key,value in metadata.items():(split/(key+'.json')).write_text(json.dumps(value))
                run(['build/test_gguf_inventory','--cost',split],env)
                print(run(['build/gguf-segment-oracle','qwen36',split,root/'oracle.json'],env),end='',flush=True)
                print(f'{name}: split GGUF preserves native oracle PASS',flush=True)
                if args.household:
                    raw=root/'raw';raw.mkdir(exist_ok=True)
                    target=raw/'model.gguf'
                    if not target.exists():os.link(path,target)
                    context='2048' if name=='muse-spark' else '512'
                    catalogue=json.loads(run(['./lumabri','models','--models-dir',raw,'--context',context,'--json'],
                        {**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                    assert len(catalogue)==1 and catalogue[0]['sizing_verified'] and catalogue[0]['capabilities']['format']=='gguf',catalogue
                    wrapper=next(raw.glob('.lumabri-gguf-*'));models=root/'home';models.mkdir(exist_ok=True)
                    link=models/name
                    if not link.exists():link.symlink_to(wrapper,target_is_directory=True)
                    print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',models,
                        '--resident-default','--crash-requester','--context',context],env,600),end='',flush=True)
    if not args.only:
        from qwen_architectures_test import tokenizer
        converted=dest/'converted-flint'
        if not (converted/'qwen36_meta.json').exists():
            source=dest/'converted-flint-source'
            run([sys.executable,engine/'tools/make_flint_tiny.py','--out',source,'--vocab','320'],env)
            run([sys.executable,engine/'tools/convert_qwen36.py','--model',source,'--out',converted,
                '--ebits','4','--gs','16','--no-readme'],env)
        tokenizer(converted)
        run(['build/qwen-upstream-oracle',converted,converted/'oracle.json'],env)
        models=[dest/'qwen3-Q8_0',dest/'qwen35-F32',converted]
        cmd=['build/qwen-mixed-oracle']
        for model in models:cmd.extend([model,model/'oracle.json'])
        print(run(cmd,env),end='',flush=True)
        muse=dest/'muse-spark-Q8_0'
        # Muse changes row permutation, latent expert width and output norms.
        # Keep it live beside a different native family and an int4 container.
        models=[muse,dest/'qwen35-F32',converted]
        cmd=['build/qwen-mixed-oracle']
        for model in models:cmd.extend([model,model/'oracle.json'])
        print(run(cmd,env),end='',flush=True)


if __name__=='__main__':main()
