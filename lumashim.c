/* lumishim.c — liblumabri.so, the chatter-side shim.
 *
 * The engine is not modified and not aware. It is launched with
 *
 *   LD_PRELOAD=liblumabri.so  LUMABRI_VROOT=/virtual/model/dir  ...
 *
 * and opens LUMABRI_VROOT as if the model directory existed locally. The shim
 * interposes exactly the libc surface the engines use on the model dir —
 * open / fopen / opendir / pread / read / close (deepseek imports: open,
 * fopen, pread, fstat, close, opendir, readdir, posix_fadvise) — and routes
 * it to a local sparse mirror:
 *
 *   LUMABRI_CACHE/data/<rel>       sparse file, ftruncated to the true size
 *   LUMABRI_CACHE/maps/<rel>.lmap  one byte per block, 1 = block present
 *
 * open() returns a REAL fd to the sparse mirror, so fstat, readdir,
 * posix_fadvise, lseek and the kernel page cache all work natively with no
 * interposition. The only per-call cost after warmup is one table lookup and
 * one bitmap check before the real pread — this is why the shim exists
 * instead of a FUSE mount: FUSE pays kernel→daemon→kernel on every read
 * forever; here a warm read costs what a local read costs.
 *
 * A missing block is fetched from a peer (placement from the tracker, or
 * LUMABRI_PEERS directly), pwritten into the mirror, recorded in the map,
 * and only then does the engine's own pread proceed. Blocks are immutable
 * once fetched — model weights never change — so there is no invalidation,
 * and a warm cache works with every peer offline.
 *
 * Correctness rule, colibri-style: the shim may only change WHERE bytes come
 * from, never WHICH bytes. Any attempt to open a model file for writing gets
 * EROFS; a block that no peer can provide is a loud EIO, never zeros.
 *
 * Env: LUMABRI_VROOT (the virtual dir), LUMABRI_CACHE (local mirror root),
 *      LUMABRI_TRACKER=host:port | LUMABRI_PEERS=h:p[,h:p...],
 *      LUMABRI_BLOCK_MIB (default 8), LUMABRI_STATS=seconds (default off).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "lumabri_proto.h"
#include "lumabri_sha.h"

#define FD_LIMIT      65536
#define MAX_RPEERS    32
#define MAX_FPEERS    8
#define POOL_SOCKS    4

/* ---- real libc entry points ------------------------------------------- */

static int    (*real_open)(const char *, int, ...);
static int    (*real_open64)(const char *, int, ...);
static int    (*real_openat)(int, const char *, int, ...);
static int    (*real_openat64)(int, const char *, int, ...);
static FILE  *(*real_fopen)(const char *, const char *);
static FILE  *(*real_fopen64)(const char *, const char *);
static DIR   *(*real_opendir)(const char *);
static ssize_t(*real_pread)(int, void *, size_t, off_t);
static ssize_t(*real_pread64)(int, void *, size_t, off_t);
static ssize_t(*real_read)(int, void *, size_t);
static int    (*real_close)(int);
static void  *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void  *(*real_mmap64)(void *, size_t, int, int, int, off_t);

static void shim_resolve(void) {
    if (real_open) return;
    real_open64   = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open64");
    real_openat   = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
    real_openat64 = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat64");
    real_fopen    = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    real_fopen64  = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen64");
    real_opendir  = (DIR *(*)(const char *))dlsym(RTLD_NEXT, "opendir");
    real_pread64  = (ssize_t (*)(int, void *, size_t, off_t))dlsym(RTLD_NEXT, "pread64");
    real_read     = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    real_close    = (int (*)(int))dlsym(RTLD_NEXT, "close");
    real_mmap     = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    real_mmap64   = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap64");
    real_pread    = (ssize_t (*)(int, void *, size_t, off_t))dlsym(RTLD_NEXT, "pread");
    /* last, and last assigned: real_open doubles as the "resolved" flag */
    real_open     = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
}

/* forward: g is defined below; the ctor only resolves symbols and learns the
 * virtual root — everything that can fail or touch the network is deferred
 * to the first access under the vroot (shim_init). */
static void shim_learn_vroot(void);

__attribute__((constructor)) static void shim_ctor(void) {
    shim_resolve();
    shim_learn_vroot();
}

/* ---- state ------------------------------------------------------------- */

typedef struct {
    char addr[64];
    int idle[POOL_SOCKS]; int nidle;
    long rtt_us;          /* measured at init; LONG_MAX = unreachable then */
    pthread_mutex_t lk;
} RPeer;

typedef struct {
    char rel[LMB_PATH_MAX];
    uint64_t size;
    uint32_t nblocks;
    uint8_t *map;         /* 1 = block present in the local mirror */
    uint8_t *inflight;    /* 1 = a thread is fetching it right now */
    int peer_idx[MAX_FPEERS]; int npeers;
    int wfd;              /* mirror write fd, opened on first fetch */
    int map_fd;           /* persisted bitmap */
    uint8_t *hash;        /* ground truth: sha256 per LMB_HASH_CHUNK */
    uint32_t nh;
    int hstate;           /* 0 unknown · 1 have · 2 tracker has none */
    pthread_mutex_t lk;
    pthread_cond_t cv;
} RFile;

