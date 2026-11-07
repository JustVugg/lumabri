"""GGUF boundaries for generated adapter copies; upstream checkout stays read-only."""
from pathlib import Path


def replace_once(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError('OLMoE GGUF adapter anchor changed: ' + old[:100])
    return s.replace(old, new, 1)


def prepare_header(root):
    # The mirror's stdio path materializes a file. Header discovery must use
    # descriptors so it fetches only the CAS blocks that contain the header.
    path=Path(root)/'gguf.h'
    s=path.read_text()
    s=replace_once(s,'typedef struct { FILE *f; uint64_t pos, size; int bad; char *err; } gguf_rd;',
                   'typedef struct { int fd; uint64_t pos, size; int bad; char *err; } gguf_rd;')
    old='''    if (n && fread(dst, 1, (size_t)n, r->f) != (size_t)n) { r->bad = 1; snprintf(r->err, 512, "read error at byte %llu", (unsigned long long)r->pos); return -1; }'''
    s=replace_once(s,old,'''    uint64_t done = 0;
    while (done < n) {
        size_t want = (size_t)((n - done) > (1u << 20) ? (1u << 20) : n - done);
        int64_t got = read(r->fd, (unsigned char *)dst + done, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { r->bad = 1; snprintf(r->err, 512, "read error at byte %llu", (unsigned long long)(r->pos + done)); return -1; }
        done += (uint64_t)got;
    }''')
    begin=s.index('static int gguf_parse_one(')
    end=s.index('\nstatic void gguf_close(',begin)
    body=s[begin:end]
    body=replace_once(body,'    FILE *f = fopen(path, "rb");','''#ifdef _WIN32
    int fd = open(path, O_RDONLY | O_BINARY);
#else
    int fd = open(path, O_RDONLY);
#endif''')
    body=body.replace('if (!f)', 'if (fd < 0)').replace('fstat(fileno(f),', 'fstat(fd,')
    body=body.replace('fclose(f)', 'close(fd)').replace('gguf_rd r = { f,', 'gguf_rd r = { fd,')
    s=s[:begin]+body+s[end:]
    s=replace_once(s,'''    FILE *f = fopen(path, "rb"); if (!f) return 0;
    char m[4]; int ok = fread(m, 1, 4, f) == 4 && !memcmp(m, "GGUF", 4);
    fclose(f); return ok;''','''#ifdef _WIN32
    int fd = open(path, O_RDONLY | O_BINARY);
#else
    int fd = open(path, O_RDONLY);
#endif
    if (fd < 0) return 0;
    char m[4]; uint64_t done = 0;
    while (done < sizeof m) {
        int64_t got = read(fd, m + done, sizeof m - (size_t)done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        done += (uint64_t)got;
    }
    close(fd); return done == sizeof m && !memcmp(m, "GGUF", sizeof m);''')
    path.write_text(s)


def prepare(root):
    prepare_header(root)
    path=Path(root)/'olmoe.c'
    s=path.read_text()
    anchor='static void model_init_range(Model *m, const char *snap, int cap, int bits,'
    s=replace_once(s,anchor,'''/* st_gguf's temporary metadata is process-global. The shards index owns its
 * names/descriptors, so release the temporary header after indexing, under a
 * lock, rather than leaking it or replacing another opening engine's header. */
static pthread_mutex_t lmb_olm_index_lock = PTHREAD_MUTEX_INITIALIZER;
static int lmb_olm_index(shards *S, const char *dir) {
    pthread_mutex_lock(&lmb_olm_index_lock);
    st_gguf_active = 0;
    st_init(S, dir);
    int gguf = st_gguf_active;
    gguf_close(&st_gguf_meta); gguf_close(&st_gguf_head);
    st_gguf_active = 0;
    pthread_mutex_unlock(&lmb_olm_index_lock);
    return gguf;
}

'''+anchor)
    s=replace_once(s,'    st_init(&m->S, snap);','    (void)lmb_olm_index(&m->S, snap);')
    s=replace_once(s,'    st_init(&engine->model.S, options->model_dir);',
                   '    engine->model.gx = lmb_olm_index(&engine->model.S, options->model_dir);')
    s=replace_once(s,'    engine->model.embed = load_t(&engine->model, "model.embed_tokens.weight", ec->vocab, ec->hidden);',
                   '    engine->model.embed = load_t_gx(&engine->model, "model.embed_tokens.weight", ec->vocab, ec->hidden, NULL, &engine->model.gembed, 0);')
    s=replace_once(s,'''#ifdef OLM_DENSE_F16
    olm_dense_half_init();
    engine->model.lm_head = load_t_dense(&engine->model, "lm_head.weight", ec->vocab, ec->hidden,
                                         &engine->model.lm_head_h);
#else
    engine->model.lm_head = load_t(&engine->model, "lm_head.weight", ec->vocab, ec->hidden);
#endif''','''#ifdef OLM_DENSE_F16
    olm_dense_half_init();
#endif
    engine->model.lm_head = load_t_gx(&engine->model, "lm_head.weight", ec->vocab, ec->hidden,
                                     &engine->model.lm_head_h, &engine->model.ghead, 1);''')
    s=replace_once(s,'''static void olm_edge_head(float *logits, const float *x, const Model *m) {
''','''static void olm_edge_head(float *logits, const float *x, const Model *m) {
    if (m->ghead.b) { olm_gx_matmul(logits, x, &m->ghead, 1, m->c.hidden, m->c.vocab); return; }
''')
    s=replace_once(s,'''    free(engine->model.lm_head_h);
''','''    free(engine->model.lm_head_h);
    free(engine->model.gembed.b); free(engine->model.ghead.b);
''')
    s=replace_once(s,'''        memcpy(output + (size_t)row * config->hidden,
               engine->model.embed + (size_t)token * config->hidden,
               (size_t)config->hidden * sizeof(float));''','''        olm_embed_row(&engine->model, token, output + (size_t)row * config->hidden);''')
    s=replace_once(s,'''    uint64_t resident = (cells + (uint64_t)config->hidden) * sizeof(float) +
                        cells * (engine->model.lm_head_h ? sizeof(uint16_t) : sizeof(float));''','''    const Model *m = &engine->model;
    uint64_t embedding = m->gembed.b ? (uint64_t)gq_row_bytes(m->gembed.t, config->hidden) * config->vocab : cells * sizeof(float);
    uint64_t head = m->ghead.b ? (uint64_t)gq_row_bytes(m->ghead.t, config->hidden) * config->vocab :
                    cells * (m->lm_head_h ? sizeof(uint16_t) : sizeof(float));
    uint64_t resident = embedding + head + (uint64_t)config->hidden * sizeof(float);''')
    # Memory admission is enforced by the planner and the locked allocator.
    # The old cap calculation assumes int8; it cannot size a mixed GGUF bank.
    anchor='''    model_init_range(&engine->model, options->model_dir, cap, 8,
                     (int)options->layer_begin, (int)options->layer_end, 0, 0);'''
    s=replace_once(s,anchor,'''    if (getenv("LUMABRI_RESIDENT_REQUIRED") && !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"), "1"))
        cap = config.n_experts;
'''+anchor)
    old='"olmoe/f32-int8/cpu-v1"'
    if s.count(old)!=2: raise RuntimeError('OLMoE numeric class anchors changed')
    s=s.replace(old,'engine->model.gx ? "olmoe/f32-gguf/cpu-v1" : '+old)
    path.write_text(s)
