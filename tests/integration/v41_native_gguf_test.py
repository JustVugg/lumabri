#!/usr/bin/env python3
"""V4.1 native GGUF: exact shared-state inference and resident Engram rows."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import copy
from qwen_native_gguf_test import byte_tokens,run
from qwen38_native_gguf_test import gguf_bytes


def fixture(kind,global_table):
    import numpy as np
    import gguf
    rng=np.random.default_rng(20261112);H=128;HC=4;E=8;I=64;D=64;Q=64;ID=32
    cfg={'model_type':'deepseek_v41','hidden_size':H,'num_hidden_layers':6,'num_attention_heads':4,
         'head_dim':D,'qk_rope_head_dim':16,'q_lora_rank':Q,'o_lora_rank':32,'o_groups':2,
         'moe_intermediate_size':I,'n_routed_experts':E,'num_experts_per_tok':2,'n_shared_experts':1,
         'vocab_size':320,'max_position_embeddings':256,'sliding_window':8,'index_n_heads':4,
         'index_head_dim':ID,'index_topk':4,'candidate_source_layer':3,'candidate_topk_blocks':2,
         'candidate_block_size':2,'hc_mult':HC,'hc_sinkhorn_iters':20,'norm_eps':1e-6,'hc_eps':1e-6,
         'route_scale':1.5,'gate_temp':1,'rope_theta':10000,'compress_rope_theta':40000,
         'rope_factor':16,'original_seq_len':32,'beta_fast':32,'beta_slow':1,'norm_topk_prob':1,
         'swiglu_limit':10,'compress_ratios':[0,2,2,1,1,0],'kv_source_layers':[1,3],
         'index_source_layers':[1,3,4],'engram_layer_ids':[1,4],'engram_max_ngram_size':4,
         'engram_n_heads':2,'engram_head_dim':32,'engram_pad_token_id':2}
    tensors=[]
    def weight(name,shape,matrix=False,norm=False):
        a=rng.normal(0,.025,size=shape).astype('<f4')
        if norm:a+=1
        qt=0
        if matrix and kind!='float':qt=8 if kind=='Q8_0' else (2 if '_exps.' in name else 30 if len(tensors)%3 else 8)
        raw=gguf.quants.quantize(a,gguf.GGMLQuantizationType(qt)).tobytes() if qt else a.tobytes()
        tensors.append((name,qt,list(reversed(shape)),raw))
    weight('token_embd.weight',(320,H),True);weight('output.weight',(320,H),True)
    weight('output_norm.weight',(H,),norm=True)
    for l in range(6):
        p=f'blk.{l}.'
        for stem in ('hc_attn','hc_ffn'):
            weight(p+stem+'_fn.weight',((HC+2)*HC,HC*H));weight(p+stem+'_base.weight',((HC+2)*HC,));weight(p+stem+'_scale.weight',(3,))
        for name,shape in [('attn_q_a',(Q,H)),('attn_q_b',(4*D,Q)),('attn_kv',(D,H)),('attn_output_a',(64,128)),('attn_output_b',(H,64))]:weight(p+name+'.weight',shape,True)
        for name,size in [('attn_q_a_norm',Q),('attn_kv_a_norm',D),('attn_norm',H),('ffn_norm',H)]:weight(p+name+'.weight',(size,),norm=True)
        weight(p+'attn_sinks.weight',(4,));weight(p+'ffn_gate_inp.weight',(E,H),True);weight(p+'exp_probs_b.bias',(E,))
        for name,shape in [('gate',(I,H)),('up',(I,H)),('down',(H,I))]:
            weight(p+f'ffn_{name}_shexp.weight',shape,True);weight(p+f'ffn_{name}_exps.weight',(E,*shape),True)
        if l in cfg['kv_source_layers']:
            weight(p+'attn_compressor_kv.weight',(D,H),True);weight(p+'attn_compressor_norm.weight',(D,),norm=True)
            if cfg['compress_ratios'][l]>1:weight(p+'attn_compressor_gate.weight',(D,H),True)
            weight(p+'indexer.attn_k.weight',(ID,D),True);weight(p+'indexer.k_norm.weight',(ID,),norm=True)
        if l in cfg['index_source_layers']:
            weight(p+'indexer.attn_q_b.weight',(4*ID,Q),True);weight(p+'indexer.proj.weight',(4,H),True)
        if l in cfg['engram_layer_ids']:
            weight(p+'engram_kv.weight',(H*(HC+1),6*32),True)
            if global_table:weight(p+'engram_gate.weight',(HC,H),norm=True)
            else:
                weight(p+'engram_q_norm.weight',(HC,H),norm=True);weight(p+'engram_k_norm.weight',(HC,H),norm=True)
    sizes=[17,19,23,29,31,37,41,43,47,53,59,61];rows=[sum(sizes[:6]),sum(sizes[6:])]
    offsets=[];end=0
    for sz in sizes:offsets.append(end);end+=sz
    if global_table:
        a=rng.normal(0,.025,size=(end,32)).astype('<f4')
        tensors.append(('per_layer_token_embd.weight',8,[32,end],gguf.quants.quantize(a,gguf.GGMLQuantizationType.Q8_0).tobytes()))
    else:
        for l,n in zip(cfg['engram_layer_ids'],rows):
            table=np.empty((n,33),dtype='u1');table[:,:32]=rng.choice([0x38,0xb8],size=(n,32));table[:,32]=122
            tensors.append((f'blk.{l}.engram_embd.weight',24,[33,n],table.tobytes()))
    meta=[('general.architecture',8,'deepseek41'),('tokenizer.ggml.model',8,'gpt2'),
          ('tokenizer.ggml.tokens',9,(8,[])),('tokenizer.ggml.bos_token_id',4,0),('tokenizer.ggml.eos_token_id',4,1),
          ('deepseek41.config',8,json.dumps(cfg))]
    # Multipliers deliberately exceed double's exact integer range.
    mult=[118650432209603643,7493640653728489,55676810147952557,114555519428547269,
          127364524771245009,7800076361716407,114038564133282483,29348051253447937]
    if global_table:
        data={'ple.head_vocab_sizes':sizes,'ple.head_offsets':offsets,'ple.layer_multipliers':mult}
    else:
        meta.append(('deepseek41.engram.encoding',8,'e4m3_e8m0_32_row264'))
        data={'engram.layer_ids':[1,4],'engram.primes':sizes,'engram.rows':rows,'engram.multipliers':mult}
    if global_table:
        data['ple.layers']=[1,4]
        meta.extend([('deepseek41.ple.ngram_size',4,4),('deepseek41.ple.heads_per_ngram',4,2),('deepseek41.embedding_length_per_layer_input',4,32)])
    data['engram.token_map']=[i%64 for i in range(320)]
    meta.extend(('deepseek41.'+k,9,(10,v)) for k,v in data.items())
    return byte_tokens(meta),tensors


def standard_keys(kv):
    cfg=json.loads(next(v for k,_,v in kv if k=='deepseek41.config'))
    kv=[(k,t,v) for k,t,v in kv if k!='deepseek41.config']
    fields={'block_count':'num_hidden_layers','embedding_length':'hidden_size','attention.head_count':'num_attention_heads',
        'attention.key_length':'head_dim','rope.dimension_count':'qk_rope_head_dim','attention.q_lora_rank':'q_lora_rank',
        'attention.output_lora_rank':'o_lora_rank','attention.output_group_count':'o_groups','expert_count':'n_routed_experts',
        'expert_used_count':'num_experts_per_tok','expert_shared_count':'n_shared_experts','expert_feed_forward_length':'moe_intermediate_size',
        'context_length':'max_position_embeddings','attention.sliding_window':'sliding_window',
        'attention.indexer.head_count':'index_n_heads','attention.indexer.key_length':'index_head_dim','attention.indexer.top_k':'index_topk',
        'attention.indexer.candidate_source_layer':'candidate_source_layer','attention.indexer.candidate_top_k_blocks':'candidate_topk_blocks',
        'attention.indexer.candidate_block_size':'candidate_block_size','hyper_connection.count':'hc_mult',
        'hyper_connection.sinkhorn_iterations':'hc_sinkhorn_iters','rope.scaling.original_context_length':'original_seq_len',
        'rope.scaling.yarn_beta_fast':'beta_fast','rope.scaling.yarn_beta_slow':'beta_slow'}
    kv.extend(('deepseek41.'+k,4,cfg[v]) for k,v in fields.items())
    floats={'attention.layer_norm_rms_epsilon':'norm_eps','hyper_connection.epsilon':'hc_eps','expert_weights_scale':'route_scale',
        'expert_gate_temperature':'gate_temp','rope.freq_base':'rope_theta','attention.compress_rope_freq_base':'compress_rope_theta',
        'rope.scaling.factor':'rope_factor'}
    kv.extend(('deepseek41.'+k,6,cfg[v]) for k,v in floats.items())
    arrays={'attention.compress_ratios':'compress_ratios','attention.kv_source_layers':'kv_source_layers','attention.indexer.source_layers':'index_source_layers'}
    kv.extend(('deepseek41.'+k,9,(4,cfg[v])) for k,v in arrays.items())
    kv.append(('deepseek41.swiglu_clamp_exp',9,(6,[10.])))
    return kv


def malformed(root,kv,ts,meta,env):
    with tempfile.TemporaryDirectory(prefix='lmb-v41-contract-') as tmp:
        tmp=Path(tmp)
        for file in ('config.json','tokenizer.json','dsv41_engram.json','gguf.json'):(tmp/file).write_bytes((root/file).read_bytes())
        def denied(tensors,metadata=kv):
            (tmp/'model.gguf').write_bytes(gguf_bytes(metadata,tensors))
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode,'invalid V4.1 admitted'
        for key in ('output.weight','blk.1.engram_kv.weight','blk.3.indexer.attn_k.weight','blk.5.ffn_down_exps.weight'):
            denied([t for t in ts if t[0]!=key])
        denied(ts+[('blk.0.unknown.weight',0,[1],b'\0'*4)])
        (tmp/'model.gguf').write_bytes((root/'model.gguf').read_bytes())
        for field in ('multipliers','token_map','offsets'):
            side=copy.deepcopy(meta['dsv41_engram'])
            if field=='multipliers':side[field][0][0]=str(int(side[field][0][0])+1)
            elif field=='token_map':side[field][3]=(side[field][3]+1)%64
            else:side[field][0][0]+=1
            (tmp/'dsv41_engram.json').write_text(json.dumps(side))
            assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode,field
        (tmp/'dsv41_engram.json').write_text(json.dumps(dict(reversed(list(meta['dsv41_engram'].items()))),indent=2))
        run(['build/test_gguf_inventory','--cost',tmp],env)
        (tmp/'config.json').write_text(json.dumps({**meta['config'],'rope_factor':999}))
        assert subprocess.run(['build/test_gguf_inventory','--cost',tmp],env=env,capture_output=True).returncode
    print('V4.1: missing tensors, modified hashes and wrong geometry refused PASS',flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',required=True,type=Path)
    p.add_argument('--prepare-only',action='store_true');p.add_argument('--household',action='store_true');args=p.parse_args()
    env={**os.environ,'OMP_NUM_THREADS':'1','CTX':'128','PIN':'off','V41_DRAFT':'0','GQ_EXACT':'1','LUMABRI_RESIDENT_REQUIRED':'1'}
    base=Path('build/v41-native').resolve();base.mkdir(parents=True,exist_ok=True)
    for kind,table in [('float',False),('Q8_0',False),('mixed',True),('mixed-keys',True),('plain',False)]:
        root=base/kind;root.mkdir(exist_ok=True);kv,ts=fixture('mixed' if kind=='mixed-keys' else kind,table)
        if kind=='mixed':
            cfg=json.loads(next(v for k,_,v in kv if k=='deepseek41.config'))
            cfg['rope_scaling']={'factor':8}
            cfg['dspark_block_size']=7
            kv=[(k,t,json.dumps(cfg) if k=='deepseek41.config' else v) for k,t,v in kv]
        if kind=='plain':
            cfg=json.loads(next(v for k,_,v in kv if k=='deepseek41.config'))
            cfg['engram_layer_ids']=[]
            cfg['norm_topk_prob']=False
            kv=[(k,t,json.dumps(cfg) if k=='deepseek41.config' else v) for k,t,v in kv if not k.startswith('deepseek41.engram.')]
            ts=[t for t in ts if '.engram_' not in t[0]]
        if kind=='mixed-keys':kv=standard_keys(kv)
        (root/'model.gguf').write_bytes(gguf_bytes(kv,ts));(root/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'model.gguf'}))
        meta=json.loads(run(['build/test_gguf_inventory','--metadata',root],env))
        for key,value in meta.items():(root/(key+'.json')).write_text(json.dumps(value))
        assert len(run(['build/test_gguf_inventory','--cost',root],env).splitlines())==21
        print(f'V4.1 {kind}: native inventory and Engram metadata PASS',flush=True)
        if kind!='plain':malformed(root,kv,ts,meta,env)
        if args.prepare_only:continue
        # Independent metadata too: the standalone reference consumes the
        # original importer output, not the C emitter's config/Engram sidecar.
        upstream=root/'upstream'
        run([sys.executable,args.engine/'tools/gguf_import.py',root/'model.gguf',upstream,'--mode','link','--no-mmproj','--no-mtp'],env)
        ref=root/'reference.bin';run(['build/v41-gguf-upstream-oracle',upstream,ref],env)
        print(run(['build/test_v41_abi',root],{**env,'LMB_V41_ORACLE':str(ref)},240),end='',flush=True)
        if kind=='Q8_0':
            packed=root/'packed';run([sys.executable,args.engine/'tools/gguf_import.py',root/'model.gguf',packed,'--mode','copy','--no-mmproj','--no-mtp'],env)
            run(['build/test_gguf_inventory','--cost',packed],env)
            print(run(['build/test_v41_abi',packed],{**env,'LMB_V41_ORACLE':str(ref)},240),end='',flush=True)
        if kind=='mixed-keys':
            split=root/'split';split.mkdir(exist_ok=True)
            for i in range(2):(split/f'weights-{i+1:05}-of-00002.gguf').write_bytes(gguf_bytes(kv+[('split.count',4,2),('split.no',4,i),('split.tensors.count',4,len(ts))],ts[i::2]))
            (split/'gguf.json').write_text(json.dumps({'format':'colibri-gguf-v1','gguf':'weights-00001-of-00002.gguf'}))
            for key,value in json.loads(run(['build/test_gguf_inventory','--metadata',split],env)).items():(split/(key+'.json')).write_text(json.dumps(value))
            run(['build/test_gguf_inventory','--cost',split],env)
            print(run(['build/test_v41_abi',split],{**env,'LMB_V41_ORACLE':str(ref)},240),end='',flush=True)
            if args.household:
                raw=root/'raw';raw.mkdir(exist_ok=True);target=raw/'model.gguf'
                if not target.exists():os.link(root/'model.gguf',target)
                catalog=json.loads(run(['./lumabri','models','--models-dir',raw,'--context','128','--json'],{**env,'LUMABRI_NO_DISK_PROBE':'1'}))['models']
                assert len(catalog)==1 and catalog[0]['sizing_verified'],catalog
                print(run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',raw,'--resident-default','--crash-requester','--context','128'],env,600),end='',flush=True)
    if not args.prepare_only:
        print(run(['build/test_v41_mixed_gguf',base/'float',base/'Q8_0',base/'mixed-keys'],env,240),end='',flush=True)
    print('V4.1 NATIVE GGUF: PASS',flush=True)


if __name__=='__main__':main()
