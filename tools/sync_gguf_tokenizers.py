#!/usr/bin/env python3
"""Generate tokenizer declarations from the pinned importer, not Python runtime code."""
import argparse
import json
from pathlib import Path
import runpy
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--engine',required=True,type=Path)
p.add_argument('--check',action='store_true')
a=p.parse_args()
upstream=runpy.run_path(str(a.engine/'tools/gguf_import.py'))
rows=[]
for name in ['',*sorted(upstream['PRE_SPLITS'])]:
    value=json.dumps(upstream['pre_tokenizer'](name),ensure_ascii=False,separators=(',',':'))
    model=json.dumps(upstream['PRE_MODEL'].get(name,{}),separators=(',',':'))
    rows.append('    {%s,%s,%s},' % (json.dumps(name),json.dumps(value,ensure_ascii=False),json.dumps(model)))
text='''/* Generated tokenizer declarations from Colibri 2.1.0 tools/gguf_import.py.
 * Apache-2.0; attribution and modifications: docs/THIRD_PARTY.md.
 * This file contains data only. The installed importer is C. */
#ifndef LMB_GGUF_TOKENIZERS_DATA_H
#define LMB_GGUF_TOKENIZERS_DATA_H
typedef struct { const char *name,*pre,*model; } LmbGgufTokenizer;
static const LmbGgufTokenizer LMB_GGUF_TOKENIZERS[] = {
'''+ '\n'.join(rows)+'\n};\n#endif\n'
target=Path(__file__).resolve().parents[1]/'src/planner/lumabri_gguf_tokenizers_data.h'
if a.check:
    if not target.is_file() or target.read_text()!=text:sys.exit('GGUF tokenizer declarations changed: regenerate and verify token parity')
else:target.write_text(text)
