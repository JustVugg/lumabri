"""Small GGUF written independently of the runtime/parser under test.

Native Q8_0 matrices include quantized Edge weights; a second mode mixes
F16 and Q8_0 experts between layers to exercise resident slot padding.
"""
import json
from pathlib import Path
import random
import struct


def string(v):
    b = v.encode()
    return struct.pack('<Q', len(b)) + b


def create(root, mixed=False, extra_metadata=(), byte_vocab=False, split=False, layers=2, block_layout=False, inter=32):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    rng = random.Random(251057)
    h, experts, vocab = 32, 2, 256 if byte_vocab else 32
    assert 2 <= layers <= 4
    assert not (block_layout and split)
    metadata = [('general.architecture', 8, string('olmoe')),
                ('general.alignment', 4, struct.pack('<I', 32))]
    cfg = dict(model_type='olmoe', hidden_size=h, intermediate_size=inter,
               num_hidden_layers=layers, num_attention_heads=2, num_key_value_heads=2,
               num_experts=experts, num_experts_per_tok=2, vocab_size=vocab,
               rms_norm_eps=1e-5, rope_theta=10000, max_position_embeddings=128,
               norm_topk_prob=False)
    fields = {'embedding_length': h, 'block_count': layers, 'attention.head_count': 2,
              'attention.head_count_kv': 2, 'expert_count': experts,
              'expert_used_count': 2, 'feed_forward_length': inter, 'context_length': 128}
    metadata += [('olmoe.'+k, 4, struct.pack('<I', v)) for k, v in fields.items()]
    tokens = [chr(65+i) for i in range(vocab)]
    if byte_vocab:
        # GPT-2's reversible byte alphabet, including whitespace and controls.
        printable=list(range(33,127))+list(range(161,173))+list(range(174,256))
        mapped={b:b for b in printable}; extra=256
        for b in range(256):
            if b not in mapped: mapped[b]=extra; extra+=1
        tokens=[chr(mapped[b]) for b in range(256)]
    metadata += [('tokenizer.ggml.tokens', 9, struct.pack('<IQ', 8, vocab)+b''.join(map(string, tokens))),
                 ('tokenizer.ggml.model', 8, string('gpt2'))]
    override={k:(t,v) for k,t,v in extra_metadata}
    metadata=[(k,*override.pop(k,(t,v))) for k,t,v in metadata]
    metadata += [(k,t,v) for k,(t,v) in override.items()]
    tensors = []

    def add(name, shape, kind=8):
        cells = 1
        for v in shape:
            cells *= v
        if kind == 0:
            data = struct.pack('<'+'f'*cells, *[1+rng.uniform(-0.2, 0.2) for _ in range(cells)])
        elif kind == 1:
            data = struct.pack('<'+'e'*cells, *[rng.uniform(-0.15, 0.15) for _ in range(cells)])
        else:
            assert cells % 32 == 0
            data = b''.join(struct.pack('<e32b', 0.0015, *[rng.randint(-100, 100) for _ in range(32)])
                            for _ in range(cells//32))
        tensors.append((name, shape, kind, data))

    add('token_embd.weight', [h, vocab])
    add('output.weight', [h, vocab])
    add('output_norm.weight', [h], 0)
    for layer in range(layers):
        prefix = f'blk.{layer}.'
        for name in ['attn_norm', 'ffn_norm', 'attn_q_norm', 'attn_k_norm']:
            add(prefix+name+'.weight', [h], 0)
        for name in ['attn_q', 'attn_k', 'attn_v', 'attn_output']:
            add(prefix+name+'.weight', [h, h])
        add(prefix+'ffn_gate_inp.weight', [h, experts])
        for name in ['ffn_gate_exps', 'ffn_up_exps', 'ffn_down_exps']:
            shape=[inter,h,experts] if name=='ffn_down_exps' else [h,inter,experts]
            add(prefix+name+'.weight', shape, 1 if mixed and layer == 1 else 8)

    parts=[tensors] if not split else [tensors[::2],tensors[1::2]]
    filenames=[]
    for part,group in enumerate(parts):
        kv=list(metadata) if not part else metadata[:2]
        if split:
            kv += [(k,4,struct.pack('<I',v)) for k,v in
                   [('split.no',part),('split.count',len(parts)),('split.tensors.count',len(tensors))]]
        header = b'GGUF'+struct.pack('<IQQ', 3, len(group), len(kv))
        header += b''.join(string(k)+struct.pack('<I', t)+v for k, t, v in kv)
        payload = b''
        for name, shape, kind, data in group:
            header += string(name)+struct.pack('<I', len(shape))+struct.pack('<'+'Q'*len(shape), *shape)
            header += struct.pack('<IQ', kind, len(payload))
            payload += data+b'\0'*((-len(data)) % 32)
        filename=f'weights-{part+1:05}-of-{len(parts):05}.gguf' if split else 'weights.gguf'
        filenames.append(filename)
        if block_layout:
            # Put Edge and each layer in its own 8 MiB CAS block. Gaps are
            # sparse: this tests selective transfer without large weights.
            data_start=(len(header)+31)//32*32
            header=b'GGUF'+struct.pack('<IQQ',3,len(group),len(kv))
            header+=b''.join(string(k)+struct.pack('<I',t)+v for k,t,v in kv)
            used={};writes=[]
            for name,shape,kind,data in group:
                bucket=int(name.split('.')[1])+2 if name.startswith('blk.') else 1
                at=bucket*(8<<20)+used.get(bucket,0)
                used[bucket]=used.get(bucket,0)+(len(data)+31)//32*32
                header+=string(name)+struct.pack('<I',len(shape))+struct.pack('<'+'Q'*len(shape),*shape)
                header+=struct.pack('<IQ',kind,at-data_start)
                writes.append((at,data))
            with (root/filename).open('wb') as f:
                f.write(header+b'\0'*(data_start-len(header)))
                for at,data in writes:f.seek(at);f.write(data)
        else:
            (root/filename).write_bytes(header+b'\0'*((-len(header)) % 32)+payload)
    (root/'config.json').write_text(json.dumps(cfg))
    (root/'gguf.json').write_text(json.dumps({'format': 'colibri-gguf-v1', 'gguf': filenames[0]}))
    (root/'tokenizer.json').write_text(json.dumps({'model': {'type': 'BPE', 'vocab': dict(zip(tokens, range(vocab))), 'merges': []}}))
    return root


if __name__ == '__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output');p.add_argument('--mixed',action='store_true')
    p.add_argument('--byte-vocab',action='store_true');p.add_argument('--split',action='store_true')
    p.add_argument('--layers',type=int,choices=(2,3,4),default=2)
    p.add_argument('--block-layout',action='store_true')
    a=p.parse_args()
    create(a.output,a.mixed,byte_vocab=a.byte_vocab,split=a.split,layers=a.layers,block_layout=a.block_layout)
