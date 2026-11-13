#!/usr/bin/env python3
"""Qwen3.8 GGUF text: PLE, hyper-connections and recurrent Segment state."""
import argparse
import json
import os
import re
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
from qwen_native_gguf_test import run,byte_tokens


def gguf_bytes(metadata,tensors):
    import gguf
    import numpy as np
    with tempfile.TemporaryDirectory(prefix='lmb-q38-writer-') as tmp:
        path=Path(tmp)/'weights.gguf';writer=gguf.GGUFWriter(path,'qwen4exp')
        for key,kind,value in metadata:
            if key=='general.architecture':continue
            if kind==9:
                subtype,items=value
                if not items:continue
                writer.add_key_value(key,items,gguf.GGUFValueType.ARRAY,gguf.GGUFValueType(subtype))
            else:writer.add_key_value(key,value,gguf.GGUFValueType(kind))
        for name,kind,dims,raw in tensors:
            writer.add_tensor_info(name,list(reversed(dims)),np.dtype('float32'),len(raw),gguf.GGMLQuantizationType(kind))
        writer.write_header_to_file();writer.write_kv_data_to_file();writer.write_ti_data_to_file()
        for _,_,_,raw in tensors:writer.write_tensor_data(np.frombuffer(raw,dtype=np.uint8))
        writer.close();return path.read_bytes()


def fixture(kind):
    import numpy as np
    import gguf
    rng=np.random.default_rng(20261112);h=256;hc=2;rank=32;layers=4;experts=4
    tensors=[]
    def weight(name,shape,matrix=False,norm=False,negative=False):
        a=rng.normal(0,.025,size=shape).astype('<f4')
        if norm:a+=1
        if negative:a=-np.exp(a)
        q=0
        if matrix:q=8 if kind=='Q8_0' else (2 if 'down_exps' in name else 30 if len(tensors)%3 else 8) if kind=='mixed' else 0
        if kind=='mixed' and 'indexer.' in name and matrix:q=8
        raw=gguf.quants.quantize(a,gguf.GGMLQuantizationType(q)).tobytes() if q else a.tobytes()
        tensors.append((name,q,list(reversed(shape)),raw))
    weight('token_embd.weight',(320,h),True);weight('output.weight',(320,h),True)
    weight('output_hc_norm.weight',(hc*h,),norm=True)
    weight('output_hc_down.weight',(rank,hc*h),True);weight('output_hc_up.weight',(hc*h,rank),True)
    vocab=[17,19,23,29,31,37,41,43];offsets=[];rows=0
    for size in vocab:offsets.append(rows);rows+=size
    weight('per_layer_token_embd.weight',(rows,32),True)
    for l in range(layers):
        p=f'blk.{l}.'
        for prefix in ('hc_attn','hc_ffn'):
            weight(p+prefix+'_norm.weight',(hc*h,),norm=True)
            weight(p+prefix+'_down.weight',(rank,hc*h),True);weight(p+prefix+'_up.weight',(hc*h,rank),True)
            weight(p+prefix+'_inject.weight',(hc,hc*h))
        weight(p+'ffn_gate_inp.weight',(experts,h),True);weight(p+'ffn_gate_inp_shexp.weight',(h,))
        for part in ('gate','up','down'):
            weight(p+f'ffn_{part}_shexp.weight',(h,h),True);weight(p+f'ffn_{part}_exps.weight',(experts,h,h),True)
        if l%2:
            for name,shape in [('attn_q',(512,h)),('attn_k',(128,h)),('attn_v',(128,h)),('attn_output',(h,256)),
                               ('indexer.q_proj',(128,h)),('indexer.k_proj',(64,h))]:weight(p+name+'.weight',shape,True)
            for name in ('attn_q_norm','attn_k_norm','indexer.q_norm','indexer.k_norm'):weight(p+name+'.weight',(64,),norm=True)
        else:
            for name,shape in [('attn_qkv',(256,h)),('attn_gate',(128,h)),('ssm_alpha',(4,h)),('ssm_beta',(4,h)),('ssm_out',(h,128))]:weight(p+name+'.weight',shape,True)
            weight(p+'ssm_conv1d.weight',(256,4));weight(p+'ssm_dt.bias',(4,));weight(p+'ssm_a',(4,),negative=True)
            weight(p+'ssm_norm.weight',(32,),norm=True)
        if l==1:
            weight(p+'ple_key.weight',(hc*h,256),True);weight(p+'ple_value.weight',(h,256),True)
            for name in ('ple_norm_key','ple_norm_query','ple_norm_conv'):weight(p+name+'.weight',(hc*h,),norm=True)
            weight(p+'ple_conv1d.weight',(hc*h,4))
    kv=[('general.architecture',8,'qwen4exp'),('tokenizer.ggml.model',8,'gpt2'),
        ('tokenizer.ggml.tokens',9,(8,[])),('tokenizer.ggml.merges',9,(8,[])),
        ('tokenizer.ggml.bos_token_id',4,0),('tokenizer.ggml.eos_token_id',4,1)]
    numbers={'block_count':layers,'embedding_length':h,'context_length':512,'hyper_connection.count':hc,'hyper_connection.low_rank':rank,
        'attention.head_count':4,'attention.head_count_kv':2,'attention.key_length':64,'rope.dimension_count':32,
        'attention.indexer.head_count':2,'attention.indexer.key_length':64,'attention.indexer.top_k':16,
        'expert_count':experts,'expert_used_count':2,'expert_feed_forward_length':h,'expert_shared_feed_forward_length':h,
        'ssm.group_count':2,'ssm.time_step_rank':4,'ssm.inner_size':128,'ssm.state_size':32,'ssm.conv_kernel':4,
        'embedding_length_per_layer_input':32,'ple.ngram_size':3,'ple.heads_per_ngram':4,'ple.conv_kernel':4}
    kv += [('qwen4exp.'+k,4,v) for k,v in numbers.items()]
    arrays={'attention.compress_ratios':(4,[0,2,0,2]),'attention.recurrent_layers':(7,[1,0,1,0]),
        'ple.layers':(4,[1]),'ple.layer_multipliers':(11,[1009,1013,1019]),
        'ple.head_vocab_sizes':(11,vocab),'ple.head_offsets':(11,offsets)}
    kv += [('qwen4exp.'+k,9,v) for k,v in arrays.items()]
    kv += [('qwen4exp.rope.freq_base',6,10000.),('qwen4exp.attention.layer_norm_rms_epsilon',6,1e-6)]
    return byte_tokens(kv),tensors


