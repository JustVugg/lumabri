#!/usr/bin/env python3
"""Snapshot container block geometry from the pinned engine, without kernels."""
import argparse
from pathlib import Path
import re
import sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--engine',type=Path,required=True)
p.add_argument('--check',action='store_true')
a=p.parse_args()
s=(a.engine/'ggml_quants.h').read_text()
enum=s.split('/* ggml type ids',1)[1].split('};',1)[0]
ids={name:int(n) for name,n in re.findall(r'(GQ_\w+)\s*=\s*(\d+)',enum)}
body=s.split('static const gq_type_info GQ_TYPES',1)[1].split('};',1)[0]
base=re.search(r'int8_t base\[\]\s*=\s*\{([^}]+)',s).group(1)
sizes=re.search(r'int16_t gb\[\]\s*=\s*\{([^}]+)',s).group(1)
packed_names=re.findall(r'GQ_\w+',base)
packed_sizes=list(map(int,re.findall(r'\d+',sizes)))
assert packed_names and len(set(packed_names))==len(packed_names)==len(packed_sizes)
packed=dict(zip(packed_names,packed_sizes))
rows=[]
for key,name,block,size,ok in re.findall(r'\[(GQ_\w+)\]\s*=\s*\{\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',body):
    rows.append(f'    [{ids[key]}] = {{"{name}",{block},{size},{ok},{packed.get(key,0)}}},')
assert rows and len(rows)==len(re.findall(r'\[GQ_',body))-1
out=('/* Generated container geometry from Colibri ggml_quants.h; no execution kernels. */\n'
 '#ifndef LMB_GGUF_TYPES_H\n#define LMB_GGUF_TYPES_H\n'
 'typedef struct { const char *name; unsigned block, bytes, decodable, rows16_bytes; } LmbGgufType;\n'
 f'static const LmbGgufType LMB_GGUF_TYPES[{ids["GQ_TYPE_MAX"]}] = {{\n'+ '\n'.join(rows)+'\n};\n#endif\n')
path=Path(__file__).resolve().parents[1]/'planner_adapters/gguf_types.h'
if a.check:
    if not path.is_file() or path.read_text()!=out:
        sys.exit('GGUF block geometry changed: regenerate and revalidate resident memory contracts')
else:path.write_text(out)
