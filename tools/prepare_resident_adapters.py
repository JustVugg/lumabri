#!/usr/bin/env python3
"""Add Lumabri's resident preparation contract to disposable engine copies.

Never modifies the upstream checkout. Exact anchors fail closed on drift.
Model arithmetic is unchanged: existing expert_get loads the ordinary slots.
"""
import argparse
from pathlib import Path

REQUIRED = 'getenv("LUMABRI_RESIDENT_REQUIRED") && !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"), "1")'


def insert_open(root, file, family, code):
    path = root / file
    source = path.read_text()
    start = source.index(f"static int {family}_segment_engine_open(")
    at = source.index("    memset(capabilities,", start)
    source = source[:at] + f"    if ({REQUIRED}) {{\n" + code + "\n        lmb_resident_adapter_prepared();\n    }\n" + source[at:]
    path.write_text(source)


def budget(cleanup):
    return '''
                if (lmb_resident_budget_exceeded(options->memory_limit_bytes)) {
                    CLEANUP
                    return coli_segment_adapter_error(error, error_size,
                        "resident preparation exceeded the approved memory budget");
                }
'''.replace("CLEANUP", cleanup)


def other_adapters(root):
    cleanup = "inkling_segment_model_destroy(engine); pthread_mutex_destroy(&engine->run_lock); free(engine);"
    insert_open(root, "inkling.c", "inkling", '''
        for (uint32_t layer=engine->layer_begin; layer<engine->layer_end; layer++) {
            if (!engine->model.c.sparse[layer]) continue;
            if (engine->model.cache[layer].cap < engine->model.c.n_experts) {
                CLEANUP
                return coli_segment_adapter_error(error,error_size,"resident Inkling cache too small");
            }
            for (int eid=0; eid<engine->model.c.n_experts; eid++) {
                Slot *s=slot_acquire(&engine->model,(int)layer,eid);
                slot_fill(&engine->model,(int)layer,s); s->pinned=1;
                BUDGET
            }
        }
'''.replace("CLEANUP", cleanup).replace("BUDGET", budget(cleanup)))
    cleanup = "kimi_segment_model_destroy(engine); pthread_mutex_destroy(&engine->run_lock); free(engine);"
    insert_open(root, "kimi_k3.c", "kimi", '''
        Model *m=&engine->model;
        for (uint32_t layer=engine->layer_begin; layer<engine->layer_end; layer++) {
            if (!m->L[layer].sparse) continue;
            LCache *lc=&m->ecache[layer];
            if (lc->cap < m->c.n_experts) {
                CLEANUP
                return coli_segment_adapter_error(error,error_size,"resident Kimi cache too small");
            }
            for (int eid=0; eid<m->c.n_experts; eid++) {
                Slot *s=&lc->s[lc->n]; expert_read(m,(int)layer,eid,s);
                s->eid=eid; s->used=++m->clock; s->pinned=1;
                lc->n++; cache_index(m,(int)layer,s);
                BUDGET
            }
        }
'''.replace("CLEANUP", cleanup).replace("BUDGET", budget(cleanup)))
    cleanup = "glm_segment_model_destroy(engine); pthread_mutex_destroy(&engine->run_lock); free(engine);"
    insert_open(root, "colibri.c", "glm", '''
        Model *m=&engine->model;
        for (uint32_t layer=engine->layer_begin; layer<engine->layer_end; layer++) {
            if (!m->L[layer].sparse) continue;
            if (m->ecap < m->c.n_experts) {
                CLEANUP
                return coli_segment_adapter_error(error,error_size,"resident GLM cache too small");
            }
            for (int eid=0; eid<m->c.n_experts; eid++) {
                ESlot *s=&m->ecache[layer][m->ecn[layer]];
                if (expert_load(m,(int)layer,eid,s,0,0)) {
                    CLEANUP
                    return coli_segment_adapter_error(error,error_size,"cannot prepare resident GLM expert");
                }
                m->ecn[layer]++; ecache_publish(m,(int)layer,s,eid);
                s->used=++m->eclock;
                BUDGET
            }
        }
'''.replace("CLEANUP", cleanup).replace("BUDGET", budget(cleanup)))
    cleanup = "q38_model_free(&e->model); pthread_mutex_destroy(&e->run_lock); free(e);"
    insert_open(root, "qwen38.c", "qwen38", '''
        for (int part=0; part<e->model.ple_part_count; part++) {
            st_tensor *table=e->model.ple_parts[part];
            if (!table || lmb_resident_retain(table->fd,table->off,table->nbytes,
                                              options->memory_limit_bytes)) {
                CLEANUP
                return coli_segment_adapter_error(error,error_size,"cannot retain Qwen3.8 PLE table in RAM");
            }
        }
        for (uint32_t layer=e->layer_begin; layer<e->layer_end; layer++) {
            if (e->model.cache[layer].cap < e->model.c.experts) {
                CLEANUP
                return coli_segment_adapter_error(error,error_size,"resident Qwen3.8 cache too small");
            }
            for (int eid=0; eid<e->model.c.experts; eid++) {
                (void)q38_expert_get(&e->model,(int)layer,eid);
                BUDGET
            }
        }
'''.replace("CLEANUP", cleanup).replace("BUDGET", budget(cleanup)))
    path=root / "kimi_k3.c"
    source=path.read_text()
    start=source.index("static int kimi_edge_engine_open(")
    at=source.index("    uint64_t resident = kimi_edge_w_bytes", start)
    code='''
    if (REQUIRED) {
        snprintf(name,sizeof name,"%smodel.embed_tokens.weight",model->pfx);
        st_tensor *embedding=st_find(&model->S,name);
        if (!embedding || lmb_resident_retain(embedding->fd,embedding->off,embedding->nbytes,
                                               options->memory_limit_bytes)) {
            kimi_edge_engine_destroy(engine);
            return coli_edge_adapter_error(error,error_size,"cannot retain Kimi embedding in RAM");
        }
    }
'''.replace("REQUIRED", REQUIRED)
    source=source[:at]+code+source[at:]
    # Report the retained embedding too; planner already reserves its F32 upper bound.
    anchor="    int bits = getenv(\"K3_BITS\") ? atoi(getenv(\"K3_BITS\")) : 4;"
    at=source.index(anchor, source.index("static int kimi_edge_engine_open("))
    source=source[:at]+f'''    if ({REQUIRED}) {{
        snprintf(name,sizeof name,"%smodel.embed_tokens.weight",model->pfx);
        resident+=(uint64_t)st_find(&model->S,name)->nbytes;
    }}
'''+source[at:]
    path.write_text(source)
    path=root / "glm53.c"
    source=path.read_text()
    anchor="    if (cap > c->n_experts) cap = c->n_experts;\n"
    if source.count(anchor)!=1: raise RuntimeError("GLM5.3 capacity anchor changed")
    path.write_text(source.replace(anchor,anchor+f"    if ({REQUIRED}) cap = c->n_experts;\n",1))
    cleanup = "model_release(&engine->model); pthread_mutex_destroy(&engine->run_lock); free(engine);"
    insert_open(root, "glm53.c", "glm53", '''
        GModel *m=&engine->model;
        if (m->streaming) {
            for (uint32_t layer=engine->layer_begin; layer<engine->layer_end; layer++) {
                if (layer < (uint32_t)m->c.first_dense) continue;
                for (int eid=0; eid<m->c.n_experts; eid++) {
                    (void)expert_slot(m,(int)layer,eid);
                    BUDGET
                }
            }
        }
'''.replace("BUDGET", budget(cleanup)))
    insert_open(root, "deepseek_v4.c", "deepseek_v4", '''
        if (slots < (uint64_t)model->config.n_routed_experts) {
            coli_segment_adapter_error(error,error_size,"resident V4 cache too small"); goto fail;
        }
        for (uint32_t layer=engine->layer_begin; layer<engine->layer_end; layer++) {
            ColiDeepSeekV4LayerWeights weights;
            if (coli_v4_layer_load(model,&weights,&model->config,model->target_index,
                                  (int)layer,error,error_size)) goto fail;
            coli_v4_layer_free(model,&weights);
            for (int eid=0; eid<model->config.n_routed_experts; eid++) {
                ColiExpertView view={0}; ColiExpertKey key={(int)layer,eid};
                if (coli_expert_lookup(model->experts,key,&view)) {
                    coli_segment_adapter_error(error,error_size,"cannot prepare resident V4 expert"); goto fail;
                }
                coli_expert_release(model->experts,&view);
                if (lmb_resident_budget_exceeded(options->memory_limit_bytes)) {
                    coli_segment_adapter_error(error,error_size,"resident V4 exceeds approved RAM"); goto fail;
                }
            }
        }
''')
    path=root / "deepseek_v4.c"
    source=path.read_text()
    start=source.index("static int deepseek_v4_edge_engine_open(")
    at=source.index("    memset(capabilities,",start)
    code='''
    if (REQUIRED) {
        const ColiSafetensorsTensor *tables[2]={embedding,head};
        for (int i=0;i<2;i++) {
            int shard=coli_st_tensor_shard(engine->index,tables[i]);
            const char *path=coli_st_shard_path(engine->index,shard);
            int fd=path ? open(path,O_RDONLY) : -1;
            int failed=fd<0 || lmb_resident_retain(fd,tables[i]->off,tables[i]->nbytes,
                                                   options->memory_limit_bytes);
            if (fd>=0) close(fd);
            if (failed) {
                deepseek_v4_edge_engine_destroy(engine);
                return coli_edge_adapter_error(error,error_size,"cannot retain V4 embedding/head in RAM");
            }
            resident+=(uint64_t)tables[i]->nbytes;
        }
    }
'''.replace("REQUIRED",REQUIRED)
    path.write_text(source[:at]+code+source[at:])

