#!/usr/bin/env python3
"""Build-only V4.1 range hooks; never change the upstream checkout.

This is an oracle laboratory, NOT a distributed adapter registration. It
reuses the pinned engine's layer arithmetic, loads only the selected layer
weights and preserves mHC pre_mix across a boundary. Cross-layer attention
state must also be transported; tests deliberately check that contract.
"""
import argparse
import hashlib
from pathlib import Path


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"Colibri V4.1 source drift at {before[:80]!r}")
    return text.replace(before, after, 1)


def prepare(text):
    # These hooks are audited against this source, not against similarly named
    # functions in a future release. Deliberate revalidation is required.
    if hashlib.sha256(text.encode()).hexdigest() != '29957921abf6febf6c409b3aac22319065acddc6f9e303df1b4e02b30d10b141':
        raise ValueError('V4.1 range laboratory requires pinned Colibri 1.12.1')
    text = replace_once(text, '    int fd_w, fd_s;\n    int64_t off_w, off_s, rows;',
                        '    int fd_w, fd_s;\n    uint8_t *resident_w, *resident_s;\n    int64_t off_w, off_s, rows;')
    text = replace_once(text, '    t->rows = w->nbytes / head_dim;', '''    t->rows = w->nbytes / head_dim;
    if (head_dim <= 0 || head_dim % 32 || w->nbytes <= 0 ||
        w->nbytes % head_dim || s->nbytes != t->rows * (head_dim / 32)) exit(1);
    t->resident_w = xmalloc((size_t)w->nbytes, "resident Engram weights");
    t->resident_s = xmalloc((size_t)s->nbytes, "resident Engram scales");
    st_pread_full(t->fd_w, t->resident_w, w->nbytes, t->off_w, "prepare Engram weights");
    st_pread_full(t->fd_s, t->resident_s, s->nbytes, t->off_s, "prepare Engram scales");''')
    text = replace_once(text, '''        st_pread_full(t->fd_w, bytes, head_dim, t->off_w + id * head_dim, "engram row");
        st_pread_full(t->fd_s, scales, groups, t->off_s + id * groups, "engram scale");''', '''        if (id < 0 || id >= t->rows || !t->resident_w || !t->resident_s) exit(1);
        memcpy(bytes, t->resident_w + (size_t)id * head_dim, (size_t)head_dim);
        memcpy(scales, t->resident_s + (size_t)id * groups, (size_t)groups);''')
    text = replace_once(text, "    Cfg c;\n    shards S;", """    Cfg c;
    int range_begin, range_end, range_enabled;
    const float *boundary_input, *boundary_mix;
    float *boundary_output, *boundary_output_mix;
    shards S;""")
    # No global head/embedding and no unrelated layer weights on a donor.
    text = replace_once(text, '    wb_load(&m->S, &m->embed, "embed.weight", c->vocab, dim);',
                        '    if (!m->range_enabled) {\n    wb_load(&m->S, &m->embed, "embed.weight", c->vocab, dim);')
    text = replace_once(text, '    wf_load(&m->S, &m->norm, "norm.weight", dim);',
                        '    wf_load(&m->S, &m->norm, "norm.weight", dim);\n    }')
    text = replace_once(text, '    for (int i = 0; i < c->n_layers; i++) {\n        Layer *l = &m->L[i];\n        l->engram_index = -1;',
                        '    for (int i = 0; i < c->n_layers; i++) {\n        Layer *l = &m->L[i];\n        l->engram_index = -1;\n        if (m->range_enabled && (i < m->range_begin || i >= m->range_end)) continue;')
    text = replace_once(text, '            m->L[layer].engram_index = t;',
                        '            if (m->range_enabled && (layer < m->range_begin || layer >= m->range_end)) continue;\n            m->L[layer].engram_index = t;')
    text = replace_once(text, '    for (int i = 0; i < c->n_layers; i++) cache_init(m, &m->cache[i], ecap);', """    memset(m->cache, 0, (size_t)c->n_layers * sizeof(LCache));
    for (int i = 0; i < c->n_layers; i++) {
        if (!m->range_enabled || (i >= m->range_begin && i < m->range_end))
            cache_init(m, &m->cache[i], ecap);
        /* Foreign KV owners are STATE replicas, never weight replicas. */
        else if (c->kv_source[i]) {
            int ratio = c->compress_ratio[i] > 0 ? c->compress_ratio[i] : 1;
            m->L[i].ckv = calloc((size_t)c->max_positions / ratio * hd, sizeof(float));
            m->L[i].ikey = calloc((size_t)c->max_positions / ratio * c->index_head_dim, sizeof(float));
            if (!m->L[i].ckv || !m->L[i].ikey) exit(1);
        }
    }""")
    text = replace_once(text, '    spec_load(m, ecap);', '    /* Text, non-speculative range oracle: DSpark is a separate contract. */')
    text = replace_once(text, '    if (c->vision_layers > 0) {', '    if (0 && c->vision_layers > 0) {')
    # Restrict only forward_full, not the backbone helpers or speculative loop.
    at = text.index('static void forward_full(Model *m, const int *ids, int n, float *logits, int spec_batch,',
                    text.index('static void forward_full(Model *m, const int *ids, int n, float *logits, int spec_batch,') + 1)
    end = text.index('\nstatic int argmax(', at)
    f = text[at:end]
    start_embed = f.index('    float *embedded = ')
    end_embed = f.index('    free(embedded);') + len('    free(embedded);')
    embed = f[start_embed:end_embed]
    f = f[:start_embed] + '''    if (m->boundary_input) memcpy(h, m->boundary_input, (size_t)n * hc * dim * sizeof(float));
    else {
''' + embed + '\n    }' + f[end_embed:]
    f = replace_once(f, '    /* V41_TRACE=2', '''    if (m->boundary_mix) memcpy(pre_mix, m->boundary_mix, (size_t)n * hc * sizeof(float));

    /* V41_TRACE=2''')
    f = replace_once(f, '    for (int layer = 0; layer < c->n_layers; layer++) {',
                    '    for (int layer = m->range_enabled ? m->range_begin : 0;\n         layer < (m->range_enabled ? m->range_end : c->n_layers); layer++) {')
    f = replace_once(f, "    /* the last block's FFN mix collapses the stream one final time */", """    if (m->boundary_output) {
        memcpy(m->boundary_output, h, (size_t)n * hc * dim * sizeof(float));
        memcpy(m->boundary_output_mix, pre_mix, (size_t)n * hc * sizeof(float));
    } else {
    /* the last block's FFN mix collapses the stream one final time */""")
    f = replace_once(f, '    m->pos += n;', '    }\n    m->pos += n;')
    return text[:at] + f + text[end:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    if (a.source.resolve().parent in a.output.resolve().parents or a.output.is_symlink()):
        p.error('output must be a separate regular build artifact outside upstream')
    result = prepare(a.source.read_text())
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(result)


if __name__ == '__main__':
    main()