static struct {
    int ok;                       /* full init succeeded */
    char vroot[LMB_PATH_MAX]; size_t vroot_len;
    char data_dir[LMB_PATH_MAX], maps_dir[LMB_PATH_MAX];
    uint32_t block;
    RFile *files; int nfiles;
    RPeer peers[MAX_RPEERS]; int npeers;
    RFile *_Atomic fdmap[FD_LIMIT];
    _Atomic uint64_t net_bytes, net_blocks, warm_reads;
    pthread_once_t once;
} g = { .once = PTHREAD_ONCE_INIT };

static void shim_learn_vroot(void) {
    const char *v = getenv("LUMABRI_VROOT");
    if (!v || v[0] != '/') return;              /* unset or relative: disabled */
    snprintf(g.vroot, sizeof g.vroot, "%s", v);
    size_t l = strlen(g.vroot);
    while (l > 1 && g.vroot[l - 1] == '/') g.vroot[--l] = 0;
    g.vroot_len = l;
}

/* ---- small helpers ----------------------------------------------------- */

static void mkdir_p(const char *path) {
    char tmp[LMB_PATH_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void mkdir_parent(const char *path) {
    char tmp[LMB_PATH_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) { *slash = 0; mkdir_p(tmp); }
}

static uint32_t fnv1a(const char *s, uint32_t extra) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int i = 0; i < 4; i++) h = (h ^ (uint8_t)(extra >> (8 * i))) * 16777619u;
    return h;
}

/* rel path inside the vroot, or NULL if the path is not ours.
 * "" means the vroot directory itself. */
static const char *vrel(const char *path) {
    if (!g.vroot_len || !path || path[0] != '/') return NULL;
    if (strncmp(path, g.vroot, g.vroot_len)) return NULL;
    if (path[g.vroot_len] == 0) return "";
    if (path[g.vroot_len] != '/') return NULL;
    const char *r = path + g.vroot_len + 1;
    while (*r == '/') r++;
    return r;
}

static void data_path(char *dst, size_t cap, const char *rel) {
    if (rel[0]) snprintf(dst, cap, "%s/%s", g.data_dir, rel);
    else        snprintf(dst, cap, "%s", g.data_dir);
}

static int rfile_cmp(const void *a, const void *b) {
    return strcmp(((const RFile *)a)->rel, ((const RFile *)b)->rel);
}

static RFile *rfind(const char *rel) {
    if (!g.nfiles || !rel[0]) return NULL;
    RFile key;
    snprintf(key.rel, sizeof key.rel, "%s", rel);
    return (RFile *)bsearch(&key, g.files, (size_t)g.nfiles, sizeof(RFile), rfile_cmp);
}

/* ---- placement --------------------------------------------------------- */

static int peer_add(const char *addr) {
    for (int i = 0; i < g.npeers; i++)
        if (!strcmp(g.peers[i].addr, addr)) return i;
    if (g.npeers == MAX_RPEERS) return -1;
    RPeer *p = &g.peers[g.npeers];
    snprintf(p->addr, sizeof p->addr, "%s", addr);
    p->nidle = 0;
    p->rtt_us = 0;        /* all-equal until probed: FNV spreading as before */
    pthread_mutex_init(&p->lk, NULL);
    return g.npeers++;
}

static RFile *file_add(const char *rel, uint64_t size) {
    for (int i = 0; i < g.nfiles; i++)
        if (!strcmp(g.files[i].rel, rel)) {
            if (g.files[i].size != size) {
                fprintf(stderr, "[lumabri] size conflict on %s (%llu vs %llu) — "
                        "keeping the first announcement\n", rel,
                        (unsigned long long)g.files[i].size, (unsigned long long)size);
                return NULL;
            }
            return &g.files[i];
        }
    RFile *nf = (RFile *)realloc(g.files, (size_t)(g.nfiles + 1) * sizeof(RFile));
    if (!nf) return NULL;
    g.files = nf;
    RFile *f = &g.files[g.nfiles++];
    memset(f, 0, sizeof *f);
    snprintf(f->rel, sizeof f->rel, "%s", rel);
    f->size = size;
    f->wfd = f->map_fd = -1;
    return f;
}

static void file_link_peer(RFile *f, int pi) {
    if (!f || pi < 0) return;
    for (int i = 0; i < f->npeers; i++) if (f->peer_idx[i] == pi) return;
    if (f->npeers < MAX_FPEERS) f->peer_idx[f->npeers++] = pi;
}