def safetensors_fixture(root,engine,ple_dtype='F32',ple_parts=1):
    """Exercise the existing loader alongside GGUF using its own oracle."""
    import numpy as np
    meta,source=fixture('float');text=(engine/'qwen38_core.h').read_text()
    text=text[text.index('static const char *q38_gg_name'):text.index('static const gguf_ctx *q38_gg_ctx_of')]
    mapping={gg:(hf,int(norm)) for hf,gg,norm in re.findall(r'\{"([^"]+)","([^"]+)",(\d)\}',text)}
    weights={};experts={};indexer={}
    for name,_,dims,raw in source:
        a=np.frombuffer(raw,dtype='<f4').reshape(list(reversed(dims))).copy()
        if name=='per_layer_token_embd.weight':weights['model.layers.1.ple.ple_embedding.ngram_embedding.weight']=a;continue
        suffix=name;prefix=''
        if name.startswith('blk.'):
            _,l,suffix=name.split('.',2);prefix=f'model.layers.{l}.'
            if suffix.endswith('_exps.weight'):experts[(l,suffix.split('_')[1])]=a;continue
            if suffix in ('indexer.q_proj.weight','indexer.k_proj.weight'):
                indexer[(l,suffix.split('.')[1])]=a;continue
        hf,norm=mapping[suffix]
        if norm:a-=1
        if suffix=='ssm_a':a=np.log(-a)
        weights[prefix+hf]=a
    for l in range(4):
        weights[f'model.layers.{l}.mlp.experts.gate_up_proj']=np.concatenate([experts[(str(l),'gate')],experts[(str(l),'up')]],axis=1)
        weights[f'model.layers.{l}.mlp.experts.down_proj']=experts[(str(l),'down')]
        if l%2:weights[f'model.layers.{l}.self_attn.indexer.index_qk_proj.weight']=np.concatenate([indexer[(str(l),'q_proj')],indexer[(str(l),'k_proj')]])
    names={'qwen4exp.ple.layer_multipliers':'layer_multipliers','qwen4exp.ple.head_vocab_sizes':'ngram_heads_vocab_sizes',
        'qwen4exp.ple.head_offsets':'ngram_heads_offsets'}
    for key,_,value in meta:
        if key in names:weights['model.layers.1.ple.ple_embedding.'+names[key]]=np.array(value[1],dtype='<i8')
    ple_name='model.layers.1.ple.ple_embedding.ngram_embedding.'
    if ple_parts>1:
        table=weights.pop(ple_name+'weight')
        for i,part in enumerate(np.array_split(table,ple_parts)):weights[ple_name+f'shard_{i}.weight']=part
    header={};data=bytearray()
    for name,a in weights.items():
        dtype='I64' if a.dtype.kind=='i' else 'F32'
        if name.startswith(ple_name):
            dtype=ple_dtype
            if dtype=='F16':a=a.astype('<f2')
            elif dtype=='BF16':a=(a.view('<u4')>>16).astype('<u2')
        raw=a.tobytes();start=len(data);data+=raw
        header[name]={'dtype':dtype,'shape':list(a.shape),'data_offsets':[start,len(data)]}
    encoded=json.dumps(header,separators=(',',':')).encode();encoded+=b' '*((-len(encoded))%8)
    (root/'model.safetensors').write_bytes(struct.pack('<Q',len(encoded))+encoded+data)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',required=True,type=Path)
    p.add_argument('--household',action='store_true');args=p.parse_args();engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    _gguf_bytes=gguf_bytes
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','LUMABRI_RESIDENT_REQUIRED':'1',
        'LUMABRI_ENGINE_BACKEND':'cpu','Q38_TRUNK_CPU_INT8':'0','Q38_TRUNK_CPU_INT4':'0','Q38_KV':'f32','GQ_EXACT':'1'}
    dest=Path('build/qwen38-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    for kind in ('float','Q8_0','mixed'):
        root=dest/kind;root.mkdir(exist_ok=True);kv,ts=fixture(kind)
        (root/'model.gguf').write_bytes(_gguf_bytes(kv,ts));(root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        meta=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in meta.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==10
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        run(['build/qwen38-upstream-oracle',root,root/'oracle.json'],env)
        for cut in range(1,4):run(['build/gguf-segment-oracle','qwen38',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
        print(f'Qwen3.8 {kind}: all cuts, 32 upstream tokens, sealed weight input PASS',flush=True)
        print(run(['build/qwen38-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-q38-contract-') as temp:
            tmp=Path(temp);(tmp/'config.json').write_text(json.dumps(meta['config']));(tmp/'gguf.json').write_text((root/'gguf.json').read_text())
            def denied(tensors,metadata=kv):
                (tmp/'model.gguf').write_bytes(_gguf_bytes(metadata,tensors))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'invalid Qwen3.8 checkpoint admitted'
            for name in ('output.weight','per_layer_token_embd.weight','blk.1.ple_key.weight','blk.3.indexer.k_proj.weight','blk.2.ffn_down_exps.weight'):
                denied([t for t in ts if t[0]!=name])
            denied(ts+[('blk.1.unknown.weight',0,[1],b'\0'*4)])
            denied(ts,[(k,t,(4,[0,2,0,4]) if k=='qwen4exp.attention.compress_ratios' else v) for k,t,v in kv])
            denied(ts,[(k,t,(11,[999999]*8) if k=='qwen4exp.ple.head_offsets' else v) for k,t,v in kv])
            (tmp/'model.gguf').write_bytes((root/'model.gguf').read_bytes())
            (tmp/'config.json').write_text(json.dumps({**meta['config'],'hc_lowrank':64}))
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode
        print(f'Qwen3.8 {kind}: incomplete weights, bad PLE and configuration refused PASS',flush=True)
        if kind=='mixed':
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):(split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(ts))],ts[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            for key,value in json.loads(run(['build/test_gguf_inventory','--metadata',split],env)).items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','qwen38',split,root/'oracle.json'],env),end='',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True);target=raw/'model.gguf'
                if not target.exists():os.link(root/'model.gguf',target)
                catalog=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],{**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalog)==1 and catalog[0]['sizing_verified'],catalog
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',raw,'--resident-default','--crash-requester','--context','512'],env,600),end='',flush=True)
    cmd=['build/qwen38-mixed-oracle']
    for kind in ('float','Q8_0','mixed'):cmd.extend([dest/kind,dest/kind/'oracle.json'])
    print(run(cmd,{**env,'LUMABRI_RESIDENCY_POLICY':'engine-locked','LMB_ORACLE_ACCOUNTING':'1'},180),end='',flush=True)
    native=dest/'safetensors';native.mkdir(exist_ok=True);safetensors_fixture(native,engine)
    for file in ('config.json','tokenizer.json','tokenizer_config.json','generation_config.json'):(native/file).write_bytes((dest/'float'/file).read_bytes())
    run(['build/test_gguf_inventory','--cost',native],env);run(['build/qwen38-upstream-oracle',native,native/'oracle.json'],env)
    for cut in range(1,4):run(['build/gguf-segment-oracle','qwen38',native,native/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
    cmd=['build/qwen38-mixed-oracle']
    for kind in ('safetensors','Q8_0','mixed'):cmd.extend([dest/kind,dest/kind/'oracle.json'])
    print(run(cmd,{**env,'LUMABRI_RESIDENCY_POLICY':'engine-locked','LMB_ORACLE_ACCOUNTING':'1'},180),end='',flush=True)
    for dtype,parts in (('F16',1),('BF16',2)):
        root=dest/('ple-'+dtype);root.mkdir(exist_ok=True);safetensors_fixture(root,engine,dtype,parts)
        for file in ('tokenizer.json','tokenizer_config.json','generation_config.json'):(root/file).write_bytes((native/file).read_bytes())
        cfg=json.loads((native/'config.json').read_text());cfg['split_ngram_parts']=parts
        (root/'config.json').write_text(json.dumps(cfg))
        run(['build/test_gguf_inventory','--cost',root],env)
        run(['build/qwen38-upstream-oracle',root,root/'oracle.json'],env)
        print(run(['build/gguf-segment-oracle','qwen38',root,root/'oracle.json'],env),end='',flush=True)
        print(f'Qwen3.8 PLE {dtype}, {parts} part(s): resident rows match the native engine PASS',flush=True)
    root=dest/'mixed';classes=set()
    for exact,bf16,kv in (('1','1','f32'),('0','1','f32'),('1','0','f32'),('1','1','f16')):
        mode={**env,'GQ_EXACT':exact,'Q38_NATIVE_BF16':bf16,'Q38_KV':kv,'LMB_ORACLE_PRINT_CAPS':'1'}
        ref=root/f'oracle-{exact}-{bf16}-{kv}.json';run(['build/qwen38-upstream-oracle',root,ref],mode)
        output=run(['build/gguf-segment-oracle','qwen38',root,ref],mode)
        classes.add(re.search(r'^numeric_class=(.+)$',output,re.M).group(1))
        if kv=='f16':print(run(['build/qwen38-state-oracle',root],mode),end='',flush=True)
    assert len(classes)==4,classes
    print('Qwen3.8 arithmetic/KV modes: distinct capability identities and matching oracle tokens PASS',flush=True)
    print('QWEN3.8 NATIVE GGUF: PASS',flush=True)


if __name__=='__main__':main()