def prepare(root):
    for family in ("olmoe", "qwen36"):
        path = root / (family + ".c")
        source = path.read_text()
        anchor = ("    model_init_range(&engine->model, options->model_dir, cap, 8,\n"
                  "                     (int)options->layer_begin, (int)options->layer_end, 0, 0);\n")
        if source.count(anchor) != 1:
            raise RuntimeError(f"{family}: resident initialization anchor changed")
        addition = '''
    if (getenv("LUMABRI_RESIDENT_REQUIRED") &&
        !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"), "1")) {
        for (uint32_t layer = engine->layer_begin; layer < engine->layer_end; layer++) {
            if (engine->model.cache[layer].cap < engine->model.c.n_experts) {
                FAMILY_segment_model_destroy(engine);
                pthread_mutex_destroy(&engine->run_lock); free(engine);
                return coli_segment_adapter_error(error, error_size,
                    "resident plan cannot hold every assigned expert");
            }
            for (int eid = 0; eid < engine->model.c.n_experts; eid++) {
                Slot *slot = NULL;
                expert_get(&engine->model, (int)layer, eid, &slot);
                if (!slot || lmb_resident_budget_exceeded(options->memory_limit_bytes)) {
                    FAMILY_segment_model_destroy(engine);
                    pthread_mutex_destroy(&engine->run_lock); free(engine);
                    return coli_segment_adapter_error(error, error_size,
                        "resident preparation exceeded the approved memory budget");
                }
            }
            fprintf(stderr, "[resident] layer %u prepared: %d/%d experts in engine RAM\\n",
                    layer, engine->model.cache[layer].n, engine->model.c.n_experts);
        }
        lmb_resident_adapter_prepared();
    }
'''.replace("FAMILY", family)
        # Qwen chooses the checkpoint's numeric expert representation after
        # initialization. Warm the cache only after that choice is made.
        if family == "qwen36":
            anchor = "    engine->model.resident_mode = 0;\n"
            if source.count(anchor) != 1:
                raise RuntimeError("qwen36: resident policy anchor changed")
        path.write_text(source.replace(anchor, anchor + addition, 1))
    other_adapters(root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--copy", required=True, type=Path)
    prepare(parser.parse_args().copy)