static int placement_from_tracker(const char *tracker) {
    LmbMsg m = {0};
    LmbBuf b = {0};
    const char *model = getenv("LUMABRI_MODEL");   /* optional filter */
    if (model && model[0]) lmb_buf_str(&b, model);
    int rc = lmb_request(tracker, LMB_PLACEMENT, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_PLACEMENT_R) {
        if (m.body || m.pay) lmb_msg_free(&m);
        return -1;
    }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    rc = lmb_cur_u32(&c, &n) ? -1 : 0;
    for (uint32_t i = 0; rc == 0 && i < n; i++) {
        char rel[LMB_PATH_MAX], addr[64];
        uint64_t size; uint16_t np;
        if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &size) ||
            lmb_cur_u16(&c, &np)) { rc = -1; break; }
        RFile *f = file_add(rel, size);
        for (uint16_t p = 0; p < np; p++) {
            if (lmb_cur_str(&c, addr, sizeof addr)) { rc = -1; break; }
            file_link_peer(f, peer_add(addr));
        }
    }
    lmb_msg_free(&m);
    return rc == 0 && g.nfiles > 0 ? 0 : -1;
}

static int placement_from_peer(const char *addr) {
    LmbMsg m = {0};
    if (lmb_request(addr, LMB_MANIFEST, NULL, 0, &m) || m.op != LMB_MANIFEST_R) {
        if (m.body || m.pay) lmb_msg_free(&m);
        return -1;
    }
    int pi = peer_add(addr);
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    int rc = lmb_cur_u32(&c, &n) ? -1 : 0;
    for (uint32_t i = 0; rc == 0 && i < n; i++) {
        char rel[LMB_PATH_MAX]; uint64_t size;
        if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &size)) { rc = -1; break; }
        file_link_peer(file_add(rel, size), pi);
    }
    lmb_msg_free(&m);
    return rc;
}

/* The manifest survives on disk so a warm cache keeps working with every
 * peer AND the tracker offline: reads of present blocks never need the
 * network; only a miss does, and a miss with no peers is a loud EIO. */
static void manifest_save(void) {
    char path[LMB_PATH_MAX * 2], tmp[LMB_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/manifest.txt", g.maps_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *fp = real_fopen(tmp, "w");
    if (!fp) return;
    for (int i = 0; i < g.nfiles; i++)
        fprintf(fp, "%llu\t%s\n", (unsigned long long)g.files[i].size, g.files[i].rel);
    fclose(fp);
    rename(tmp, path);
}

static int manifest_load(void) {
    char path[LMB_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/manifest.txt", g.maps_dir);
    FILE *fp = real_fopen(path, "r");
    if (!fp) return -1;
    char line[LMB_PATH_MAX + 64];
    while (fgets(line, sizeof line, fp)) {
        unsigned long long size;
        char rel[LMB_PATH_MAX];
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        size = strtoull(line, NULL, 10);
        snprintf(rel, sizeof rel, "%s", tab + 1);
        size_t l = strlen(rel);
        if (l && rel[l - 1] == '\n') rel[l - 1] = 0;
        if (rel[0]) file_add(rel, size);
    }
    fclose(fp);
    return g.nfiles ? 0 : -1;
}

/* ---- the distance map ---------------------------------------------------
 * Every chatter measures its own edges: two PINGs per peer (the second one
 * rides the warm connection; the min is the distance), all peers probed in
 * parallel at init. The tracker stays a Napster index and never needs to
 * know where anyone is — proximity lives with the node that benefits from
 * it, which is the only place it can be measured honestly anyway. */

static void *probe_thread(void *arg) {
    RPeer *p = (RPeer *)arg;
    p->rtt_us = LONG_MAX;
    int fd = lmb_connect_ms(p->addr, 2000);
    if (fd < 0) return NULL;
    if (lmb_auth(fd)) { real_close(fd); return NULL; }
    for (int i = 0; i < 2; i++) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        LmbMsg m = {0};
        if (lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) || lmb_recv(fd, &m)) {
            real_close(fd);
            return NULL;
        }
        lmb_msg_free(&m);
        clock_gettime(CLOCK_MONOTONIC, &b);
        long us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000;
        if (us < p->rtt_us) p->rtt_us = us;
    }
    pthread_mutex_lock(&p->lk);
    if (p->nidle < POOL_SOCKS) { p->idle[p->nidle++] = fd; fd = -1; }
    pthread_mutex_unlock(&p->lk);
    if (fd >= 0) real_close(fd);
    return NULL;
}

static void probe_peers(void) {
    pthread_t t[MAX_RPEERS];
    int started[MAX_RPEERS];
    for (int i = 0; i < g.npeers; i++)
        started[i] = pthread_create(&t[i], NULL, probe_thread, &g.peers[i]) == 0;
    for (int i = 0; i < g.npeers; i++)
        if (started[i]) pthread_join(t[i], NULL);
}

/* ---- prefetch ------------------------------------------------------------
 * The Spotify move: bytes of a model load are overwhelmingly sequential, so
 * while the engine chews on block N the swarm is already sending N+1..N+K.
 * A small queue feeds worker threads that call the same ensure_block the
 * read path uses — the in-flight map dedups, the bitmap makes a wasted
 * prefetch impossible to double-fetch, and a prefetch failure is silent
 * because the read path will retry it loudly if the block is ever needed.
 * LUMABRI_PREFETCH sets the readahead depth in blocks (default 2, 0 = off). */

