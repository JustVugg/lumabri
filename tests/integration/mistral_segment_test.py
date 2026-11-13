#!/usr/bin/env python3
"""Mistral native GGUF: monolithic oracle, every layer cut and real home flow."""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from qwen_native_gguf_test import run, fixture_contents, byte_tokens


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--engine',required=True,type=Path)
    p.add_argument('--household',action='store_true')
    args=p.parse_args();engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    import make_mistral_large4_tiny as module
    from make_flint_tiny import _gguf_bytes,_q8_0
    import torch
    torch.set_num_threads(1)
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','COLI_DENSE_I8':'0',
         'LUMABRI_RESIDENT_REQUIRED':'1','LUMABRI_ENGINE_BACKEND':'cpu','PILOT':'0',
         'GLM_SEGMENT_EBITS':'16','GLM_SEGMENT_DBITS':'16'}
    dest=Path('build/mistral-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    config=module.tiny_config();config['vocab']=320
    weights=module.random_weights(config,20261030)
    for kind in ('F32','Q8_0'):
        root=dest/kind;root.mkdir(exist_ok=True)
        path=root/'model.gguf';blocks={}
        if kind=='Q8_0':
            for name,t in weights.items():
                # Norms/router/indexer projections stay float, like the release.
                if t.ndim>=2 and t.shape[-1]%32==0 and not any(x in name for x in ('gate_inp','indexer.proj')):
                    blocks[name]=(8,_q8_0(t.float().contiguous().numpy()))
        module.write_gguf(path,config,weights,blocks)
        kv,tensors=fixture_contents(path);kv=byte_tokens(kv)
        path.write_bytes(_gguf_bytes(kv,tensors))
        (root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        metadata=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in metadata.items():(root/(key+'.json')).write_text(json.dumps(value))
        costs=run(['build/test_gguf_inventory','--cost',root],env)
        assert len(costs.splitlines())==36
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        run(['build/glm-upstream-oracle',root,root/'oracle.json'],env)
        for cut in range(1,8):
            run(['build/gguf-segment-oracle','glm',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
        print(f'Mistral {kind}: every cut 1..7, full/split/fresh, 32 tokens vs upstream and zero inference reads PASS',flush=True)
        print(run(['build/mistral-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-mistral-contract-') as tmp:
            tmp=Path(tmp);(tmp/'config.json').write_text(json.dumps(metadata['config']))
            (tmp/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
            import subprocess
            def denied(ts,meta=kv):
                (tmp/'model.gguf').write_bytes(_gguf_bytes(meta,ts))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'invalid Mistral checkpoint was admitted'
            for name in ('rope_freqs.weight','blk.0.attn_ssmax.bias','blk.2.indexer.q_norm.weight','blk.7.attn_v_b.weight'):
                if name=='rope_freqs.weight':continue  # optional learned correction
                denied([t for t in tensors if t[0]!=name])
            denied(tensors+[('blk.0.unknown.weight',0,[1],struct.pack('<f',1))])
            denied([(n,t,[d[1],d[0]],v) if n=='blk.0.attn_q.weight' else (n,t,d,v) for n,t,d,v in tensors])
            for key,value in [('expert_gating_func',0),('attention.kv_source_map',[7]*8),('attention.indexer.types',[False]*8)]:
                denied(tensors,[(k,t,(v[0],value) if t==9 else value) if k=='mistral-large4.'+key else (k,t,v) for k,t,v in kv])
            (tmp/'model.gguf').write_bytes(path.read_bytes())
            for key in ('kv_lora_rank','index_topk','hidden_size'):
                bad={**metadata['config'],key:metadata['config'][key]+1}
                (tmp/'config.json').write_text(json.dumps(bad))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'incompatible config/GGUF accepted'
        print(f'Mistral {kind}: missing/mis-shaped tensors, invalid controls and sidecar mismatch rejected PASS',flush=True)
        if kind=='Q8_0':
            packed=root/'packed'
            run([sys.executable,engine/'tools/gguf_import.py',path,packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            meta=json.loads(run(['build/test_gguf_inventory','--metadata',packed],env))
            for key,value in meta.items():(packed/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/gguf-segment-oracle','glm',packed,root/'oracle.json'],env),end='',flush=True)
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):
                meta=kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(tensors))]
                (split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(meta,tensors[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            meta=json.loads(run(['build/test_gguf_inventory','--metadata',split],env))
            for key,value in meta.items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','glm',split,root/'oracle.json'],env),end='',flush=True)
            print('Mistral: packed/split GGUF preserve native oracle PASS',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True)
                target=raw/'model.gguf'
                if not target.exists():os.link(path,target)
                catalogue=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],
                    {**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalogue)==1 and catalogue[0]['sizing_verified'],catalogue
                wrapper=next(raw.glob('.lumabri-gguf-*'));models=root/'home';models.mkdir(exist_ok=True)
                link=models/'mistral'
                if not link.exists():link.symlink_to(wrapper,target_is_directory=True)
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',models,
                    '--resident-default','--crash-requester','--context','512'],env,600),end='',flush=True)

    # Same hidden width/theta, different learned RoPE frequencies and larger
    # layer/expert counts: no first-model-sized global state may be reused.
    different=dest/'different-rope';different.mkdir(exist_ok=True)
    larger={**config,'experts':16,'rope_pattern':config['rope_pattern']+[True],
        'kv_source':config['kv_source']+[8],'indexer_full':config['indexer_full']+[True]}
    larger_weights=module.random_weights(larger,20261031)
    larger_weights['alpha']=weights['alpha']+.7
    module.write_gguf(different/'model.gguf',larger,larger_weights)
    kv,ts=fixture_contents(different/'model.gguf')
    (different/'model.gguf').write_bytes(_gguf_bytes(byte_tokens(kv),ts))
    (different/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
    metadata=json.loads(run(['build/test_gguf_inventory','--metadata',different],env))
    for key,value in metadata.items():(different/(key+'.json')).write_text(json.dumps(value))
    run(['build/glm-upstream-oracle',different,different/'oracle.json'],env)
    command=['build/mistral-mixed-oracle']
    for root in (dest/'F32',dest/'Q8_0',different):command.extend([root,root/'oracle.json'])
    print(run(command,env),end='',flush=True)

if __name__=='__main__':main()
