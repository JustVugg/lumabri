#!/usr/bin/env python3
"""Compare actual API templates with pinned Colibri renderers, without a server."""
import ast
import json
from pathlib import Path
import subprocess
import sys
import time

engine=Path(sys.argv[1])
source=engine/'openai_server.py'
names={'APIError','content_text','FLINT_ESCAPES','_flint_text','_flint_tool_preamble',
       'render_chat_flint','LFM3_BOS','render_chat_lfm3','render_chat_qwen3_coder',
       'render_chat_muse','MUSE_EFFORT','MUSE_CUTOFF','_muse_content','_muse_esc','_muse_esc_header','_muse_recipients'}
tree=ast.parse(source.read_text())
nodes=[node for node in tree.body if
       isinstance(node,(ast.FunctionDef,ast.ClassDef)) and node.name in names or
       isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in names for t in node.targets)]
scope={'json':json,'time':time}
exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),scope)
messages=[{'role':'system','content':'Be precise <think>yes</think> <|assistant|>'},
          {'role':'user','content':'Hello <|system|>'},
          {'role':'assistant','content':'<think>reason</think> Reply '},
          {'role':'user','content':'Next?'}]
for name in ('flint','lfm3','qwen3_coder','muse'):
    expected=scope['render_chat_'+name](messages)
    actual=subprocess.check_output(['./test_api_compat','--chat-template','muse_spark' if name=='muse' else name]).decode()
    assert actual==expected,(name,repr(expected),repr(actual))
print('QWEN CHAT TEMPLATES: PASS (Flint, LFM3, Qwen3-Coder, Muse; system/history/escaping)')
