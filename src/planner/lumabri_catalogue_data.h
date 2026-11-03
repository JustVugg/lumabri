/* Shared catalogue data: model metadata and authenticated machine reports.
 * Rendering and background preparation consume this same typed snapshot. */
#ifndef LMB_CATALOGUE_DATA_H
#define LMB_CATALOGUE_DATA_H
#include "lumabri_inventory.h"
#include "lumabri_checkpoint_inventory.h"
#include "src/ui/lumabri_tui.h"
#include "src/planner/lumabri_stage_placement.h"

typedef struct {
    char dir[512];
    char name[64];
    LmbModelShape shape;
    int has_weights;
    uint64_t checkpoint_bytes;
    int inventory_ok;
} CatalogEntry;

typedef struct {const char *root;CatalogEntry *out;int count,cap;} CatalogScan;
static int catalog_scan_entry(const char *name,void *context) {
    CatalogScan *scan=context;
    if (name[0]=='.' || scan->count>=scan->cap) return 0;
    char path[832];int n=snprintf(path,sizeof path,"%s/%s",scan->root,name);
    if (n<0 || (size_t)n>=sizeof path || strlen(path)>=sizeof scan->out[0].dir ||
        strlen(name)>=sizeof scan->out[0].name) return 0;
    CatalogEntry *entry=&scan->out[scan->count];
    if (lmb_shape_from_config(path,&entry->shape)) return 0;
    strcpy(entry->dir,path);strcpy(entry->name,name);
    LmbCheckpointInventory inventory;
    entry->inventory_ok=!lmb_checkpoint_inventory(path,&inventory);
    entry->has_weights=inventory.has_weights;entry->checkpoint_bytes=inventory.bytes;
    scan->count++;return 0;
}
static int catalog_scan(const char *root,CatalogEntry *out,int cap) {
    if (!root || !*root || !out || cap<=0) return 0;
    CatalogScan scan={root,out,0,cap};
    return lmb_source_list(root,catalog_scan_entry,&scan) ? 0 : scan.count;
}

/* This machine as a planner node. The reserve is the governor's, so the
 * catalogue never plans into memory the governor would refuse to give. */
static void catalog_self(LmbClusterNode *n, LmbMachineProfile *profile,
                          const char *disk) {
    LmbMachineProfile p;
    memset(n, 0, sizeof *n);
    if (profile->logical_cpus) {
        p = *profile;
        lmb_machine_refresh_resources(&p, disk);
    } else if (lmb_machine_probe(&p, disk, NULL)) return;
    *profile = p;
    snprintf(n->name, sizeof n->name, "%s", p.hostname);
    uint64_t reserve = lmb_machine_ram_reserve();
    n->ram_budget_bytes = p.ram_available_bytes > reserve
                        ? p.ram_available_bytes - reserve : 0;
    n->vram_budget_bytes = p.vram_available_bytes;
    n->disk_read_bps = p.disk_read_bps;
    /* TUI compile flags and a detected device are not an adapter execution
     * test. The resident household contract is currently CPU-only. */
    n->gpu_backends = 0;
    n->threads = p.logical_cpus;
    n->has_checkpoint = 0;      /* set per catalogue entry, never globally */
}

static void inventory_id_text(const uint8_t id[32], char out[65]) {
    for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", id[i]);
}

static int catalog_inventory(LmbTuiState *st) {
    memset(st->nodes, 0, sizeof st->nodes);
    memset(st->identities, 0, sizeof st->identities);
    memset(st->runtime_ids, 0, sizeof st->runtime_ids);
    memset(st->ages_ms, 0, sizeof st->ages_ms);
    memset(st->facts, 0, sizeof st->facts);
    memset(st->workloads, 0, sizeof st->workloads);
    catalog_self(&st->nodes[0], &st->profiles[0], st->disk[0] ? st->disk : ".");
    st->facts[0] = lmb_resource_facts_local(st->profiles[0].logical_cpus ? st->profiles[0].load_one : -1);
    st->nnodes = 1;
    st->inventory_ok = 1;
    if (!st->tracker[0]) return 0;
    char path[1024]; uint8_t sk[64], pk[32];
    if (lmb_peer_identity(lmb_peer_key_path(path, sizeof path), sk, pk)) {
        st->inventory_ok = 0; return -1;
    }
    memset(sk, 0, sizeof sk);
    inventory_id_text(pk, st->identities[0]);
    LmbMachineReport reports[LMB_INVENTORY_MAX];
    uint32_t n = 0;
    if (lmb_inventory_fetch(st->tracker, reports, &n)) {
        st->inventory_ok = 0; return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        LmbMachineReport *r = &reports[i];
        uint32_t at;
        if (!memcmp(pk, r->identity, 32)) at = 0;
        else {
            if (st->nnodes == LMB_CLUSTER_MAX_NODES) {
                st->nnodes = 1; st->inventory_ok = 0; return -1;
            }
            at = st->nnodes++;
        }
        LmbClusterNode *node = &st->nodes[at];
        snprintf(node->name, sizeof node->name, "%s", r->machine.hostname);
        snprintf(node->addr, sizeof node->addr, "%s", r->control_addr);
        node->ram_budget_bytes = r->ram_budget_bytes;
        node->roles = r->roles;
        node->vram_budget_bytes = r->machine.vram_available_bytes;
        node->threads = r->machine.logical_cpus;
        if (r->runtime_threads && node->threads > r->runtime_threads) node->threads = r->runtime_threads;
        snprintf(st->runtime_ids[at], sizeof st->runtime_ids[at], "%s", r->runtime_id);
        /* Resource reporting is not executable Segment capability. No remote
         * card is promoted to a supported backend by an inventory advert. */
        node->gpu_backends = 0;
        node->disk_read_bps = r->machine.disk_read_bps;
        st->profiles[at] = r->machine;
        st->facts[at] = r->facts;
        st->workloads[at] = r->workload;
        st->ages_ms[at] = r->age_ms;
        inventory_id_text(r->identity, st->identities[at]);
    }
    return 0;
}

