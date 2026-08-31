#include "lumabri_content.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(lmb_content_runtime_local_name(".coli_usage"));
    assert(lmb_content_runtime_local_name(".coli_kv"));
    assert(lmb_content_runtime_local_name(".coli_kv.3"));
    assert(lmb_content_runtime_local_name(".coli_ssd"));
    assert(lmb_content_runtime_local_name(".coli_ckpt"));
    assert(lmb_content_runtime_local_name(".lumabri_hashes"));
    assert(!lmb_content_runtime_local_name("config.json"));
    assert(!lmb_content_runtime_local_name("model-00001.safetensors"));
    assert(!lmb_content_runtime_local_name("tokenizer.json"));
    assert(!lmb_content_runtime_local_name("coli_usage"));
    assert(!lmb_content_runtime_local_name(".colibri_weights"));
    puts("CONTENT FILTER TEST: PASS (runtime-local files never enter manifests)");
    return 0;
}
