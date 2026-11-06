/* Stat-only preview: no hashing or reading model weights. The signed source
 * inventory is still authoritative and is checked again before offers. */
#ifndef LUMABRI_CHECKPOINT_INVENTORY_H
#define LUMABRI_CHECKPOINT_INVENTORY_H
#include "src/runtime/lumabri_source_file.h"
#include "lumabri_content.h"
#include "src/planner/lumabri_capabilities.h"

typedef struct {
    uint64_t bytes;
    uint32_t files;
    int has_weights;
    uint32_t formats; /* file containers, not proof that their tensors are supported */
} LmbCheckpointInventory;

typedef int (*LmbCheckpointVisitor)(const char *,const char *,const LmbSourceStat *,void *);
typedef struct {
    const char *root,*rel;
    unsigned depth;
    LmbCheckpointInventory *out;
    LmbCheckpointVisitor visit;
    void *arg;
} LmbCheckpointWalk;
static int lmb_checkpoint_walk(const char *,const char *,unsigned,LmbCheckpointInventory *,LmbCheckpointVisitor,void *);
static int lmb_checkpoint_entry(const char *name,void *context) {
    LmbCheckpointWalk *w=context;
    if (!strcmp(name,".") || !strcmp(name,"..") || lmb_content_runtime_local_name(name)) return 0;
    char child[512],path[1024];
    int n=snprintf(child,sizeof child,"%s%s%s",w->rel,*w->rel?"/":"",name);
    if (n<0 || (size_t)n>=sizeof child) return -1;
    n=snprintf(path,sizeof path,"%s/%s",w->root,child);
    if (n<0 || (size_t)n>=sizeof path) return -1;
    LmbSourceStat st;if (lmb_source_stat(path,&st)) return -1;
    if (st.directory) {
        /* Weight-file symlinks in HF caches remain valid on POSIX; directory
         * aliases cannot introduce cycles into this bounded preview. */
        return st.symlink ? -1 : lmb_checkpoint_walk(w->root,child,w->depth+1,w->out,w->visit,w->arg);
    }
    if (st.regular) {
        if (w->out->files==LMB_CONTENT_MAX_FILES || UINT64_MAX-w->out->bytes<st.size) return -1;
        w->out->files++;w->out->bytes+=st.size;
        if (w->visit && w->visit(w->root,child,&st,w->arg)) return -1;
        const char *dot=strrchr(name,'.');
        if (dot && !strcmp(dot,".safetensors")) w->out->formats |= LMB_FORMAT_SAFETENSORS;
        else if (dot && !strcmp(dot,".gguf")) w->out->formats |= LMB_FORMAT_GGUF;
        else if (dot && (!strcmp(dot,".bin") || !strcmp(dot,".coli"))) w->out->formats |= LMB_FORMAT_OTHER;
        if (st.size>=4096 && dot && (!strcmp(dot,".safetensors") || !strcmp(dot,".bin") ||
            !strcmp(dot,".gguf") || !strcmp(dot,".coli"))) w->out->has_weights=1;
    }
    return 0;
}
static int lmb_checkpoint_walk(const char *root,const char *rel,unsigned depth,
    LmbCheckpointInventory *out,LmbCheckpointVisitor visit,void *arg) {
    if (depth>32) return -1;
    char directory[1024];int n=snprintf(directory,sizeof directory,"%s%s%s",root,*rel?"/":"",rel);
    if (n<0 || (size_t)n>=sizeof directory) return -1;
    LmbCheckpointWalk w={root,rel,depth,out,visit,arg};
    return lmb_source_list(directory,lmb_checkpoint_entry,&w);
}

static inline int lmb_checkpoint_inventory(const char *root, LmbCheckpointInventory *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!root || !*root || lmb_checkpoint_walk(root, "", 0, out, NULL, NULL)) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}
#endif
