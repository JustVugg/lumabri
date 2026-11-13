#!/usr/bin/env python3
"""Build-only V4.1 range hooks; never change the upstream checkout.

The household adapter and its independent boundary oracle share this copy. It
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
    if hashlib.sha256(text.encode()).hexdigest() != '3dbc511047cc0fd40b09d8476d00bf6cdbef08f54ebd70cf4d10b24e10c833ff':
        raise ValueError('V4.1 range engine requires pinned Colibri 2.1.0')
    text = replace_once(text, '#include "st.h"', '#include "v41_st.h"')
    text = replace_once(text, '    int fd_w, fd_s;\n    int64_t off_w, off_s, rows;',
                        '    int fd_w, fd_s;\n    uint8_t *resident_w, *resident_s;\n    size_t resident_w_bytes, resident_s_bytes;\n    int64_t resident_base_w, resident_base_s;\n    int64_t off_w, off_s, rows;')
    text = replace_once(text, "int head_dim, int cache_rows) {", "int head_dim, int cache_rows, int64_t first_row, int64_t end_row) {")
    text = replace_once(text,
        '            engram_table_open(&e->table[t], &m->S, layer, e->head_dim, engram_cache_rows);',
        '''            int64_t first_row=INT64_MAX,end_row=0;
            for(int g=0;g<e->max_ngram-1;g++)for(int h=0;h<e->heads;h++) {
                int k=g*e->heads+h;int64_t off=e->offsets[t][k],rows=e->primes[t][g][h];
                if(off<first_row)first_row=off;
                if(off+rows>end_row)end_row=off+rows;
            }
            engram_table_open(&e->table[t], &m->S, layer, e->head_dim, engram_cache_rows, first_row, end_row);''')
    # The released engine now also understands inline and ggml Engram rows.
    # Retain their exact bytes and keep its decoder unchanged. Planner format
    # admission remains separate; no format may fall back to disk at RUN.
    text = replace_once(text, 'cache:;\n    int cap = 1;', '''cache:;
    const st_tensor *weights = t->gt >= 0 ? g : w;
    if (!weights || weights->nbytes <= 0 || (uint64_t)weights->nbytes > SIZE_MAX) exit(1);
    if (first_row < 0 || end_row <= first_row || end_row > t->rows) exit(1);
    size_t row_bytes = t->gt >= 0 ? (size_t)t->rb : (size_t)head_dim + (t->inline_scales ? (size_t)head_dim/32 : 0);
    t->resident_w_bytes = (size_t)(end_row-first_row) * row_bytes;
    t->resident_base_w = t->off_w + first_row * (int64_t)row_bytes;
    t->resident_w = xmalloc(t->resident_w_bytes, "resident Engram weights");
    st_pread_full(t->fd_w, t->resident_w, t->resident_w_bytes, t->resident_base_w, "prepare Engram weights");
    if (t->gt < 0 && !t->inline_scales) {
        if (!s || s->nbytes <= 0 || (uint64_t)s->nbytes > SIZE_MAX) exit(1);
        t->resident_s_bytes = (size_t)(end_row-first_row) * (size_t)head_dim/32;
        t->resident_base_s = t->off_s + first_row * (head_dim/32);
        t->resident_s = xmalloc(t->resident_s_bytes, "resident Engram scales");
        st_pread_full(t->fd_s, t->resident_s, t->resident_s_bytes, t->resident_base_s, "prepare Engram scales");
    }
    int cap = 1;''')
    text = replace_once(text, '} EngramTable;', '''} EngramTable;
static void lmb_v41_engram_read(const EngramTable *t, int scales, void *out,
                                size_t bytes, int64_t offset, const char *what) {
    const uint8_t *data = scales ? t->resident_s : t->resident_w;
    size_t size = scales ? t->resident_s_bytes : t->resident_w_bytes;
    int64_t base = scales ? t->resident_base_s : t->resident_base_w;
    (void)what;
    if (!data || offset < base || (uint64_t)(offset-base) > size ||
        bytes > size-(size_t)(offset-base)) exit(1);
    memcpy(out, data+(size_t)(offset-base), bytes);
}''')
    # Limit replacements to the row decoder; preparation still performs reads.
    start = text.index('static const float *engram_row(')
    end = text.index('\nstatic ', start + 1)
    row = text[start:end]
    if row.count('st_pread_full(t->fd_w,') != 3 or row.count('st_pread_full(t->fd_s,') != 1:
        raise ValueError('V4.1 Engram row read contract changed')
    row = row.replace('st_pread_full(t->fd_w,', 'lmb_v41_engram_read(t, 0,')
    row = row.replace('st_pread_full(t->fd_s,', 'lmb_v41_engram_read(t, 1,')
    text = text[:start] + row + text[end:]
    text = replace_once(text, "    Cfg c;\n    shards S;", """    Cfg c;
    int range_begin, range_end, range_enabled, context_limit;
    const float *boundary_input, *boundary_mix;
    float *boundary_output, *boundary_output_mix;
    shards S;""")
    text = replace_once(text, '    cfg_load(c, snap);\n',
                        '    cfg_load(c, snap);\n    if (m->context_limit >= 2 && m->context_limit < c->max_positions) c->max_positions = m->context_limit;\n')
    # No global head/embedding and no unrelated layer weights on a donor.
    text = replace_once(text, '    wb_load(&m->S, &m->embed, "embed.weight", c->vocab, dim);',
                        '    if (!m->range_enabled) {\n    wb_load(&m->S, &m->embed, "embed.weight", c->vocab, dim);')
    text = replace_once(text, '    wf_load(&m->S, &m->norm, "norm.weight", dim);',
                        '    wf_load(&m->S, &m->norm, "norm.weight", dim);\n    }')
    text = replace_once(text, '    for (int i = 0; i < c->n_layers; i++) {\n        Layer *l = &m->L[i];\n        l->engram_index = -1;',
                        '    for (int i = 0; i < c->n_layers; i++) {\n        Layer *l = &m->L[i];\n        l->engram_index = -1;\n        if (m->range_enabled && (i < m->range_begin || i >= m->range_end)) continue;')
    text = replace_once(text, '            m->L[layer].engram_index = t;',
                        '            if (m->range_enabled && (layer < m->range_begin || layer >= m->range_end)) continue;\n            m->L[layer].engram_index = t;')
    text = replace_once(text, '''    for (int i = 0; i < c->n_layers; i++) {
        if (g_v41_dry) { memset(&m->cache[i], 0, sizeof(LCache)); continue; }   /* nothing is read */
        cache_init(m, &m->cache[i], ecap, 0);
    }''', """    memset(m->cache, 0, (size_t)c->n_layers * sizeof(LCache));
    for (int i = 0; i < c->n_layers; i++) {
        if (!m->range_enabled || (i >= m->range_begin && i < m->range_end))
            cache_init(m, &m->cache[i], ecap, 0);
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
    f = replace_once(f, '    for (int layer = chained; layer < c->n_layers; layer++) {',
                    '    for (int layer = m->range_enabled ? m->range_begin : 0;\n         layer < (m->range_enabled ? m->range_end : c->n_layers); layer++) {')
    f = replace_once(f, "    /* the last block's FFN mix collapses the stream one final time */", """    if (m->boundary_output) {
        memcpy(m->boundary_output, h, (size_t)n * hc * dim * sizeof(float));
        memcpy(m->boundary_output_mix, pre_mix, (size_t)n * hc * sizeof(float));
    } else {
    /* the last block's FFN mix collapses the stream one final time */""")
    f = replace_once(f, '    m->pos += n_all;', '    }\n    m->pos += n_all;')
    return text[:at] + f + text[end:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--artifact', choices=('core', 'st', 'st_gguf', 'gguf'), default='core')
    a = p.parse_args()
    if (a.source.resolve().parent in a.output.resolve().parents or a.output.is_symlink()):
        p.error('output must be a separate regular build artifact outside upstream')
    if a.artifact == 'core':
        result = prepare(a.source.read_text())
    elif a.artifact == 'st':
        result = replace_once(a.source.read_text(), '#include "st_gguf.h"', '#include "v41_st_gguf.h"')
    elif a.artifact == 'st_gguf':
        result = replace_once(a.source.read_text(), '    free(arena);\n',
                              '    json_free(root); free(arena);\n')
        result = replace_once(result, '#include "gguf.h"', '#include "v41_gguf.h"')
    else:
        from prepare_gguf_adapters import prepare_gguf_io
        result = prepare_gguf_io(a.source.read_text())
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(result)


if __name__ == '__main__':
    main()
