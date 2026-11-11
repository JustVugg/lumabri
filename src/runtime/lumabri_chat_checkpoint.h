/* Checkpoint-owned chat identity, resolved before resident inputs are sealed.
 * Requires the planner's bounded JSON accessors and chat_format.h. */
#ifndef LUMABRI_CHAT_CHECKPOINT_H
#define LUMABRI_CHAT_CHECKPOINT_H
#include "src/runtime/lumabri_chat_format.h"
#include <errno.h>

static char *chat_checkpoint_text(const char *root,const char *file,int *error) {
    char path[1400];
    *error=1;
    if (snprintf(path,sizeof path,"%s/%s",root,file)>=(int)sizeof path) return NULL;
    FILE *f=fopen(path,"rb");
    if (!f) { *error=errno!=ENOENT; return NULL; }
    const size_t limit=1u<<20;
    char *text=malloc(limit+1);
    if (!text) { fclose(f); return NULL; }
    size_t n=fread(text,1,limit,f);
    int bad=ferror(f) || !feof(f) || memchr(text,0,n)!=NULL;
    fclose(f);
    if (bad) { free(text); return NULL; }
    text[n]=0; *error=0; return text;
}

static EngKind chat_checkpoint_kind(const char *root,const char *engine) {
    EngKind base=engine_kind_of(engine),kind=base;
    if (base!=EK_QWEN36 && base!=EK_GLM) return base;
    int error=0;
    char *config=chat_checkpoint_text(root,"config.json",&error),type[64];
    if (!config) return EK_INVALID;
    int bad=lmb_json_string(config,"model_type",type,sizeof type);
    free(config);
    if (bad) return EK_INVALID;
    kind=engine_kind_of(type);
    if (engine_family_kind(kind)!=base) return EK_INVALID;
    if (kind!=EK_QWEN36) return kind;
    /* Qwen3 and Qwen3-Coder share model_type; the checkpoint's own
     * template distinguishes them, as in the pinned Colibri server. */
    const char *const files[]={"chat_template.jinja","tokenizer_config.json"};
    for (size_t i=0;i<2;i++) {
        char *text=chat_checkpoint_text(root,files[i],&error);
        if (error) return EK_INVALID;
        if (!text) continue;
        int coder=strstr(text,"interact with a computer to solve tasks")!=NULL;
        free(text);
        if (coder) return EK_QWEN_CODER;
    }
    return kind;
}
#endif
