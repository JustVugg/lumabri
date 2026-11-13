"""Emit native Qwen GGUF fixtures from the pinned upstream HF tiny models.

This is a test writer, independent from the C planner/loader. Payloads stay
in their GGUF representation for both the upstream and distributed oracle.
"""
import json
from pathlib import Path
import subprocess
import sys


def create(engine, root, family, quant):
    import numpy as np
    import torch
    from safetensors.torch import load_file
    from make_flint_tiny import _gguf_bytes, _q8_0
    from qwen_architectures_test import tokenizer
    geometries={'qwen3':'qwen3-8b-dense','qwen3moe':'qwen3-coder-30b',
                'qwen35':'qwen38-27b-dense','qwen35moe':'qwen36-35b'}
    source=root.parent/(family+'-source')
    if not (source/'config.json').exists():
        subprocess.run([sys.executable,str(engine/'tools/make_qwen36_tiny.py'),'--out',str(source),
                        '--geometry',geometries[family],'--ref-mode','full','--max-new','1',
                        '--emit-ref',str(source/'hf-reference.json')],check=True,
                       stdout=subprocess.DEVNULL,timeout=180)
    cfg=json.loads((source/'config.json').read_text());w=load_file(str(source/'model.safetensors'))
    if 'num_experts' not in cfg and 'num_local_experts' in cfg:cfg['num_experts']=cfg['num_local_experts']
    plain=family.startswith('qwen3') and not family.startswith('qwen35');moe=family.endswith('moe')
    fields={'block_count':cfg['num_hidden_layers'],'embedding_length':cfg['hidden_size'],
        'context_length':cfg['max_position_embeddings'],'attention.head_count':cfg['num_attention_heads'],
        'attention.head_count_kv':cfg['num_key_value_heads'],'attention.key_length':cfg['head_dim'],
        'attention.value_length':cfg['head_dim'],'feed_forward_length':cfg.get('intermediate_size',0)}
    if moe:fields.update(expert_count=cfg['num_experts'],expert_used_count=cfg['num_experts_per_tok'],
        expert_feed_forward_length=cfg['moe_intermediate_size'],
        expert_shared_feed_forward_length=cfg.get('shared_expert_intermediate_size',0))
    if not plain:
        fields.update({'ssm.group_count':cfg['linear_num_key_heads'],'ssm.time_step_rank':cfg['linear_num_value_heads'],
            'ssm.state_size':cfg['linear_key_head_dim'],'ssm.inner_size':cfg['linear_num_value_heads']*cfg['linear_value_head_dim'],
            'ssm.conv_kernel':cfg['linear_conv_kernel_dim'],'full_attention_interval':4})
    rp=cfg.get('rope_parameters',{})
    fields['rope.dimension_count']=int(cfg['head_dim']*rp.get('partial_rotary_factor',1 if plain else .25))
    tokenizer(source);tokens=json.loads((source/'tokenizer.json').read_text())['model']['vocab']
    vocab=[s for s,_ in sorted(tokens.items(),key=lambda pair:pair[1])]
    kv=[('general.architecture',8,family)]+[(family+'.'+k,4,v) for k,v in fields.items()]
    kv += [(family+'.rope.freq_base',6,rp.get('rope_theta',10000)),
        (family+'.attention.layer_norm_rms_epsilon',6,cfg['rms_norm_eps']),
        ('tokenizer.ggml.model',8,'gpt2'),('tokenizer.ggml.tokens',9,(8,vocab)),
        ('tokenizer.ggml.merges',9,(8,[])),('tokenizer.ggml.pre',8,'qwen2')]
    if family=='qwen3moe':kv.append(('tokenizer.chat_template',8,'interact with a computer to solve tasks'))
    aliases={'input_layernorm.weight':'attn_norm.weight',
        'post_attention_layernorm.weight':'ffn_norm.weight' if plain else 'post_attention_norm.weight',
        'self_attn.q_norm.weight':'attn_q_norm.weight','self_attn.k_norm.weight':'attn_k_norm.weight',
        'self_attn.q_proj.weight':'attn_q.weight','self_attn.k_proj.weight':'attn_k.weight',
        'self_attn.v_proj.weight':'attn_v.weight','self_attn.o_proj.weight':'attn_output.weight',
        'mlp.gate.weight':'ffn_gate_inp.weight','mlp.shared_expert_gate.weight':'ffn_gate_inp_shexp.weight',
        'linear_attn.in_proj_qkv.weight':'attn_qkv.weight','linear_attn.in_proj_z.weight':'attn_gate.weight',
        'linear_attn.in_proj_a.weight':'ssm_alpha.weight','linear_attn.in_proj_b.weight':'ssm_beta.weight',
        'linear_attn.conv1d.weight':'ssm_conv1d.weight','linear_attn.dt_bias':'ssm_dt.bias',
        'linear_attn.A_log':'ssm_a','linear_attn.norm.weight':'ssm_norm.weight','linear_attn.out_proj.weight':'ssm_out.weight'}
    for src,dst in (('gate','gate'),('up','up'),('down','down')):
        aliases[f'mlp.{src}_proj.weight']=f'ffn_{dst}.weight'
        aliases[f'mlp.shared_expert.{src}_proj.weight']=f'ffn_{dst}_shexp.weight'
    tensors=[]

    def put(name,t):
        a=t.float().contiguous().numpy().astype(np.float32)
        # Vectors and narrow DN matrices use float; all other matrices can
        # be Q8_0, including routed experts and the output head.
        q=quant and a.ndim>=2 and a.shape[-1]%32==0 and 'ssm_conv' not in name
        tensors.append((name,8 if q else 0,list(reversed(a.shape)),_q8_0(a) if q else a.tobytes()))

    for src,dst in [('model.embed_tokens.weight','token_embd.weight'),('lm_head.weight','output.weight'),('model.norm.weight','output_norm.weight')]:
        t=w[src];put(dst,t+1 if not plain and src=='model.norm.weight' else t)
    for l in range(cfg['num_hidden_layers']):
        prefix=f'model.layers.{l}.'
        for key,t in w.items():
            if not key.startswith(prefix) or '.experts.' in key:continue
            suffix=key[len(prefix):];name=aliases[suffix]
            if not plain and suffix in ('input_layernorm.weight','post_attention_layernorm.weight','self_attn.q_norm.weight','self_attn.k_norm.weight'):t=t+1
            if suffix=='linear_attn.A_log':t=-torch.exp(t)
            put(f'blk.{l}.{name}',t.squeeze(1) if suffix=='linear_attn.conv1d.weight' else t)
        if moe:
            for part in ('gate','up','down'):
                if f'{prefix}mlp.experts.0.{part}_proj.weight' in w:
                    t=torch.stack([w[f'{prefix}mlp.experts.{e}.{part}_proj.weight'] for e in range(cfg['num_experts'])])
                elif part=='down':t=w[f'{prefix}mlp.experts.down_proj']
                else:
                    both=w[f'{prefix}mlp.experts.gate_up_proj'];inter=cfg['moe_intermediate_size']
                    t=both[:,:inter] if part=='gate' else both[:,inter:]
                put(f'blk.{l}.ffn_{part}_exps.weight',t)
    (root/'model.gguf').write_bytes(_gguf_bytes(kv,tensors))