#define PF_QLEN 64
static struct {
    RFile *f; uint32_t blk;
} pf_q[PF_QLEN];
static int pf_head, pf_tail, pf_depth;
static pthread_mutex_t pf_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pf_cv = PTHREAD_COND_INITIALIZER;

static int ensure_block(RFile *f, uint32_t blk);      /* fwd */

static void *prefetch_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pf_lk);
        while (pf_head == pf_tail) pthread_cond_wait(&pf_cv, &pf_lk);
        RFile *f = pf_q[pf_tail].f;
        uint32_t blk = pf_q[pf_tail].blk;
        pf_tail = (pf_tail + 1) % PF_QLEN;
        pthread_mutex_unlock(&pf_lk);
        ensure_block(f, blk);
    }
    return NULL;
}

/* queue the blocks after the range just read; full queue = drop, no harm */
static void prefetch_after(RFile *f, uint64_t off, uint64_t len) {
    if (!pf_depth || !f->nblocks) return;
    uint64_t end = off + len;
    if (end > f->size) end = f->size;
    uint32_t last = (uint32_t)((end ? end - 1 : 0) / g.block);
    int queued = 0;
    pthread_mutex_lock(&pf_lk);
    for (uint32_t b = last + 1; b < f->nblocks && queued < pf_depth; b++) {
        if (f->map[b] || f->inflight[b]) continue;    /* benign unlocked peek */
        int nh = (pf_head + 1) % PF_QLEN;
        if (nh == pf_tail) break;
        pf_q[pf_head].f = f;
        pf_q[pf_head].blk = b;
        pf_head = nh;
        queued++;
    }
    if (queued) pthread_cond_broadcast(&pf_cv);
    pthread_mutex_unlock(&pf_lk);
}

/* ---- stats -------------------------------------------------------------- */

static void *stats_thread(void *arg) {
    int period = (int)(intptr_t)arg;
    uint64_t last_bytes = 0;
    for (;;) {
        sleep((unsigned)period);
        uint64_t nb = atomic_load(&g.net_bytes);
        uint64_t blk = atomic_load(&g.net_blocks);
        uint64_t warm = atomic_load(&g.warm_reads);
        if (nb == last_bytes && !warm) continue;
        fprintf(stderr, "[lumabri] net %.1f MB in %llu blocks (%.1f MB/s) · warm preads %llu\n",
                (double)nb / 1e6, (unsigned long long)blk,
                (double)(nb - last_bytes) / 1e6 / period, (unsigned long long)warm);
        last_bytes = nb;
    }
    return NULL;
}

/* ---- init --------------------------------------------------------------- */

