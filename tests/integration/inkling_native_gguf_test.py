#!/usr/bin/env python3
"""Native Inkling GGUF: mixed attention, convolution, experts and resident chat."""
import argparse
import json
import os
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
from qwen_native_gguf_test import run,byte_tokens


def fixture(kind):
    import numpy as np
    import gguf
    rng=np.random.default_rng(20261111)
    h=256;inter=256;dense=320;layers=4;heads=4;hd=32;experts=8;shared=2
    local=[1,0,1,0];tensors=[]
    def weight(name,shape,matrix=False,norm=False,expert=False,scalar=False):
        a=np.ones(shape,dtype='<f4') if norm else rng.normal(0,.04,size=shape).astype('<f4')
        if scalar:a.fill(.7)
        q=0
        if matrix or expert:
            q=8 if kind=='Q8_0' else (1 if len(tensors)%2 else 8) if kind=='mixed' else 0
            if expert and kind=='mixed':q=2 if 'down' in name else 8
        raw=gguf.quants.quantize(a,gguf.GGMLQuantizationType(q)).tobytes() if q else a.tobytes()
        tensors.append((name,q,list(reversed(shape)),raw))
    weight('token_embd.weight',(320,h),matrix=True)
    weight('token_embd_norm.weight',(h,),norm=True)
    weight('output.weight',(320,h),matrix=True)
    weight('output_norm.weight',(h,),norm=True)
    for l in range(layers):
        p=f'blk.{l}.';kv=(4 if local[l] else 2)*hd
        weight(p+'attn_norm.weight',(h,),norm=True);weight(p+'ffn_norm.weight',(h,),norm=True)
        for name,shape in [('attn_q',(heads*hd,h)),('attn_k',(kv,h)),('attn_v',(kv,h)),
            ('attn_r',(heads*8,h)),('attn_output',(h,heads*hd))]:weight(p+name+'.weight',shape,matrix=True)
        weight(p+'attn_q_norm.weight',(hd,),norm=True);weight(p+'attn_k_norm.weight',(hd,),norm=True)
        weight(p+'attn_rel_proj.weight',(8,8 if local[l] else 32))
        for name,dim in [('shortconv_k',kv),('shortconv_v',kv),('shortconv_attn',h),('shortconv_mlp',h)]:weight(p+name+'.weight',(dim,4))
        weight(p+'ffn_gscale.weight',(1,),scalar=True)
        if l==0:
            for name,shape in [('gate',(dense,h)),('up',(dense,h)),('down',(h,dense))]:weight(p+f'ffn_{name}.weight',shape,matrix=True)
        else:
            weight(p+'ffn_gate_inp.weight',(experts+shared,h));weight(p+'exp_probs_b.bias',(experts,))
            for name in ('gate','up','down'):
                shape=(shared,h,inter) if name=='down' else (shared,inter,h)
                # Both the native rank-three bank and equivalent flat rows are
                # used by exporters. Preserve the matrix contraction axis.
                if kind=='mixed':shape=(shape[0]*shape[1],shape[2])
                weight(p+f'ffn_{name}_shexp.weight',shape,matrix=True)
                shape=(experts,h,inter) if name=='down' else (experts,inter,h)
                weight(p+f'ffn_{name}_exps.weight',shape,expert=True)
    kv=[('general.architecture',8,'inkling'),('tokenizer.ggml.model',8,'gpt2'),
        ('tokenizer.ggml.tokens',9,(8,[])),('tokenizer.ggml.merges',9,(8,[])),
        ('tokenizer.ggml.bos_token_id',4,0),('tokenizer.ggml.eos_token_id',4,1)]
    numbers={'block_count':layers,'embedding_length':h,'attention.head_count':heads,'attention.key_length':hd,
        'attention.sliding_window':8,'d_rel':8,'rel_extent':32,'shortconv_kernel':4,'expert_count':experts,
        'expert_used_count':2,'expert_shared_count':shared,'feed_forward_length':dense,'expert_feed_forward_length':inter,
        'dense_block_count':1,'context_length':512,'log_scaling_n_floor':4,'unpadded_vocab_size':310}
    kv += [('inkling.'+k,4,v) for k,v in numbers.items()]
    kv += [('inkling.attention.sliding_window_pattern',9,(7,local)),('inkling.attention.head_count_kv',9,(4,[4,2,4,2]))]
    kv += [('inkling.'+k,6,v) for k,v in {'logit_scale_denom':4.,'expert_weights_scale':2.,'log_scaling_alpha':.1,'attention.layer_norm_rms_epsilon':1e-6}.items()]
    return byte_tokens(kv),tensors


