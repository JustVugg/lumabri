#!/usr/bin/env python3
"""DeepSeek V4 native GGUF inference through resident Edge and Segment."""
import argparse
import json
import os
import re
import sys
from pathlib import Path
import subprocess
import tempfile
from qwen_native_gguf_test import run,byte_tokens
from qwen38_native_gguf_test import gguf_bytes


def fixture(kind):
    import numpy as np
    import gguf
    rng=np.random.default_rng(20261112);H=256;D=128;Q=256;HC=2;E=4;I=256;P=8
    tensors=[]
    def weight(name,shape,matrix=False,norm=False):
        a=rng.normal(0,.025,size=shape).astype('<f4')
        if norm:a+=1
        expert='_exps.' in name
        qt=8 if expert or (matrix and kind!='float') else 0
        if kind=='mixed' and matrix:qt=2 if expert else 30 if len(tensors)%3 else 8
        # The pinned router and index-head scoring consume BF16 values.
        if name.endswith(('ffn_gate_inp.weight','indexer.proj.weight')):qt=0
        # Layer norms are native BF16 values even when stored as F32.
        if norm:a=(a.view('<u4')&np.uint32(0xffff0000)).view('<f4')
        raw=gguf.quants.quantize(a,gguf.GGMLQuantizationType(qt)).tobytes() if qt else a.tobytes()
        tensors.append((name,qt,list(reversed(shape)),raw))
    weight('token_embd.weight',(320,H),True);weight('output.weight',(320,H),True)
    weight('output_norm.weight',(H,),norm=True);weight('output_hc_fn.weight',(HC,HC*H))
    weight('output_hc_base.weight',(HC,));weight('output_hc_scale.weight',(1,))
    ratios=[0,4,8,128]
    for l,r in enumerate(ratios):
        p=f'blk.{l}.'
        for stem in ('hc_attn','hc_ffn'):
            weight(p+stem+'_fn.weight',(P,HC*H));weight(p+stem+'_base.weight',(P,));weight(p+stem+'_scale.weight',(3,))
        weight(p+'attn_sinks.weight',(4,))
        for n,shape in [('attn_q_a',(Q,H)),('attn_q_b',(4*D,Q)),('attn_kv',(D,H)),
                         ('attn_output_a',(256,256)),('attn_output_b',(H,256))]:weight(p+n+'.weight',shape,True)
        for n,size in [('attn_q_a_norm',Q),('attn_kv_a_norm',D),('attn_norm',H),('ffn_norm',H)]:weight(p+n+'.weight',(size,),norm=True)
        if r:
            coff=2 if r==4 else 1
            weight(p+'attn_compressor_ape.weight',(r,coff*D));weight(p+'attn_compressor_norm.weight',(D,),norm=True)
            weight(p+'attn_compressor_kv.weight',(coff*D,H),True);weight(p+'attn_compressor_gate.weight',(coff*D,H),True)
        if r==4:
            weight(p+'indexer_compressor_ape.weight',(4,256));weight(p+'indexer_compressor_norm.weight',(128,),norm=True)
            weight(p+'indexer_compressor_kv.weight',(256,H),True);weight(p+'indexer_compressor_gate.weight',(256,H),True)
            weight(p+'indexer.attn_q_b.weight',(256,Q),True);weight(p+'indexer.proj.weight',(2,H),True)
        weight(p+'ffn_gate_inp.weight',(E,H),True)
        if not l:
            table=np.array([[(i+k)%E for k in range(2)] for i in range(320)],dtype='<i4')
            tensors.append((p+'ffn_gate_tid2eid.weight',26,[2,320],table.tobytes()))
        else:weight(p+'exp_probs_b.bias',(E,))
        for n,shape in [('gate',(I,H)),('up',(I,H)),('down',(H,I))]:
            weight(p+f'ffn_{n}_shexp.weight',shape,True);weight(p+f'ffn_{n}_exps.weight',(E,*shape),True)
    kv=[('general.architecture',8,'deepseek4'),('tokenizer.ggml.model',8,'gpt2'),
        ('tokenizer.ggml.tokens',9,(8,[])),('tokenizer.ggml.merges',9,(8,[])),
        ('tokenizer.ggml.bos_token_id',4,0),('tokenizer.ggml.eos_token_id',4,1)]
    fields={'block_count':4,'embedding_length':H,'attention.head_count':4,'attention.key_length':D,
        'attention.q_lora_rank':Q,'rope.dimension_count':32,'attention.output_group_count':2,
        'attention.output_lora_rank':128,'attention.sliding_window':8,'attention.indexer.head_count':2,
        'attention.indexer.key_length':128,'attention.indexer.top_k':16,'expert_count':E,'expert_used_count':2,
        'expert_feed_forward_length':I,'hash_layer_count':1,'hyper_connection.count':HC,
        'hyper_connection.sinkhorn_iterations':20,'context_length':512}
    kv += [('deepseek4.'+k,4,v) for k,v in fields.items()]
    kv += [('deepseek4.attention.compress_ratios',9,(4,ratios)),('deepseek4.attention.compress_rope_freq_base',6,10000.)]
    return byte_tokens(kv),tensors


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--household',action='store_true')
    p.add_argument('--engine',type=Path,required=True);args=p.parse_args();engine=args.engine.resolve()
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','LUMABRI_RESIDENT_REQUIRED':'1',
        'LUMABRI_ENGINE_BACKEND':'cpu','GQ_EXACT':'1','V4_DRAFT':'0','COLI_V4_DIRECT':'0','COLI_V4_EXPERT_PREFETCH':'0'}
    dest=Path('build/v4-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    for kind in ('float','Q8_0','mixed'):
        root=dest/kind;root.mkdir(exist_ok=True);kv,ts=fixture(kind)
        (root/'model.gguf').write_bytes(gguf_bytes(kv,ts));(root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        meta=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in meta.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==10
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        run(['build/v4-upstream-oracle',root,root/'oracle.json'],env,180)
        for cut in range(1,4):print(run(['build/gguf-segment-oracle','deepseek_v4',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)}),end='',flush=True)
        print(f'DeepSeek V4 {kind}: all cuts, 32 upstream tokens, sealed input PASS',flush=True)
        print(run(['build/v4-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-v4-contract-') as temp:
            tmp=Path(temp);(tmp/'config.json').write_text(json.dumps(meta['config']));(tmp/'gguf.json').write_text((root/'gguf.json').read_text())
            def denied(tensors,metadata=kv):
                (tmp/'model.gguf').write_bytes(gguf_bytes(metadata,tensors))
                result=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert result.returncode!=0,'invalid DeepSeek V4 checkpoint admitted'
            for name in ('output.weight','blk.0.ffn_gate_tid2eid.weight','blk.1.indexer.proj.weight',
                         'blk.2.attn_compressor_norm.weight','blk.3.ffn_down_exps.weight'):
                denied([t for t in ts if t[0]!=name])
            denied(ts+[('blk.1.unknown.weight',0,[1],b'\0'*4)])
            denied(ts,[(k,t,(4,[0,4,8,7]) if k=='deepseek4.attention.compress_ratios' else v) for k,t,v in kv])
            denied(ts,[(k,t,3 if k=='deepseek4.hyper_connection.count' else v) for k,t,v in kv])
            # The pinned raw-BF16 router and index-head kernels cannot decode Q8.
            import numpy as np
            import gguf
            for suffix in ('ffn_gate_inp.weight','indexer.proj.weight'):
                name=next(n for n,_,_,_ in ts if n.endswith(suffix))
                denied([(n,8,d,gguf.quants.quantize(np.frombuffer(raw,dtype='<f4').reshape(list(reversed(d))),gguf.GGMLQuantizationType.Q8_0).tobytes())
                        if n==name else (n,t,d,raw) for n,t,d,raw in ts])
            (tmp/'model.gguf').write_bytes((root/'model.gguf').read_bytes())
            (tmp/'config.json').write_text(json.dumps({**meta['config'],'hc_mult':3}))
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode
        print(f'DeepSeek V4 {kind}: incomplete weights, unsupported kernels and config mismatch refused PASS',flush=True)
        if kind=='Q8_0':
            packed=root/'packed';run([sys.executable,engine/'tools/gguf_import.py',root/'model.gguf',packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/gguf-segment-oracle','deepseek_v4',packed,root/'oracle.json'],env),end='',flush=True)
        if kind=='mixed':
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):(split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(gguf_bytes(kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(ts))],ts[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            for key,value in json.loads(run(['build/test_gguf_inventory','--metadata',split],env)).items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','deepseek_v4',split,root/'oracle.json'],env),end='',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True);target=raw/'model.gguf'
                if not target.exists():os.link(root/'model.gguf',target)
                catalog=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],{**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalog)==1 and catalog[0]['sizing_verified'],catalog
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',raw,'--resident-default','--crash-requester','--context','512'],env,600),end='',flush=True)
    cmd=['build/v4-mixed-oracle']
    for kind in ('float','Q8_0','mixed'):cmd.extend([dest/kind,dest/kind/'oracle.json'])
    print(run(cmd,{**env,'LUMABRI_RESIDENCY_POLICY':'engine-locked','LMB_ORACLE_ACCOUNTING':'1'},180),end='',flush=True)
    root=dest/'mixed';classes=set()
    for exact,rows,identity in (('1','1','0'),('0','1','0'),('1','0','0'),('1','1','1')):
        mode={**env,'GQ_EXACT':exact,'COLI_V4_ROWS16':rows,'V4_IDX_IDENTITY':identity,'LMB_ORACLE_PRINT_CAPS':'1'}
        ref=root/f'oracle-{exact}-{rows}-{identity}.json';run(['build/v4-upstream-oracle',root,ref],mode)
        output=run(['build/gguf-segment-oracle','deepseek_v4',root,ref],mode)
        classes.add(re.search(r'^numeric_class=(.+)$',output,re.M).group(1))
    assert len(classes)==4,classes
    print('DeepSeek V4 arithmetic modes: distinct capability identities and matching tokens PASS',flush=True)
    print('DEEPSEEK V4 NATIVE GGUF: PASS',flush=True)


if __name__=='__main__':main()