static void shim_init_impl(void) {
    shim_resolve();
    const char *cache = getenv("LUMABRI_CACHE");
    if (!cache || !cache[0]) {
        fprintf(stderr, "[lumabri] LUMABRI_VROOT is set but LUMABRI_CACHE is not — disabled\n");
        return;
    }
    snprintf(g.data_dir, sizeof g.data_dir, "%s/data", cache);
    snprintf(g.maps_dir, sizeof g.maps_dir, "%s/maps", cache);
    mkdir_p(g.data_dir);
    mkdir_p(g.maps_dir);

    long mib = 8;
    const char *bs = getenv("LUMABRI_BLOCK_MIB");
    if (bs && atol(bs) >= 1 && atol(bs) <= 64) mib = atol(bs);
    g.block = (uint32_t)(mib << 20);

    /* placement: tracker, or peers directly, or the persisted manifest */
    const char *tracker = getenv("LUMABRI_TRACKER");
    const char *peers = getenv("LUMABRI_PEERS");
    int placed = -1;
    if (tracker && tracker[0]) placed = placement_from_tracker(tracker);
    if (placed && peers && peers[0]) {
        char list[1024];
        snprintf(list, sizeof list, "%s", peers);
        placed = -1;
        for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save))
            if (placement_from_peer(tok) == 0) placed = 0;
    }
    if (placed == 0) {
        qsort(g.files, (size_t)g.nfiles, sizeof(RFile), rfile_cmp);
        manifest_save();
    } else {
        if (manifest_load()) {
            fprintf(stderr, "[lumabri] no tracker, no peers, no saved manifest — disabled\n");
            return;
        }
        qsort(g.files, (size_t)g.nfiles, sizeof(RFile), rfile_cmp);
        fprintf(stderr, "[lumabri] offline: serving from the local mirror only "
                        "(a cold block will be EIO)\n");
    }

    /* materialize the sparse mirror + load the block maps */
    uint64_t total = 0, have = 0;
    for (int i = 0; i < g.nfiles; i++) {
        RFile *f = &g.files[i];
        f->nblocks = (uint32_t)((f->size + g.block - 1) / g.block);
        pthread_mutex_init(&f->lk, NULL);
        pthread_cond_init(&f->cv, NULL);
        f->wfd = -1;

        char path[LMB_PATH_MAX * 2];
        data_path(path, sizeof path, f->rel);
        mkdir_parent(path);
        int fd = real_open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { fprintf(stderr, "[lumabri] cannot create mirror %s\n", path); return; }
        struct stat st;
        int stale = fstat(fd, &st) == 0 && st.st_size != 0 &&
                    (uint64_t)st.st_size != f->size;
        if (ftruncate(fd, (off_t)f->size))
            { fprintf(stderr, "[lumabri] ftruncate %s failed\n", path); real_close(fd); return; }
        real_close(fd);

        snprintf(path, sizeof path, "%s/%s.lmap", g.maps_dir, f->rel);
        mkdir_parent(path);
        f->map = (uint8_t *)calloc(f->nblocks ? f->nblocks : 1, 1);
        f->inflight = (uint8_t *)calloc(f->nblocks ? f->nblocks : 1, 1);
        f->map_fd = real_open(path, O_RDWR | O_CREAT, 0644);
        if (!f->map || !f->inflight || f->map_fd < 0)
            { fprintf(stderr, "[lumabri] map alloc failed for %s\n", f->rel); return; }
        if (stale) {
            /* the upstream file changed size: every cached block is suspect */
            fprintf(stderr, "[lumabri] %s changed size upstream — mirror reset\n", f->rel);
            if (ftruncate(f->map_fd, 0)) { /* map cleared, blocks refetch */ }
        }
        if (ftruncate(f->map_fd, (off_t)(f->nblocks ? f->nblocks : 1))) { /* keep going */ }
        ssize_t r = real_pread ? real_pread(f->map_fd, f->map, f->nblocks, 0) : -1;
        if (r < 0) memset(f->map, 0, f->nblocks);
        total += f->size;
        for (uint32_t b = 0; b < f->nblocks; b++)
            if (f->map[b]) have += b + 1 == f->nblocks ? f->size - (uint64_t)b * g.block : g.block;
    }

    /* measure the distance map, then start the readahead workers */
    probe_peers();
    for (int i = 0; i < g.npeers; i++) {
        if (g.peers[i].rtt_us == LONG_MAX)
            fprintf(stderr, "[lumabri] peer %s: unreachable directly (relay or failover)\n",
                    g.peers[i].addr);
        else
            fprintf(stderr, "[lumabri] peer %s: rtt %.2f ms\n",
                    g.peers[i].addr, (double)g.peers[i].rtt_us / 1000.0);
    }
    pf_depth = 2;
    const char *pf = getenv("LUMABRI_PREFETCH");
    if (pf) pf_depth = atoi(pf);
    if (pf_depth < 0) pf_depth = 0;
    if (pf_depth > 16) pf_depth = 16;
    if (pf_depth && g.npeers) {
        int nw = pf_depth < 4 ? pf_depth : 4;
        for (int i = 0; i < nw; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, prefetch_thread, NULL) == 0)
                pthread_detach(t);
        }
    }

    const char *stats = getenv("LUMABRI_STATS");
    if (stats && atoi(stats) > 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, stats_thread, (void *)(intptr_t)atoi(stats)) == 0)
            pthread_detach(t);
    }
    fprintf(stderr, "[lumabri] %d files · %.1f GB · %.1f%% already local · "
                    "%d peer(s) · block %u MiB · prefetch %d\n",
            g.nfiles, (double)total / 1e9,
            total ? 100.0 * (double)have / (double)total : 0.0,
            g.npeers, g.block >> 20, pf_depth);
    g.ok = 1;
}

static void shim_init(void) { pthread_once(&g.once, shim_init_impl); }

/* ---- block transfer ----------------------------------------------------- */

/* One READ against one peer over a pooled connection. Returns a malloc'd
 * buffer of exactly `len` bytes, or NULL. */
static uint8_t *peer_fetch(RPeer *p, const char *rel, uint64_t off, uint32_t len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&p->lk);
        int fd = p->nidle ? p->idle[--p->nidle] : -1;
        pthread_mutex_unlock(&p->lk);
        if (fd < 0) {
            fd = lmb_connect_ms(p->addr, 2500);
            if (fd < 0) return NULL;             /* unreachable: relay decides */
            if (lmb_auth(fd)) { real_close(fd); return NULL; }
        }

        LmbBuf b = {0};
        lmb_buf_str(&b, rel); lmb_buf_u64(&b, off); lmb_buf_u32(&b, len);
        int rc = lmb_send(fd, LMB_READ, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        LmbMsg m = {0};
        if (rc == 0) rc = lmb_recv(fd, &m);
        if (rc != 0) { real_close(fd); continue; }   /* dead socket: retry fresh */

        if (m.op == LMB_READ_R && m.pay_len == len) {
            uint8_t *data = m.pay;
            m.pay = NULL;                       /* steal the payload */
            lmb_msg_free(&m);
            pthread_mutex_lock(&p->lk);
            if (p->nidle < POOL_SOCKS) p->idle[p->nidle++] = fd;
            else { pthread_mutex_unlock(&p->lk); real_close(fd); return data; }
            pthread_mutex_unlock(&p->lk);
            return data;
        }
        /* protocol-level refusal (ERR, short pay): the stream is consistent,
         * keep the socket, but this peer cannot serve the block */
        lmb_msg_free(&m);
        pthread_mutex_lock(&p->lk);
        if (p->nidle < POOL_SOCKS) p->idle[p->nidle++] = fd;
        else { pthread_mutex_unlock(&p->lk); real_close(fd); return NULL; }
        pthread_mutex_unlock(&p->lk);
        return NULL;
    }
    return NULL;
}

