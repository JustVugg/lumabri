#!/usr/bin/env python3
"""GLM native GGUF and shared DSA selections through every Segment cut."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from qwen_native_gguf_test import run,fixture_contents,byte_tokens


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine',required=True,type=Path)
    parser.add_argument('--household',action='store_true')
    parser.add_argument('--mistral-fixture',type=Path,help='also interleave GLM and Mistral checkpoints')
    args=parser.parse_args();engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    spec=importlib.util.spec_from_file_location('upstream_gguf_fixture',engine/'tests/test_gguf_import.py')
    upstream=importlib.util.module_from_spec(spec);spec.loader.exec_module(upstream)
    import gguf
    import numpy as np
    from make_flint_tiny import _gguf_bytes
    upstream.VOCAB=320
    dest=Path('build/glm-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    upstream.write_glmdsa(dest/'source.gguf',np.random.default_rng(20261111))
    kv,weights=fixture_contents(dest/'source.gguf');kv=byte_tokens(kv)
    # Four is deliberately smaller than the prompt: this exercises real sparse
    # selections, rather than merely reading the full causal attention prefix.
    kv=[(k,t,4 if k=='glm-dsa.attention.indexer.top_k' else v) for k,t,v in kv]
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','COLI_DENSE_I8':'0',
        'LUMABRI_RESIDENT_REQUIRED':'1','LUMABRI_ENGINE_BACKEND':'cpu','PILOT':'0',
        'GLM_SEGMENT_EBITS':'16','GLM_SEGMENT_DBITS':'16'}
    models=[]
    for kind in ('native','shared','float','tied'):
        root=dest/kind;root.mkdir(exist_ok=True);models.append(root)
        tensors=list(weights)
        if kind!='native':
            tensors=[t for t in tensors if not any(t[0].startswith(f'blk.{i}.indexer.') for i in (1,3))]
        if kind=='tied':tensors=[t for t in tensors if t[0]!='output.weight']
        if kind=='float':
            tensors=[(n,0,d,gguf.quants.dequantize(np.frombuffer(b,dtype=np.uint8),gguf.GGMLQuantizationType(t))
                .astype('<f4').tobytes()) if t else (n,t,d,b) for n,t,d,b in tensors]
        path=root/'model.gguf';path.write_bytes(_gguf_bytes(kv,tensors))
        (root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        metadata=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in metadata.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==10
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        reference=root
        if kind=='tied':
            # Upstream's GLM loader requires an explicit head tensor. Copy the
            # exact embedding bytes only in its reference container; Lumabri
            # must read the single tensor from the original tied checkpoint.
            reference=root/'reference';reference.mkdir(exist_ok=True)
            n,t,d,b=next(t for t in tensors if t[0]=='token_embd.weight')
            (reference/'model.gguf').write_bytes(_gguf_bytes(kv,tensors+[('output.weight',t,d,b)]))
            (reference/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
            for key,value in metadata.items():(reference/(key+'.json')).write_text(json.dumps(value))
        run(['build/glm-upstream-oracle',reference,root/'oracle.json'],env)
        for cut in range(1,4):
            run(['build/gguf-segment-oracle','glm',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
        print(f'GLM {kind}: all cuts, 32 upstream greedy tokens, no inference weight reads PASS',flush=True)
        print(run(['build/mistral-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-glm-gguf-contract-') as tmp:
            tmp=Path(tmp);(tmp/'config.json').write_text(json.dumps(metadata['config']))
            (tmp/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
            def denied(ts,meta=kv):
                (tmp/'model.gguf').write_bytes(_gguf_bytes(meta,ts))
                p=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert p.returncode!=0,'incomplete GLM GGUF admitted'
            for name in ('blk.0.indexer.k_norm.bias','blk.1.exp_probs_b.bias','blk.3.attn_v_b.weight'):
                denied([t for t in tensors if t[0]!=name])
            denied(tensors+[('blk.0.unknown.weight',0,[1],struct.pack('<f',1))])
            denied([(n,t,[d[1],d[0]],b) if n=='blk.0.attn_q_a.weight' else (n,t,d,b) for n,t,d,b in tensors])
            denied(tensors,[(k,t,2 if k=='glm-dsa.expert_shared_count' else v) for k,t,v in kv])
            (tmp/'model.gguf').write_bytes(path.read_bytes())
            for key in ('q_lora_rank','kv_lora_rank','index_topk'):
                (tmp/'config.json').write_text(json.dumps({**metadata['config'],key:metadata['config'][key]+1}))
                p=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert p.returncode!=0,'mismatching GLM config admitted'
            if kind=='tied':
                (tmp/'config.json').write_text(json.dumps({**metadata['config'],'tie_word_embeddings':False}))
                p=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert p.returncode!=0,'missing untied output head admitted'
        if kind=='shared':
            packed=root/'packed'
            run([sys.executable,engine/'tools/gguf_import.py',path,packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/gguf-segment-oracle','glm',packed,root/'oracle.json'],env),end='',flush=True)
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):
                meta=kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(tensors))]
                (split/f'model-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(meta,tensors[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model-00001-of-00002.gguf'}))
            for key,value in metadata.items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','glm',split,root/'oracle.json'],env),end='',flush=True)
            print('GLM: split/packed GGUF preserve the native oracle PASS',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True)
                if not (raw/'model.gguf').exists():os.link(path,raw/'model.gguf')
                catalog=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],
                    {**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalog)==1 and catalog[0]['sizing_verified'],catalog
                folder=root/'home';folder.mkdir(exist_ok=True);link=folder/'glm'
                if not link.exists():link.symlink_to(next(raw.glob('.lumabri-gguf-*')),target_is_directory=True)
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',folder,
                    '--resident-default','--crash-requester','--context','512'],env,600),end='',flush=True)
    groups=[models[:3],models[1:]]
    if args.mistral_fixture:
        other=args.mistral_fixture.resolve()
        assert (other/'oracle.json').is_file(),'run test-mistral-segments first'
        groups.append([models[0],other,models[-1]])
    for group in groups:
        cmd=['build/mistral-mixed-oracle']
        for root in group:cmd.extend([root,root/'oracle.json'])
        print(run(cmd,env),end='',flush=True)


if __name__=='__main__':main()
