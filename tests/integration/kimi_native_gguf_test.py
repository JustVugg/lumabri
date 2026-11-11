#!/usr/bin/env python3
"""Resident Kimi K3 GGUF against the unmodified monolithic CPU engine."""
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
    import make_kimi_k3_tiny as f
    # Use upstream's tensor generator with GGUF's shared KDA/MLA head count.
    f.KDA_HEADS=f.HEADS;f.KDA_HD=32;f.QK_NOPE=32;f.QK_ROPE=32;f.V_HEAD=32
    rng=np.random.default_rng(20261112)
    source=f.build_tensors(rng);tensors=[]
    def add(name,a,matrix=False,expert=False):
        a=np.ascontiguousarray(a,dtype='<f4');typ=0;data=a.tobytes()
        if expert or (matrix and kind!='float-dense' and a.shape[-1]%32==0):
            typ=8;data=_q8_0(a)
        if kind=='mixed' and matrix and not expert and name.startswith('blk.3.'):
            typ=1;data=a.astype('<f2').tobytes()
        tensors.append((name,typ,list(reversed(a.shape)),data))
    add('token_embd.weight',source['model.embed_tokens.weight'])
    add('output_norm.weight',source['model.norm.weight'])
    add('output.weight',source['lm_head.weight'],matrix=True)
    add('output_res_score.weight',(source['model.output_attn_res_norm.weight']*source['model.output_attn_res_proj.weight']).reshape(-1))
    for l in range(f.LAYERS):
        p=f'model.layers.{l}.'
        def w(name,field,matrix=False,shape=None):
            a=source[p+field];add(f'blk.{l}.{name}',a.reshape(shape) if shape else a,matrix)
        w('attn_norm.weight','input_layernorm.weight');w('ffn_norm.weight','post_attention_layernorm.weight')
        for gg,hf in [('attn','self_attention'),('ffn','mlp')]:
            add(f'blk.{l}.{gg}_res_score.weight',(source[p+hf+'_res_norm.weight']*source[p+hf+'_res_proj.weight']).reshape(-1))
        if l+1 in f.KDA_LAYERS:
            for gg,hf in [('attn_q','q_proj'),('attn_k','k_proj'),('attn_v','v_proj'),('ssm_g','g_proj'),('attn_output','o_proj')]:
                w(gg+'.weight','self_attn.'+hf+'.weight',True)
            for n in ('q','k','v'):w('ssm_conv1d_'+n+'.weight','self_attn.'+n+'_conv1d.weight',shape=(f.KDA_HEADS*f.KDA_HD,f.CONV_K))
            for gg,hf in [('ssm_f_a.weight','f_a_proj.weight'),('ssm_f_b.weight','f_b_proj.weight'),('ssm_beta.weight','b_proj.weight'),
                          ('ssm_dt.bias','dt_bias'),('ssm_norm.weight','o_norm.weight')]:w(gg,'self_attn.'+hf)
            add(f'blk.{l}.ssm_a',-np.exp(source[p+'self_attn.A_log'][:f.KDA_HEADS]))
        else:
            for gg,hf in [('attn_q_a','q_a_proj'),('attn_q_b','q_b_proj'),('attn_kv_a_mqa','kv_a_proj_with_mqa'),
                          ('attn_output','o_proj'),('attn_gate','g_proj')]:w(gg+'.weight','self_attn.'+hf+'.weight',True)
            w('attn_q_a_norm.weight','self_attn.q_a_layernorm.weight');w('attn_kv_a_norm.weight','self_attn.kv_a_layernorm.weight')
            kv=source[p+'self_attn.kv_b_proj.weight'].reshape(f.HEADS,f.QK_NOPE+f.V_HEAD,f.KV_RANK)
            add(f'blk.{l}.attn_k_b.weight',kv[:,:f.QK_NOPE,:].transpose(0,2,1),matrix=True)
            add(f'blk.{l}.attn_v_b.weight',kv[:,f.QK_NOPE:,:],matrix=True)
        if l<f.FIRST_DENSE:
            for gg,hf in [('gate','gate_proj'),('up','up_proj'),('down','down_proj')]:w('ffn_'+gg+'.weight','mlp.'+hf+'.weight',True)
        else:
            for gg,hf in [('ffn_gate_inp.weight','gate.weight'),('exp_probs_b.bias','gate.e_score_correction_bias'),
                          ('ffn_routed_norm.weight','routed_expert_norm.weight')]:w(gg,'block_sparse_moe.'+hf)
            for gg,hf in [('ffn_routed_down','routed_expert_down_proj'),('ffn_routed_up','routed_expert_up_proj')]:w(gg+'.weight','block_sparse_moe.'+hf+'.weight',True)
            for gg,hf in [('gate','gate_proj'),('up','up_proj'),('down','down_proj')]:
                w('ffn_'+gg+'_shexp.weight','block_sparse_moe.shared_experts.'+hf+'.weight',True)
                shape=(f.EXPERTS,f.LATENT,f.MOE_INTER) if gg=='down' else (f.EXPERTS,f.MOE_INTER,f.LATENT)
                a=rng.normal(0,.04,size=shape).astype('<f4');add(f'blk.{l}.ffn_{gg}_exps.weight',a,expert=True)
    kv=[('general.architecture',8,'kimi-k3'),('tokenizer.ggml.model',8,'gpt2'),('tokenizer.ggml.tokens',9,(8,[])),
        ('tokenizer.ggml.merges',9,(8,[])),('tokenizer.ggml.bos_token_id',4,0),('tokenizer.ggml.eos_token_id',4,1)]
    integers={'block_count':f.LAYERS,'embedding_length':f.HIDDEN,'attention.head_count':f.HEADS,
        'attention.q_lora_rank':f.Q_RANK,'attention.kv_lora_rank':f.KV_RANK,'rope.dimension_count':f.QK_ROPE,
        'attention.key_length_mla':f.QK_NOPE+f.QK_ROPE,'attention.value_length_mla':f.V_HEAD,'expert_count':f.EXPERTS,
        'expert_used_count':f.TOPK,'expert_feed_forward_length':f.MOE_INTER,'expert_latent_length':f.LATENT,
        'feed_forward_length':f.DENSE_INTER,'expert_shared_count':f.SHARED,'leading_dense_block_count':f.FIRST_DENSE,
        'attn_res.block_size':f.RES_BLOCK,'kda.head_dim':f.KDA_HD,'ssm.conv_kernel':f.CONV_K,'context_length':512}
    kv += [('kimi-k3.'+k,4,v) for k,v in integers.items()]
    kv += [('kimi-k3.attention.head_count_kv',9,(4,[int(l+1 not in f.KDA_LAYERS) for l in range(f.LAYERS)]))]
    kv += [('kimi-k3.'+k,6,v) for k,v in {'activation.situ_beta':1.,'activation.situ_linear_beta':1.,'attention.layer_norm_rms_epsilon':1e-5}.items()]
    return byte_tokens(kv),tensors


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',required=True,type=Path)
    p.add_argument('--household',action='store_true');args=p.parse_args();engine=args.engine.resolve();sys.path.insert(0,str(engine/'tools'))
    from make_flint_tiny import _gguf_bytes
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','LUMABRI_RESIDENT_REQUIRED':'1',
        'LUMABRI_ENGINE_BACKEND':'cpu','K3_BITS':'32','K3_MLA_BITS':'32','K3_HEAD_BITS':'32','K3_PIPE':'0','K3_DIRECT':'0','K3_EXPERT_GB':'.05'}
    dest=Path('build/kimi-native').resolve();dest.mkdir(parents=True,exist_ok=True)
    for kind in ('float-dense','Q8_0','mixed'):
        root=dest/kind;root.mkdir(exist_ok=True);kv,ts=fixture(kind)
        (root/'model.gguf').write_bytes(_gguf_bytes(kv,ts));(root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        meta=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in meta.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==21
        env['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',root],env).strip()
        run(['build/kimi-upstream-oracle',root,root/'oracle.json'],env)
        for cut in range(1,6):run(['build/gguf-segment-oracle','kimi',root,root/'oracle.json'],{**env,'LMB_ORACLE_CUT':str(cut)})
        print(f'Kimi {kind}: all cuts, 32 upstream tokens, sealed weight input PASS',flush=True)
        print(run(['build/kimi-state-oracle',root],env),end='',flush=True)
        with tempfile.TemporaryDirectory(prefix='lmb-kimi-contract-') as temp:
            tmp=Path(temp);(tmp/'config.json').write_text(json.dumps(meta['config']));(tmp/'gguf.json').write_text((root/'gguf.json').read_text())
            def denied(tensors,metadata=kv):
                (tmp/'model.gguf').write_bytes(_gguf_bytes(metadata,tensors))
                r=subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True)
                assert r.returncode!=0,'invalid Kimi checkpoint admitted'
            for name in ('output_res_score.weight','blk.0.ssm_f_a.weight','blk.1.attn_k_b.weight','blk.2.ffn_routed_up.weight'):
                denied([t for t in ts if t[0]!=name])
            denied(ts+[('blk.1.unknown.weight',0,[1],b'\0'*4)])
            denied([(n,t,list(reversed(d)),b) if n=='blk.0.ssm_f_a.weight' else (n,t,d,b) for n,t,d,b in ts])
            denied(ts,kv+[('kimi-k3.expert_weights_scale',6,2.)])
            (tmp/'model.gguf').write_bytes((root/'model.gguf').read_bytes())
            bad=json.loads(json.dumps(meta['config']));bad['text_config']['attn_res_block_size']=3
            (tmp/'config.json').write_text(json.dumps(bad))
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode
        print(f'Kimi {kind}: malformed shapes, missing tensors and config mismatch rejected PASS',flush=True)
        if kind=='Q8_0':
            packed=root/'packed';run([sys.executable,engine/'tools/gguf_import.py',root/'model.gguf',packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/gguf-segment-oracle','kimi',packed,root/'oracle.json'],env),end='',flush=True)
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):(split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(_gguf_bytes(kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(ts))],ts[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            for key,value in json.loads(run(['build/test_gguf_inventory','--metadata',split],env)).items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/gguf-segment-oracle','kimi',split,root/'oracle.json'],env),end='',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True);target=raw/'model.gguf'
                if not target.exists():os.link(root/'model.gguf',target)
                catalog=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','512','--json'],{**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalog)==1 and catalog[0]['sizing_verified'],catalog
                wrapper=next(raw.glob('.lumabri-gguf-*'));models=root/'home';models.mkdir(exist_ok=True);link=models/'kimi'
                if not link.exists():link.symlink_to(wrapper,target_is_directory=True)
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',models,'--resident-default','--crash-requester','--context','512'],env,600),end='',flush=True)
    cmd=['build/kimi-mixed-oracle']
    for kind in ('float-dense','Q8_0','mixed'):cmd.extend([dest/kind,dest/kind/'oracle.json'])
    print(run(cmd,env),end='',flush=True)
    for bits in ('4','8'):
        root=dest/'float-dense';num={**env,'K3_BITS':bits,'K3_MLA_BITS':bits,'K3_HEAD_BITS':bits};ref=root/f'oracle-{bits}.json'
        run(['build/kimi-upstream-oracle',root,ref],num)
        for cut in range(1,6):run(['build/gguf-segment-oracle','kimi',root,ref],{**num,'LMB_ORACLE_CUT':str(cut)})
        print(f'Kimi dense bits {bits}: all cuts match monolithic tokens PASS',flush=True)
    # The new owned embedding and resident budget also cover existing MXFP4
    # checkpoints. Keep their independent vendor reference as a regression.
    native=dest/'mxfp4'
    run([sys.executable,engine/'tools/make_kimi_k3_tiny.py','--output',native,'--force'],env)
    (native/'tokenizer.json').write_text((dest/'Q8_0/tokenizer.json').read_text())
    ref=json.loads((engine/'kimi_k3_tiny/ref.json').read_text());case=ref['cases']['short']
    (native/'oracle.json').write_text(json.dumps({'prompt_ids':case['prompt_ids'],'full_ids':case['greedy_full_ids']}))
    numeric={**env,**{k:str(v) for k,v in ref['engine_env'].items()}}
    numeric['LMB_ORACLE_WIDTH']=run(['build/test_gguf_inventory','--boundary',native],numeric).strip()
    run(['build/test_gguf_inventory','--cost',native],numeric)
    for cut in range(1,6):run(['build/gguf-segment-oracle','kimi',native,native/'oracle.json'],{**numeric,'LMB_ORACLE_CUT':str(cut)})
    print('Kimi MXFP4: all cuts match independent vendor reference PASS',flush=True)


if __name__=='__main__':main()
