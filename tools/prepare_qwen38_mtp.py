#!/usr/bin/env python3
"""The Qwen3.8 MTP draft head in the disposable Edge adapter source.

Never modifies the upstream checkout. Exact anchors fail closed on drift.
The head's arithmetic is upstream's; engine_patches/qwen38_draft_head.h only
loads the head without the model's layers and keeps its state per
conversation. Runs after prepare_qwen38_gguf and the vision Edge hooks.
"""
from pathlib import Path
from prepare_qwen_variants import once

# What upstream's q38_mtp_attach loads, after prepare_qwen38_gguf's renames.
# qwen38_draft_head.h repeats these calls between its LMB-MTP-LOADER marks
# because upstream's function also demands the whole model and exits the
# process on refusal. If upstream changes what a head needs, this fails.
UPSTREAM_LOADER = '''    snprintf(nm,sizeof nm,"%s.pre_fc_norm_embedding.weight",found); m->mtp_norm_emb=q38_load_tensor(m,nm,H);
    snprintf(nm,sizeof nm,"%s.pre_fc_norm_hidden.weight",found); m->mtp_norm_hid=q38_load_tensor(m,nm,W);
    snprintf(nm,sizeof nm,"%s.fc_embedding.weight",found); m->mtp_fc_emb=q38_load_weight(m,nm,H,H);
    snprintf(nm,sizeof nm,"%s.fc_hidden.weight",found); m->mtp_fc_hid=q38_load_weight(m,nm,H,H);
    snprintf(base,sizeof base,"%s.hyper_connection_mixer",found); q38_load_gr_at(m,&m->mtp_mixer,base,0);
    m->mtp=1;                                 /* from here on index c.layers is the head's layer */
    m->mtp_drafts=2;
'''
UPSTREAM_STATE = '''    int mtp, mtp_wiring;
    int mtp_drafts;                   /* drafts per verify when Q38_MTP_DRAFTS is unset */
    size_t mtp_expert_bytes;          /* one routed expert of the head, gate, up and down (0 when fused) */
    char mtp_prefix[32];
    float *mtp_norm_emb, *mtp_norm_hid;
    Q38Weight mtp_fc_emb, mtp_fc_hid;
    GatedResidual mtp_mixer;
    int mtp_len, mtp_pend_n, mtp_pend_tok[Q38_SPEC_ROWS];
    float *mtp_pend;                  /* Q38_SPEC_ROWS rows of hc_width */
'''
OPEN_FLAG = 'UINT64_C(0x4c4d424d54504844)'  # "LMBMTPHD", LMB_EDGE_OPEN_DRAFT_HEAD


def prepare(root):
    root = Path(root)
    core = (root / 'qwen38_core.h').read_text()
    for name, text in (('loader', UPSTREAM_LOADER), ('state', UPSTREAM_STATE)):
        if core.count(text) != 1:
            raise RuntimeError('Qwen3.8 MTP ' + name + ' changed upstream; review engine_patches/qwen38_draft_head.h')
    # planner_adapters/qwen38_gguf.h prices this many pending rows per conversation.
    if core.count('#define Q38_SPEC_ROWS 6\n') != 1:
        raise RuntimeError('Qwen3.8 Q38_SPEC_ROWS changed upstream; update LMB_Q38_HEAD_PENDING_ROWS')
    for needed in ('static void q38_mtp_feed(Model *m,const int *ids,int S,int pos_base,const float *streams) {',
                   'static int q38_mtp_draft(Model *m,int tok,int pos,float *hidden) {',
                   'static int q38_mtp_draft_more(Model *m,float *hidden,int tok,int row) {',
                   'static void q38_load_layer(Model *m,int i,int cap) {'):
        if core.count(needed) != 1:
            raise RuntimeError('Qwen3.8 MTP entry point changed upstream: ' + needed)
    path = root / 'qwen38.c'
    s = path.read_text()
    s = once(s, '#include "engine_patches/qwen38_vision_edge.h"',
             '#include "engine_patches/qwen38_vision_edge.h"\n#include "engine_patches/qwen38_draft_head.h"')
    s = once(s, '    lmb_q38_vision_close(model);\n', '    lmb_q38_vision_close(model);\n    lmb_q38_head_close(model);\n')
    # The caller asks for the head; a checkpoint without one opens as before.
    s = once(s, '''"unsupported or unavailable resident vision tower");
    }
    uint64_t resident=model->resident_weight_bytes;''', '''"unsupported or unavailable resident vision tower");
    }
    if (options->reserved_u64[0]==''' + OPEN_FLAG + ''' &&
        lmb_q38_head_open(model,options->memory_limit_bytes)) {
        qwen38_edge_engine_destroy(engine);
        return coli_edge_adapter_error(error,error_size,"cannot prepare the resident Qwen3.8 MTP draft head");
    }
    uint64_t resident=model->resident_weight_bytes;''')
    path.write_text(s)


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--copy', required=True, type=Path)
    prepare(p.parse_args().copy)