/* The relay: when no peer can be dialed directly (typical for a maintainer
 * behind a home NAT), the block is fetched through the tracker, which
 * forwards the request down the maintainer's own outbound control
 * connection. Slower per request, but it means the swarm works with zero
 * router configuration — and the direct path stays the first choice. */
static uint8_t *relay_fetch(const char *rel, uint64_t off, uint32_t len) {
    const char *tracker = getenv("LUMABRI_TRACKER");
    if (!tracker || !tracker[0]) return NULL;
    const char *model = getenv("LUMABRI_MODEL");
    LmbBuf b = {0};
    lmb_buf_str(&b, model ? model : "");
    lmb_buf_str(&b, rel);
    lmb_buf_u64(&b, off);
    lmb_buf_u32(&b, len);
    LmbMsg m = {0};
    int rc = lmb_request(tracker, LMB_RREAD, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_RREAD_R || m.pay_len != len) {
        lmb_msg_free(&m);
        return NULL;
    }
    uint8_t *data = m.pay;
    m.pay = NULL;
    lmb_msg_free(&m);
    return data;
}

/* ---- integrity ----------------------------------------------------------
 * The ground truth for a file comes from the TRACKER (which took it from
 * the origin's first registration), never from the peer that serves the
 * bytes — so a lying peer cannot vouch for itself. Every fetched block is
 * hashed per LMB_HASH_CHUNK and compared; a mismatch is treated as a failed
 * fetch and the block is refetched from the next peer, loudly. A swarm
 * without integrity data still works (warned once per file);
 * LUMABRI_REQUIRE_HASH=1 turns that into a hard EIO for untrusted swarms. */

static void hashes_ensure(RFile *f) {
    pthread_mutex_lock(&f->lk);
    while (f->hstate < 0) pthread_cond_wait(&f->cv, &f->lk);
    int state = f->hstate;
    if (!state) f->hstate = -1;              /* claim the fetch */
    pthread_mutex_unlock(&f->lk);
    if (state > 0) return;

    uint8_t *hash = NULL;
    uint32_t nh = 0;
    const char *tracker = getenv("LUMABRI_TRACKER");
    if (tracker && tracker[0]) {
        const char *model = getenv("LUMABRI_MODEL");
        LmbBuf b = {0};
        lmb_buf_str(&b, model ? model : "");
        lmb_buf_str(&b, f->rel);
        LmbMsg m = {0};
        int rc = lmb_request(tracker, LMB_HASHES, b.p, (uint32_t)b.len, &m);
        free(b.p);
        if (rc == 0 && m.op == LMB_HASHES_R) {
            LmbCur c = { m.body, m.body_len, 0 };
            uint32_t chunk = 0, n = 0;
            if (!lmb_cur_u32(&c, &chunk) && !lmb_cur_u32(&c, &n) &&
                chunk == LMB_HASH_CHUNK && m.pay_len == n * 32 &&
                n == (uint32_t)((f->size + LMB_HASH_CHUNK - 1) / LMB_HASH_CHUNK)) {
                hash = m.pay; nh = n; m.pay = NULL;
            }
        }
        lmb_msg_free(&m);
    }
    pthread_mutex_lock(&f->lk);
    f->hash = hash; f->nh = nh;
    f->hstate = hash ? 1 : 2;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->lk);
    if (!hash)
        fprintf(stderr, "[lumabri] no integrity data for %s — fetches are "
                        "UNVERIFIED%s\n", f->rel,
                getenv("LUMABRI_REQUIRE_HASH") ? " and LUMABRI_REQUIRE_HASH is set"
                                               : "");
}

/* 0 = the bytes match the truth (or there is none to check against) */
static int block_verify(RFile *f, uint64_t off, const uint8_t *data, uint32_t len) {
    if (f->hstate != 1) return 0;
    for (uint32_t o = 0; o < len; o += LMB_HASH_CHUNK) {
        uint32_t ci = (uint32_t)((off + o) / LMB_HASH_CHUNK);
        uint32_t pl = len - o < LMB_HASH_CHUNK ? len - o : LMB_HASH_CHUNK;
        uint8_t h[32];
        if (ci >= f->nh) return -1;
        lmb_sha256(data + o, pl, h);
        if (memcmp(h, f->hash + (size_t)ci * 32, 32)) return -1;
    }
    return 0;
}