static void catalog_advice_refresh(LmbTuiState *st) {
    LmbAdviceCandidate candidates[LMB_TUI_MAX_MODELS] = {0};
    uint32_t advice[LMB_TUI_MAX_MODELS];
    for (int i = 0; i < st->nmodels; i++) {
        const LmbTuiModel *m = &st->models[i];
        LmbAdviceCandidate *c = &candidates[i];
        c->eligible = st->inventory_ok && m->planned && m->shape.sizing_verified &&
            m->weights_present && m->checkpoint_inventory_ok &&
            m->plan.state == LMB_PLAN_RESIDENT && m->plan.nslices;
        for (uint32_t j = 0; c->eligible && j < m->plan.nslices; j++) {
            uint32_t node = m->plan.slices[j].node;
            if (node >= st->nnodes || !lmb_tui_node_enabled(st, node)) c->eligible = 0;
            c->reserved_bytes = lmb_budget_add(c->reserved_bytes, m->plan.slices[j].bytes_resident);
        }
        c->checkpoint_bytes = m->checkpoint_bytes;
        if (m->has_calibration && m->calibration_key_valid && lmb_cal_valid(&m->calibration) &&
            lmb_cal_matches(&m->calibration.key, &m->calibration_key))
            c->measured_tok_s = m->calibration.decode_tok_s;
    }
    lmb_catalogue_advice(candidates, (size_t)st->nmodels, advice);
    for (int i = 0; i < st->nmodels; i++) st->models[i].advice_flags = advice[i];
}


static int catalog_plan_entry(LmbTuiState *st,LmbTuiModel *m,const CatalogEntry *entry,
    LmbClusterNode selected[LMB_CLUSTER_MAX_NODES],uint32_t mapping[LMB_CLUSTER_MAX_NODES],uint32_t *selected_count) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%.63s", entry->name);
    snprintf(m->dir, sizeof m->dir, "%.511s", entry->dir);
    m->shape = entry->shape;
    m->weights_present = entry->has_weights;
    m->checkpoint_bytes = entry->checkpoint_bytes;
    m->checkpoint_inventory_ok = entry->inventory_ok;
    st->nodes[0].has_checkpoint = entry->has_weights;
    uint32_t nselected=0;int household=0;
    for (uint32_t j = 0; j < st->nnodes; j++)
        if (st->nodes[j].addr[0]) household = 1;
    for (uint32_t j = 0; j < st->nnodes; j++) {
        if (household && !lmb_tui_node_enabled(st, j)) continue;
        selected[nselected] = st->nodes[j];
        mapping[nselected++] = j;
    }
    if (household && m->weights_present) {
        m->planned = st->inventory_ok && m->checkpoint_inventory_ok &&
            !lmb_home_plan_source(&m->shape, m->checkpoint_bytes, selected,
            nselected, st->context, st->sessions, LMB_GOAL_ONE_SESSION, 1, &m->plan);
        if (m->planned && lmb_home_plan_selected(&m->shape, m->checkpoint_bytes,
            selected, nselected, st->context, &m->plan, NULL, &m->plan)) {
            m->plan.state = LMB_PLAN_UNRUNNABLE;
            m->plan.nslices = 0;
        }
        const char *hybrid = getenv("LUMABRI_HOME_HYBRID");
        const char *resident = getenv("LUMABRI_RESIDENT_REQUIRED");
        if (m->planned && st->sessions == 1 && (!resident || !strcmp(resident, "1")) &&
            (!hybrid || strcmp(hybrid, "0"))) {
            LmbClusterPlan candidate;
            if (!lmb_home_plan_hybrid(&m->shape, m->checkpoint_bytes, selected,
                nselected, st->context, &m->plan, &candidate)) m->plan = candidate;
        }
    } else
        m->planned = st->inventory_ok && !lmb_plan_cluster_source(&m->shape, selected,
        nselected, st->context, st->sessions, LMB_GOAL_ONE_SESSION, 0, &m->plan);
    if (m->planned) {
        m->plan.edge_node = mapping[m->plan.edge_node];
        for (uint32_t j = 0; j < m->plan.nslices; j++)
            m->plan.slices[j].node = mapping[m->plan.slices[j].node];
    }
    *selected_count=nselected;return household;
}

#endif