def native_fixture(root):
    import numpy as np
    _,source=fixture('float');weights={}
    names={'token_embd.weight':'model.embed_tokens.weight','token_embd_norm.weight':'model.embed_norm.weight',
        'output.weight':'lm_head.weight','output_norm.weight':'model.norm.weight'}
    layers={'attn_norm':'input_layernorm','ffn_norm':'post_attention_layernorm',
        **{'attn_'+k:'self_attn.'+v+'_proj' for k,v in [('q','q'),('k','k'),('v','v'),('r','r'),('output','o')]},
        'attn_q_norm':'self_attn.q_norm','attn_k_norm':'self_attn.k_norm',
        'shortconv_k':'self_attn.k_sconv.conv1d','shortconv_v':'self_attn.v_sconv.conv1d',
        'shortconv_attn':'attn_sconv.conv1d','shortconv_mlp':'mlp_sconv.conv1d',
        'ffn_gate_inp':'mlp.gate','ffn_gate':'mlp.gate_proj','ffn_up':'mlp.up_proj','ffn_down':'mlp.down_proj'}
    experts={}
    for name,_,dims,raw in source:
        a=np.frombuffer(raw,dtype='<f4').reshape(list(reversed(dims)))
        if name in names:weights[names[name]]=a;continue
        _,idx,suffix=name.split('.',2);layer=int(idx);prefix=f'model.layers.{layer}.'
        if suffix.endswith('_exps.weight'):experts[(layer,suffix.split('_')[1])]=a;continue
        if suffix=='attn_rel_proj.weight':target='self_attn.rel_logits_proj.proj'
        elif suffix=='exp_probs_b.bias':target='mlp.gate.e_score_correction_bias'
        elif suffix=='ffn_gscale.weight':target='mlp.'+('gate.' if layer else '')+'global_scale'
        elif suffix.endswith('_shexp.weight'):target='mlp.shared_experts.'+suffix.split('_')[1]+'_proj'
        else:target=layers[suffix.removesuffix('.weight')]+'.weight'
        weights[prefix+target]=a
    for layer in range(1,4):
        weights[f'model.layers.{layer}.mlp.experts.gate_up_proj']=np.concatenate([experts[(layer,'gate')],experts[(layer,'up')]],axis=1)
        weights[f'model.layers.{layer}.mlp.experts.down_proj']=experts[(layer,'down')]
    header={};payload=bytearray()
    for name,a in weights.items():
        raw=a.tobytes();start=len(payload);payload+=raw
        header[name]={'dtype':'F32','shape':list(a.shape),'data_offsets':[start,len(payload)]}
    raw=json.dumps(header,separators=(',',':')).encode();raw+=b' '*((-len(raw))%8)
    (root/'model.safetensors').write_bytes(struct.pack('<Q',len(raw))+raw+payload)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',required=True,type=Path)
    p.add_argument('--household',action='store_true');args=p.parse_args();engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    from make_flint_tiny import _gguf_bytes
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','LUMABRI_RESIDENT_REQUIRED':'1',
        'LUMABRI_ENGINE_BACKEND':'cpu','INK_SEGMENT_BITS':'0','INK_DENSE_Q4':'0','INK_SHARED_BATCH':'0'}
    dest=Path('build/inkling-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    for kind in ('float','Q8_0','mixed'):
        root=dest/kind;root.mkdir(exist_ok=True);kv,ts=fixture(kind)
        (root/'model.gguf').write_bytes(_gguf_bytes(kv,ts));(root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        meta=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in meta.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==10
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        run(['build/inkling-upstream-oracle',root,root/'oracle.json'],env)
        for cut in range(1,4):run(['build/gguf-segment-oracle','inkling',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
        print(f'Inkling {kind}: all cuts, 32 upstream tokens, sealed weight input PASS',flush=True)
        print(run(['build/inkling-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-inkling-contract-') as temp:
            tmp=Path(temp);(tmp/'config.json').write_text(json.dumps(meta['config']));(tmp/'gguf.json').write_text((root/'gguf.json').read_text())
            def denied(tensors,metadata=kv):
                (tmp/'model.gguf').write_bytes(_gguf_bytes(metadata,tensors))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'invalid Inkling checkpoint admitted'
            for name in ('output.weight','blk.0.shortconv_k.weight','blk.1.attn_rel_proj.weight','blk.3.ffn_down_exps.weight'):
                denied([t for t in ts if t[0]!=name])
            denied(ts+[('blk.1.unknown.weight',0,[1],b'\0'*4)])
            denied([(n,t,list(reversed(d)),b) if n=='blk.0.shortconv_k.weight' else (n,t,d,b) for n,t,d,b in ts])
            denied(ts,[(k,t,(4,[4,2,2,2]) if k=='inkling.attention.head_count_kv' else v) for k,t,v in kv])
            (tmp/'model.gguf').write_bytes((root/'model.gguf').read_bytes())
            bad={**meta['config'],'local_layer_ids':[1,3]};(tmp/'config.json').write_text(json.dumps(bad))
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode
            (tmp/'config.json').write_text(json.dumps(meta['config']));(tmp/'dense-int4g64').mkdir()
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode
        print(f'Inkling {kind}: malformed shapes, missing tensors and config mismatch rejected PASS',flush=True)
        if kind=='mixed':
            packed=root/'packed';run([sys.executable,engine/'tools/gguf_import.py',root/'model.gguf',packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/gguf-segment-oracle','inkling',packed,root/'oracle.json'],env),end='',flush=True)
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):(split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(ts))],ts[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            for key,value in json.loads(run(['build/test_gguf_inventory','--metadata',split],env)).items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','inkling',split,root/'oracle.json'],env),end='',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True);target=raw/'model.gguf'
                if not target.exists():os.link(root/'model.gguf',target)
                catalog=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],{**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalog)==1 and catalog[0]['sizing_verified'],catalog
                wrapper=next(raw.glob('.lumabri-gguf-*'));models=root/'home';models.mkdir(exist_ok=True);link=models/'inkling'
                if not link.exists():link.symlink_to(wrapper,target_is_directory=True)
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',models,'--resident-default','--crash-requester','--context','512'],env,600),end='',flush=True)
    native=dest/'safetensors';native.mkdir(exist_ok=True);native_fixture(native)
    for name in ('config.json','tokenizer.json'):(native/name).write_bytes((dest/'float'/name).read_bytes())
    run(['build/test_gguf_inventory','--cost',native],env)
    run(['build/inkling-upstream-oracle',native,native/'oracle.json'],env)
    for cut in range(1,4):run(['build/gguf-segment-oracle','inkling',native,native/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
    print('Inkling Safetensors regression: all cuts, 32 monolithic tokens PASS',flush=True)
    cmd=['build/inkling-mixed-oracle']
    for kind in ('float','Q8_0','mixed'):cmd.extend([dest/kind,dest/kind/'oracle.json'])
    print(run(cmd,env),end='',flush=True)
    print(run(cmd,{**env,'LUMABRI_RESIDENCY_POLICY':'engine-locked','LMB_ORACLE_ACCOUNTING':'1'}),end='',flush=True)
    print('Inkling locked allocation lifetime: stable after unloading all models PASS',flush=True)


if __name__=='__main__':main()