/* Make one block present in the mirror. 0 on success. */
static int ensure_block(RFile *f, uint32_t blk) {
    pthread_mutex_lock(&f->lk);
    for (;;) {
        if (f->map[blk]) { pthread_mutex_unlock(&f->lk); return 0; }
        if (!f->inflight[blk]) break;
        pthread_cond_wait(&f->cv, &f->lk);
    }
    f->inflight[blk] = 1;
    if (f->wfd < 0) {
        char path[LMB_PATH_MAX * 2];
        data_path(path, sizeof path, f->rel);
        f->wfd = real_open(path, O_RDWR);
    }
    int wfd = f->wfd;
    pthread_mutex_unlock(&f->lk);

    uint64_t off = (uint64_t)blk * g.block;
    uint32_t len = (uint32_t)(off + g.block <= f->size ? g.block : f->size - off);
    uint8_t *data = NULL;
    hashes_ensure(f);
    int require = getenv("LUMABRI_REQUIRE_HASH") != NULL;
    if (require && f->hstate != 1) {
        fprintf(stderr, "[lumabri] block %u of %s: integrity required but "
                        "unavailable — refusing the fetch\n", blk, f->rel);
    } else if (f->npeers > 0) {
        /* nearest first: the file's peers sorted by measured distance; peers
         * within 25% + 2 ms of the best are "equally near" and the block
         * spreads over them by hash (load balance among true replicas);
         * everyone farther is failover, in distance order, relay last. */
        int ord[MAX_FPEERS];
        int n = f->npeers;
        for (int i = 0; i < n; i++) ord[i] = f->peer_idx[i];
        for (int i = 1; i < n; i++) {
            int v = ord[i], j = i;
            while (j > 0 && g.peers[ord[j - 1]].rtt_us > g.peers[v].rtt_us)
                { ord[j] = ord[j - 1]; j--; }
            ord[j] = v;
        }
        long best = g.peers[ord[0]].rtt_us;
        int nnear = 1;
        while (nnear < n && best != LONG_MAX &&
               g.peers[ord[nnear]].rtt_us <= best + best / 4 + 2000)
            nnear++;
        uint32_t start = fnv1a(f->rel, blk) % (uint32_t)nnear;
        for (int i = 0; i < n && !data; i++) {
            int pick = i < nnear ? (int)((start + (uint32_t)i) % (uint32_t)nnear) : i;
            data = peer_fetch(&g.peers[ord[pick]], f->rel, off, len);
            if (data && block_verify(f, off, data, len)) {
                fprintf(stderr, "[lumabri] peer %s served CORRUPT bytes for "
                        "block %u of %s — rejected, trying elsewhere\n",
                        g.peers[ord[pick]].addr, blk, f->rel);
                free(data);
                data = NULL;
            }
        }
    }
    if (!data && !(require && f->hstate != 1)) {
        data = relay_fetch(f->rel, off, len);          /* NAT floor */
        if (data && block_verify(f, off, data, len)) {
            fprintf(stderr, "[lumabri] relay served CORRUPT bytes for "
                    "block %u of %s — rejected\n", blk, f->rel);
            free(data);
            data = NULL;
        }
    }
    int ok = 0;
    if (data && wfd >= 0) {
        uint32_t put = 0;
        while (put < len) {
            ssize_t w = pwrite(wfd, data + put, len - put, (off_t)(off + put));
            if (w < 0) { if (errno == EINTR) continue; break; }
            put += (uint32_t)w;
        }
        if (put == len) {
            uint8_t one = 1;
            ok = pwrite(f->map_fd, &one, 1, (off_t)blk) == 1;
        }
        atomic_fetch_add(&g.net_bytes, len);
        atomic_fetch_add(&g.net_blocks, 1);
    }
    free(data);
    if (!ok)
        fprintf(stderr, "[lumabri] block %u of %s: no peer could serve it\n", blk, f->rel);

    pthread_mutex_lock(&f->lk);
    f->inflight[blk] = 0;
    if (ok) f->map[blk] = 1;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->lk);
    return ok ? 0 : -1;
}

static int ensure_range(RFile *f, uint64_t off, uint64_t len) {
    if (off >= f->size || len == 0) return 0;
    uint64_t end = off + len;
    if (end > f->size) end = f->size;
    for (uint32_t b = (uint32_t)(off / g.block); b <= (uint32_t)((end - 1) / g.block); b++)
        if (ensure_block(f, b)) return -1;
    return 0;
}

static int ensure_full(RFile *f) { return ensure_range(f, 0, f->size); }

/* ---- fd table ----------------------------------------------------------- */

static void fdmap_set(int fd, RFile *f) {
    if (fd < 0 || fd >= FD_LIMIT) return;
    g.fdmap[fd] = f;
}

static RFile *fdmap_get(int fd) {
    return fd >= 0 && fd < FD_LIMIT ? g.fdmap[fd] : NULL;
}

/* ---- interposed libc ---------------------------------------------------- */

