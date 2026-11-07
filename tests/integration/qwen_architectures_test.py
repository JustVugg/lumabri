#!/usr/bin/env python3
"""Converted Qwen, Flint and LFM3: native monolithic versus resident ranges.

Fixtures come from pinned Colibri generators and its real converter. The
oracle binary includes upstream qwen36.c, not Lumabri's modified copies.
No checkpoint downloads, hardware services or network access are involved.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from qwen_contract_test import check_contract


def run(args, env, timeout=180):
    result=subprocess.run(list(map(str,args)),env=env,stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT,text=True,timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{args}: exit {result.returncode}\n{result.stdout[-20000:]}")
    return result.stdout


def lfm_source(engine, output):
    # The upstream fixture exposes LFM3's weights in GGUF names. Emit the HF
    # names accepted by its converter; no arithmetic or quantization changes.
    import torch
    from safetensors.torch import save_file
    sys.path.insert(0,str(engine/'tools'))
    spec=importlib.util.spec_from_file_location('fixture_lfm',engine/'tools/make_lfm3_tiny.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    cfg=module.tiny_config(vocab=320)
    weights=module.random_weights(cfg,20261029)
    aliases={'attn_norm.weight':'operator_norm.weight','post_attention_norm.weight':'post_operator_norm.weight',
        'ffn_norm.weight':'ffn_norm.weight','post_ffw_norm.weight':'post_ffn_norm.weight',
        'ffn_gate.weight':'feed_forward.w1.weight','ffn_up.weight':'feed_forward.w3.weight',
        'ffn_down.weight':'feed_forward.w2.weight','shortconv.in_proj.weight':'conv.in_proj.weight',
        'shortconv.conv.weight':'conv.conv.weight','shortconv.out_proj.weight':'conv.out_proj.weight',
        'attn_q.weight':'self_attn.q_proj.weight','attn_k.weight':'self_attn.k_proj.weight',
        'attn_v.weight':'self_attn.v_proj.weight','attn_output.weight':'self_attn.o_proj.weight',
        'attn_q_norm.weight':'self_attn.q_norm.weight','attn_k_norm.weight':'self_attn.k_norm.weight',
        'attn_gate.weight':'self_attn.gate_proj.weight'}
    tensors={}
    for name,tensor in weights.items():
        if name=='token_embd.weight': key='model.embed_tokens.weight'
        elif name=='output_norm.weight': key='model.embedding_norm.weight'
        else:
            _,layer,field=name.split('.',2)
            key=f'model.layers.{layer}.{aliases[field]}'
        tensors[key]=tensor.contiguous()
    output.mkdir(parents=True,exist_ok=True)
    (output/'config.json').write_text(json.dumps(cfg))
    save_file(tensors,str(output/'model.safetensors'),metadata={'format':'pt'})


def tokenizer(root):
    cfg=json.loads((root/'config.json').read_text());vocab=cfg['vocab_size']
    assert vocab>=256
    # GPT-2's byte alphabet; avoids treating a code point as a multi-byte token.
    bs=list(range(33,127))+list(range(161,173))+list(range(174,256))
    chars=dict(zip(bs,bs));extra=256
    for b in range(256):
        if b not in chars: chars[b]=extra;extra+=1
    tokens={chr(chars[b]):b for b in range(256)}
    tokens.update({f'<fixture_{i}>':i for i in range(256,vocab)})
    (root/'tokenizer.json').write_text(json.dumps({'model':{'type':'BPE','vocab':tokens,'merges':[]},
        'added_tokens':[],'pre_tokenizer':{'type':'ByteLevel','add_prefix_space':False}},ensure_ascii=False))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--engine',type=Path,required=True)
    p.add_argument('--only',choices=('qwen3','coder','qwen35','qwen35moe','flint','lfm3'))
    p.add_argument('--household',action='store_true',help='also exercise real donor approvals and Hosted chat on loopback')
    args=p.parse_args();engine=args.engine.resolve()
    fixtures=Path('build/qwen-architectures').resolve();fixtures.mkdir(parents=True,exist_ok=True)
    env={**os.environ,'OMP_NUM_THREADS':'1','COLI_NO_OMP_TUNE':'1','PIN':'off','COLI_DENSE_I8':'0',
        'LUMABRI_RESIDENT_REQUIRED':'1','LUMABRI_ENGINE_BACKEND':'cpu','PILOT':'0','COLIBRI_RESIDENT':'0',
        'HF_HUB_OFFLINE':'1','TRANSFORMERS_OFFLINE':'1'}
    cases={'qwen3':'qwen3-8b-dense','coder':'qwen3-coder-30b','qwen35':'qwen38-27b-dense',
           'qwen35moe':'qwen36-35b','flint':None,'lfm3':None}
    for name,geometry in cases.items():
        if args.only and name!=args.only: continue
        source=fixtures/(name+'-source')
        if not (source/'config.json').exists():
            if geometry:
                run([sys.executable,engine/'tools/make_qwen36_tiny.py','--out',source,'--geometry',geometry,
                     '--ref-mode','full','--max-new','2','--emit-ref',source/'hf-ref.json'],env)
            elif name=='flint':
                run([sys.executable,engine/'tools/make_flint_tiny.py','--out',source,'--vocab','320'],env)
            else: lfm_source(engine,source)
        formats=[(8,0),(4,0),(4,8)] if name in ('coder','flint','qwen35moe') else [(8,0)]
        for bits,down in formats:
            label=f'{bits}-down{down}' if down else str(bits)
            root=fixtures/f'{name}-{label}'
            if not (root/'qwen36_meta.json').exists():
                run([sys.executable,engine/'tools/convert_qwen36.py','--model',source,'--out',root,
                     '--ebits',bits,'--gs','16' if bits==4 else '0','--down-bits',down,
                     '--down-gs','8' if down else '0','--no-readme'],env)
            tokenizer(root)
            if name=='coder':
                # Same distinguishing text used by the upstream template
                # detector; the tokenizer is a local byte fixture.
                (root/'chat_template.jinja').write_text('interact with a computer to solve tasks')
            cfg=json.loads((root/'config.json').read_text());layers=cfg['num_hidden_layers']
            budget=run([Path('segment_budget_probe').resolve(),root,0,layers,128,1],env).strip()
            oracle=root/'oracle.json'
            run([Path('build/qwen-upstream-oracle').resolve(),root,oracle],env)
            output=run([Path('build/gguf-segment-oracle').resolve(),'qwen36',root,oracle],env)
            assert output.count('independent oracle tokens PASS')==3,output
            run([Path('build/qwen-state-oracle').resolve(),'qwen36',root,0,layers//2,layers,128],env)
            check_contract(root,Path('segment_budget_probe').resolve())
            print(f'{name} int{bits}: resident budget {budget}; whole/split/fresh oracle and snapshots PASS',flush=True)
            if args.household and bits==8 and name in ('coder','qwen35','flint','lfm3'):
                home=fixtures/(name+'-home');home.mkdir(exist_ok=True)
                link=home/name
                if not link.exists(): link.symlink_to(root,target_is_directory=True)
                output=run([sys.executable,'tests/integration/home_flow_test.py','--models-dir',home,
                            '--resident-default','--context','512'],env,timeout=300)
                assert 'HOME RESIDENT PASS' in output and 'HOME FLOW: PASS' in output,output
                print(f'{name}: donor approvals, resident TUI and source-offline second chat PASS',flush=True)
    print('QWEN ARCHITECTURES: PASS')


if __name__=='__main__':main()
