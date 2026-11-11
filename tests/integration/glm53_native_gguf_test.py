#!/usr/bin/env python3
"""GLM5-Next GGUF: KDA/MLA/mHC ranges against the pinned monolithic engine."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from qwen_native_gguf_test import run,byte_tokens


def fixture(kind):
    import numpy as np
    from make_flint_tiny import _q8_0
    rng=np.random.default_rng(20261112)
    kv=[('general.architecture',8,'glm5-next'),('tokenizer.ggml.model',8,'gpt2'),
        ('tokenizer.ggml.tokens',9,(8,[])),('tokenizer.ggml.merges',9,(8,[]))]
    values={'block_count':4,'embedding_length':128,'attention.head_count':4,'expert_count':4,
        'expert_used_count':2,'expert_feed_forward_length':128,'feed_forward_length':256,
        'expert_shared_count':1,'leading_dense_block_count':1,'attention.q_lora_rank':128,
        'attention.kv_lora_rank':64,'attention.key_length_mla':32,'attention.value_length_mla':32,
        'attention.indexer.head_count':2,'attention.indexer.key_length':32,'attention.indexer.top_k':4,
        'attention.indexer.kpool':2,'kda.head_dim':32,'ssm.conv_kernel':4,'hyper_connection.count':2,
        'hyper_connection.sinkhorn_iterations':3,'context_length':512,'rope.dimension_count':0}
    kv += [('glm5-next.'+k,4,v) for k,v in values.items()]
    kv += [('glm5-next.attention.head_count_kv',9,(4,[0,1,0,1])),
        ('glm5-next.attention.indexer.types',9,(7,[True]*4)),
        ('glm5-next.attention.indexer.kpool_select_tail',7,True),
        ('glm5-next.expert_weights_scale',6,1.5),('glm5-next.swiglu_clamp_exp',9,(6,[10.]))]
    tensors=[]
    def add(name,shape,mat=False,norm=False,constant=None):
        a=(rng.normal(0,.025,size=shape)).astype('<f4')
        if norm:a+=1
        if constant is not None:a.fill(constant)
        typ=0;data=a.tobytes()
        if kind!='F32' and mat and shape[-1]%32==0:
            typ=8;data=_q8_0(a)
            if kind=='mixed' and name.startswith('blk.3.ffn_') and '_exps.' in name:
                typ=1;data=a.astype('<f2').tobytes()
        tensors.append((name,typ,list(reversed(shape)),data))
    add('token_embd.weight',(320,128));add('output_norm.weight',(128,),norm=True)
    if kind!='tied':add('output.weight',(320,128),mat=True)
    for l in range(4):
        def w(s,shape,**kw):add(f'blk.{l}.{s}',shape,**kw)
        w('attn_norm.weight',(128,),norm=True);w('ffn_norm.weight',(128,),norm=True)
        for branch in ('attn','ffn'):
            w(f'hc_{branch}_fn.weight',(8,256));w(f'hc_{branch}_base.weight',(8,))
            w(f'hc_{branch}_scale.weight',(3,),constant=1)
        if l%2:
            w('attn_q_a.weight',(128,128),mat=True);w('attn_q_a_norm.weight',(128,),norm=True)
            w('attn_q_b.weight',(128,128),mat=True);w('attn_kv_a_mqa.weight',(64,128),mat=True)
            w('attn_kv_a_norm.weight',(64,),norm=True)
            w('attn_k_b.weight',(4,64,32),mat=True);w('attn_v_b.weight',(4,32,64),mat=True)
            w('attn_output.weight',(128,128),mat=True)
            w('indexer.attn_q_b.weight',(64,128),mat=True);w('indexer.attn_k.weight',(32,128),mat=True)
            w('indexer.proj.weight',(2,128),mat=True);w('indexer.k_norm.weight',(32,),norm=True)
            w('indexer.k_norm.bias',(32,));w('indexer_compressor_ape.weight',(2,32))
            w('indexer_compressor_gate.weight',(32,128),mat=True)
        else:
            for s in ('attn_q','attn_k','attn_v','attn_output'):w(s+'.weight',(128,128),mat=True)
            for s in ('ssm_g_a','ssm_f_a'):w(s+'.weight',(32,128),mat=True)
            for s in ('ssm_g_b','ssm_f_b'):w(s+'.weight',(128,32),mat=True)
            w('ssm_beta.weight',(4,128),mat=True);w('ssm_dt.bias',(128,))
            w('ssm_a',(4,),constant=-1);w('ssm_norm.weight',(32,),norm=True)
            for s in ('q','k','v'):w(f'ssm_conv1d_{s}.weight',(128,4))
        if l==0:
            w('ffn_gate.weight',(256,128),mat=True);w('ffn_up.weight',(256,128),mat=True)
            w('ffn_down.weight',(128,256),mat=True)
        else:
            w('ffn_gate_inp.weight',(4,128));w('exp_probs_b.bias',(4,))
            for s in ('gate','up','down'):
                w(f'ffn_{s}_shexp.weight',(128,128),mat=True)
                w(f'ffn_{s}_exps.weight',(4,128,128),mat=True)
    if kind=='flat':
        tensors=[(n,t,[d[0],d[1]*d[2]],b) if n.endswith(('attn_k_b.weight','attn_v_b.weight'))
                 else (n,t,d,b) for n,t,d,b in tensors]
    return byte_tokens(kv),tensors


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',required=True,type=Path)
    p.add_argument('--household',action='store_true');args=p.parse_args()
    engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    from make_flint_tiny import _gguf_bytes
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off',
        'LUMABRI_RESIDENT_REQUIRED':'1','LUMABRI_ENGINE_BACKEND':'cpu','GLM53_BITS':'32'}
    dest=Path('build/glm53-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    for kind in ('F32','Q8_0','mixed','tied','flat'):
        root=dest/kind;root.mkdir(exist_ok=True);kv,ts=fixture(kind)
        (root/'model.gguf').write_bytes(_gguf_bytes(kv,ts))
        (root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        meta=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in meta.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==10
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        run(['build/glm53-upstream-oracle',root,root/'oracle.json'],env)
        for cut in range(1,4):
            run(['build/gguf-segment-oracle','glm53',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
        print(f'GLM5-Next {kind}: all cuts, 32 upstream tokens, no inference reads PASS',flush=True)
        print(run(['build/glm53-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-g5-contract-') as tmp:
            tmp=Path(tmp);(tmp/'config.json').write_text(json.dumps(meta['config']))
            (tmp/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
            def denied(tensors,metadata=kv):
                (tmp/'model.gguf').write_bytes(_gguf_bytes(metadata,tensors))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'invalid GLM5-Next checkpoint admitted'
            for name in ('blk.0.ssm_f_a.weight','blk.1.attn_k_b.weight','blk.3.indexer_compressor_gate.weight'):
                denied([t for t in ts if t[0]!=name])
            denied([(n,t,list(reversed(d)),b) if n=='blk.0.ssm_f_a.weight' else (n,t,d,b) for n,t,d,b in ts])
            denied(ts+[('blk.2.unknown.weight',0,[1],b'\0'*4)])
            denied(ts,[(k,t,4 if k=='glm5-next.rope.dimension_count' else v) for k,t,v in kv])
            denied(ts,[(k,t,(4,[0,0,0,1]) if k=='glm5-next.attention.head_count_kv' else v) for k,t,v in kv])
            (tmp/'model.gguf').write_bytes((root/'model.gguf').read_bytes())
            for key in ('hc_mult','index_kpool','kv_lora_rank'):
                bad={**meta['config'],'text_config':{**meta['config']['text_config'],key:meta['config']['text_config'][key]+1}}
                (tmp/'config.json').write_text(json.dumps(bad))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'mismatched config admitted'
        print(f'GLM5-Next {kind}: malformed shapes, missing tensors and unsupported controls rejected PASS',flush=True)
        if kind=='Q8_0':
            packed=root/'packed'
            run([sys.executable,engine/'tools/gguf_import.py',root/'model.gguf',packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/gguf-segment-oracle','glm53',packed,root/'oracle.json'],env),end='',flush=True)
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):
                metadata=kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(ts))]
                (split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(metadata,ts[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            split_meta=json.loads(run(['build/test_gguf_inventory','--metadata',split],env))
            for key,value in split_meta.items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','glm53',split,root/'oracle.json'],env),end='',flush=True)
            print('GLM5-Next packed/split GGUF preserve native oracle PASS',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True);target=raw/'model.gguf'
                if not target.exists():os.link(root/'model.gguf',target)
                catalogue=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],
                    {**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalogue)==1 and catalogue[0]['sizing_verified'],catalogue
                wrapper=next(raw.glob('.lumabri-gguf-*'));models=root/'home';models.mkdir(exist_ok=True)
                link=models/'glm53'
                if not link.exists():link.symlink_to(wrapper,target_is_directory=True)
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',models,
                    '--resident-default','--crash-requester','--context','512'],{**env,'GLM53_BITS':'4'},600),end='',flush=True)
    command=['build/glm53-mixed-oracle']
    for kind in ('F32','Q8_0','mixed'):command.extend([dest/kind,dest/kind/'oracle.json'])
    print(run(command,env),end='',flush=True)
    for bits in ('4','8'):
        root=dest/'F32';numeric={**env,'GLM53_BITS':bits};reference=root/f'oracle-bits{bits}.json'
        run(['build/glm53-upstream-oracle',root,reference],numeric)
        for cut in range(1,4):
            run(['build/gguf-segment-oracle','glm53',root,reference],{**numeric,'LMB_ORACLE_CUT':str(cut)})
        print(f'GLM5-Next F32 with dense bits {bits}: all cuts match monolithic tokens PASS',flush=True)


if __name__=='__main__':main()