static int open_common(const char *path, int flags, mode_t mode) {
    const char *rel = vrel(path);
    if (!rel) return real_open(path, flags, mode);
    shim_init();
    if (!g.ok) { errno = EIO; return -1; }
    char cpath[LMB_PATH_MAX * 2];
    data_path(cpath, sizeof cpath, rel);
    RFile *f = rfind(rel);
    if (f && (flags & (O_WRONLY | O_RDWR | O_TRUNC))) { errno = EROFS; return -1; }
    if (!f && (flags & O_CREAT)) mkdir_parent(cpath);
    int fd = real_open(cpath, flags, mode);
    if (fd >= FD_LIMIT && f) { real_close(fd); errno = EMFILE; return -1; }
    if (fd >= 0 && f) fdmap_set(fd, f);
    return fd;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned);
        va_end(ap);
    }
    shim_resolve();
    return open_common(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned);
        va_end(ap);
    }
    shim_resolve();
    if (!vrel(path)) return real_open64 ? real_open64(path, flags, mode)
                                        : real_open(path, flags, mode);
    return open_common(path, flags | O_LARGEFILE, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned);
        va_end(ap);
    }
    shim_resolve();
    if (path && path[0] == '/' && vrel(path)) return open_common(path, flags, mode);
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned);
        va_end(ap);
    }
    shim_resolve();
    if (path && path[0] == '/' && vrel(path)) return open_common(path, flags | O_LARGEFILE, mode);
    return real_openat64 ? real_openat64(dirfd, path, flags, mode)
                         : real_openat(dirfd, path, flags, mode);
}

static FILE *fopen_common(const char *path, const char *mode, FILE *(*fn)(const char *, const char *)) {
    const char *rel = vrel(path);
    if (!rel) return fn(path, mode);
    shim_init();
    if (!g.ok) { errno = EIO; return NULL; }
    char cpath[LMB_PATH_MAX * 2];
    data_path(cpath, sizeof cpath, rel);
    RFile *f = rfind(rel);
    if (f) {
        if (strpbrk(mode, "wa+")) { errno = EROFS; return NULL; }
        /* stdio streams read sequentially behind our back; a model file
         * opened with fopen is small (config/tokenizer) — fetch it whole
         * so every later fread is a plain local read */
        if (ensure_full(f)) { errno = EIO; return NULL; }
    } else if (strpbrk(mode, "wa")) {
        mkdir_parent(cpath);
    }
    return fn(cpath, mode);
}

FILE *fopen(const char *path, const char *mode) {
    shim_resolve();
    return fopen_common(path, mode, real_fopen);
}

FILE *fopen64(const char *path, const char *mode) {
    shim_resolve();
    return fopen_common(path, mode, real_fopen64 ? real_fopen64 : real_fopen);
}

DIR *opendir(const char *path) {
    shim_resolve();
    const char *rel = vrel(path);
    if (!rel) return real_opendir(path);
    shim_init();
    if (!g.ok) { errno = EIO; return NULL; }
    char cpath[LMB_PATH_MAX * 2];
    data_path(cpath, sizeof cpath, rel);
    return real_opendir(cpath);
}

ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    shim_resolve();
    RFile *f = fdmap_get(fd);
    if (f) {
        if (ensure_range(f, (uint64_t)off, n)) { errno = EIO; return -1; }
        atomic_fetch_add(&g.warm_reads, 1);
        prefetch_after(f, (uint64_t)off, n);
    }
    return real_pread(fd, buf, n, off);
}

ssize_t pread64(int fd, void *buf, size_t n, off_t off) {
    shim_resolve();
    RFile *f = fdmap_get(fd);
    if (f) {
        if (ensure_range(f, (uint64_t)off, n)) { errno = EIO; return -1; }
        atomic_fetch_add(&g.warm_reads, 1);
        prefetch_after(f, (uint64_t)off, n);
    }
    return real_pread64 ? real_pread64(fd, buf, n, off) : real_pread(fd, buf, n, off);
}

ssize_t read(int fd, void *buf, size_t n) {
    shim_resolve();
    RFile *f = fdmap_get(fd);
    if (f) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        if (cur >= 0 && ensure_range(f, (uint64_t)cur, n)) { errno = EIO; return -1; }
        if (cur >= 0) prefetch_after(f, (uint64_t)cur, n);
    }
    return real_read(fd, buf, n);
}

/* mmap on a mirror fd would hand the engine raw sparse-file pages: a block
 * never fetched reads as zeros with no fault we could catch — the exact
 * silent corruption the EIO rule exists to prevent. So the mapped range is
 * materialized first; if it cannot be, the map fails loudly. */
static void *mmap_common(void *addr, size_t len, int prot, int flags, int fd,
                         off_t off, void *(*fn)(void *, size_t, int, int, int, off_t)) {
    RFile *f = fdmap_get(fd);
    if (f && ensure_range(f, (uint64_t)off, len)) { errno = EIO; return MAP_FAILED; }
    return fn(addr, len, prot, flags, fd, off);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    shim_resolve();
    return mmap_common(addr, len, prot, flags, fd, off, real_mmap);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    shim_resolve();
    return mmap_common(addr, len, prot, flags, fd, off,
                       real_mmap64 ? real_mmap64 : real_mmap);
}

int close(int fd) {
    shim_resolve();
    if (fdmap_get(fd)) fdmap_set(fd, NULL);
    return real_close(fd);
}
