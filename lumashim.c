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
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#define LMB_SECURE_NO_CLOSE_REDIRECT 1
#include "lumabri_proto.h"
#include "lumabri_secure.h"
#include "lumabri_sha.h"
#include "lumabri_sign.h"

#define FD_LIMIT      65536
#define MAX_RPEERS    32
#define MAX_FPEERS    8
#define POOL_SOCKS    4
#define TRUTH_MAGIC   "LMBTRTH1"
#define TRUTH_VERSION 1u
#define LMB_CACHE_PATH_MAX (LMB_PATH_MAX * 2 + 128)

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
    /* Non-empty: the advertised address is this very machine seen from the
     * outside (a maintainer donated next to this chat, advertised at the
     * public IP the tracker observed — a NAT rarely hairpins). Sockets dial
     * this; addr stays the identity. Every fetched block is hash-verified
     * downstream, so a wrong local service can only fail, never lie. */
    char dial[64];
    int idle[POOL_SOCKS]; int nidle;
    long rtt_us;          /* measured at init; LONG_MAX = unreachable then */
    pthread_mutex_t lk;
} RPeer;

typedef struct {
    char rel[LMB_PATH_MAX];
    uint64_t size;
    uint32_t nblocks;
    uint8_t *map;         /* 1 = block present in the local mirror */
    uint8_t *verified;    /* process-local: present block matched current truth */
    uint8_t *inflight;    /* 1 = a thread is fetching it right now */
    int peer_idx[MAX_FPEERS]; int npeers;
    int wfd;              /* mirror write fd, opened on first fetch */
    int map_fd;           /* persisted bitmap */
    uint64_t dirty_gen, durable_gen; /* batched data-before-map commits */
    uint8_t *hash;        /* ground truth: sha256 per LMB_HASH_CHUNK */
    uint32_t nh;
    int hstate;           /* 0 unknown · 1 have · 2 tracker has none */
    int signed_;          /* the truth carried a valid operator signature */
    int truth_invalid;    /* a persisted truth exists but cannot be used */
    char truth_model[64];
    uint8_t truth_sig[64];
    int truth_has_sig;
    pthread_mutex_t lk;
    pthread_mutex_t commit_lk;
    pthread_cond_t cv;
} RFile;

static struct {
    int ok;                       /* full init succeeded */
    char vroot[LMB_PATH_MAX]; size_t vroot_len;
    char data_dir[LMB_CACHE_PATH_MAX], maps_dir[LMB_CACHE_PATH_MAX];
    char cas_dir[LMB_CACHE_PATH_MAX];
    int cache_lock_fd;              /* shared for process lifetime; exclusive on reset */
    int reset_lock_fd;              /* elects one resetter without queuing behind its chat */
    uint32_t block;
    RFile *files; int nfiles;
    RPeer peers[MAX_RPEERS]; int npeers;
    LmbTrustKeys trust;                    /* old+new keys during rotation */
    RFile *_Atomic fdmap[FD_LIMIT];
    _Atomic uint64_t net_bytes, net_blocks, warm_reads, cas_hits, cas_bytes;
    /* the dense warm-up: bytes of non-expert tensors, and how many of them
     * are in the mirror (already there, or fetched by the warm-up thread) */
    _Atomic uint64_t dense_total, dense_done;
    _Atomic int dense_state;        /* 0 off · 1 running · 2 ready · 3 failed */
    pthread_once_t once;
} g = { .cache_lock_fd = -1, .reset_lock_fd = -1, .once = PTHREAD_ONCE_INIT };

/* Persist bitmap claims in batches.  A fetched block is immediately usable
 * from the in-process map, but the on-disk map is updated only after the data
 * fd has reached stable storage.  This preserves crash safety without paying
 * two fdatasync calls on every MiB, which destroys cold-stream throughput. */
static int mirror_commit_file(RFile *f) {
    pthread_mutex_lock(&f->commit_lk);
    pthread_mutex_lock(&f->lk);
    if (f->dirty_gen == f->durable_gen || f->wfd < 0) {
        pthread_mutex_unlock(&f->lk);
        pthread_mutex_unlock(&f->commit_lk);
        return 0;
    }
    uint64_t target = f->dirty_gen;
    size_t n = f->nblocks ? f->nblocks : 1;
    uint8_t *snapshot = (uint8_t *)calloc(n, 1);
    if (snapshot) memcpy(snapshot, f->map, f->nblocks);
    int wfd = f->wfd, mfd = f->map_fd;
    pthread_mutex_unlock(&f->lk);
    if (!snapshot) { pthread_mutex_unlock(&f->commit_lk); return -1; }

    /* Several chatters may share this mirror.  Serialize the persisted-map
     * merge across processes and OR in bits published by the others, or an
     * older in-memory snapshot could erase their warm blocks. */
    int rc = flock(mfd, LOCK_EX);
    uint8_t *ondisk = NULL;
    if (!rc) rc = fdatasync(wfd);       /* data is durable before any map bit */
    if (!rc) {
        ondisk = (uint8_t *)calloc(n, 1);
        if (!ondisk) rc = -1;
    }
    if (!rc) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = real_pread(mfd, ondisk + got, n - got, (off_t)got);
            if (r < 0) { if (errno == EINTR) continue; rc = -1; break; }
            if (r == 0) break;
            got += (size_t)r;
        }
        for (size_t i = 0; i < n; i++) snapshot[i] |= ondisk[i];
    }
    size_t put = 0;
    while (!rc && put < n) {
        ssize_t w = pwrite(mfd, snapshot + put, n - put, (off_t)put);
        if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
        put += (size_t)w;
    }
    if (!rc && fdatasync(mfd)) rc = -1;
    free(ondisk);
    if (flock(mfd, LOCK_UN) && !rc) rc = -1;
    free(snapshot);
    if (!rc) {
        pthread_mutex_lock(&f->lk);
        if (f->durable_gen < target) f->durable_gen = target;
        pthread_mutex_unlock(&f->lk);
    }
    pthread_mutex_unlock(&f->commit_lk);
    return rc;
}

static void mirror_commit_all(void) {
    for (int i = 0; i < g.nfiles; i++)
        if (mirror_commit_file(&g.files[i]))
            fprintf(stderr, "[lumabri] cannot make the mirror map durable for %s\n",
                    g.files[i].rel);
}

static void *mirror_commit_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        mirror_commit_all();
    }
    return NULL;
}

static int cache_flock(int op) {
    int rc;
    do rc = flock(g.cache_lock_fd, op); while (rc && errno == EINTR);
    return rc;
}

static void cache_reset_unlock(void) {
    if (g.reset_lock_fd < 0) return;
    flock(g.reset_lock_fd, LOCK_UN);
    real_close(g.reset_lock_fd);
    g.reset_lock_fd = -1;
}

static void cache_lock_release(void) {
    if (g.cache_lock_fd >= 0) {
        cache_flock(LOCK_UN);
        real_close(g.cache_lock_fd);
        g.cache_lock_fd = -1;
    }
    cache_reset_unlock();
}

__attribute__((destructor)) static void shim_dtor(void) {
    if (g.ok) mirror_commit_all();
    cache_lock_release();
}

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
    char tmp[LMB_CACHE_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void mkdir_parent(const char *path) {
    char tmp[LMB_CACHE_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) { *slash = 0; mkdir_p(tmp); }
}

static int fsync_parent_dir(const char *path) {
    char dir[LMB_CACHE_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    if (slash == dir) slash[1] = 0;
    else *slash = 0;
    int fd = real_open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    real_close(fd);
    return rc;
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

static int path_printf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) dst[0] = 0;
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int data_path(char *dst, size_t cap, const char *rel) {
    return rel[0] ? path_printf(dst, cap, "%s/%s", g.data_dir, rel)
                  : path_printf(dst, cap, "%s", g.data_dir);
}

static int map_path(char *dst, size_t cap, const RFile *f, const char *ext) {
    return path_printf(dst, cap, "%s/%s.%s", g.maps_dir, f->rel, ext);
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += (size_t)w; n -= (size_t)w;
    }
    return 0;
}

/* ---- persisted truth ----------------------------------------------------
 * The signed hash vector lives only in tracker memory, so a restart used to
 * demote every warm block to "unverifiable".  Persist the exact record as a
 * sidecar next to the map: magic, version, model, path, hash chunk, file
 * size, hash count, signature flag/signature, then the raw hash vector.
 * The sidecar is a courier for the tracker record, never a new authority —
 * offline it is only believed after its signature checks out against the
 * out-of-band operator trust set, exactly like the live record. */
static int truth_save(RFile *f) {
    if (f->hstate != 1) return -1;
    LmbBuf b = {0};
    if (lmb_buf_bytes(&b, (const uint8_t *)TRUTH_MAGIC, 8) ||
        lmb_buf_u32(&b, TRUTH_VERSION) ||
        lmb_buf_str(&b, f->truth_model) || lmb_buf_str(&b, f->rel) ||
        lmb_buf_u32(&b, LMB_HASH_CHUNK) || lmb_buf_u64(&b, f->size) ||
        lmb_buf_u32(&b, f->nh) || lmb_buf_u32(&b, (uint32_t)f->truth_has_sig) ||
        (f->truth_has_sig && lmb_buf_bytes(&b, f->truth_sig, 64)) ||
        lmb_buf_bytes(&b, f->hash, (size_t)f->nh * 32)) {
        free(b.p); return -1;
    }
    char path[LMB_CACHE_PATH_MAX], tmp[LMB_CACHE_PATH_MAX];
    if (map_path(path, sizeof path, f, "truth")) { free(b.p); return -1; }
    mkdir_parent(path);
    static _Atomic uint64_t tmp_seq;
    if (path_printf(tmp, sizeof tmp, "%s.tmp.%ld.%llu", path, (long)getpid(),
                    (unsigned long long)atomic_fetch_add(&tmp_seq, 1))) {
        free(b.p); return -1;
    }
    int fd = real_open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    int rc = fd < 0 || write_all(fd, b.p, b.len) || fdatasync(fd);
    if (fd >= 0 && real_close(fd)) rc = -1;
    if (!rc && rename(tmp, path)) rc = -1;
    if (!rc) (void)fsync_parent_dir(path);
    if (rc) unlink(tmp);
    free(b.p);
    return rc ? -1 : 0;
}

static int truth_sidecar_exists(RFile *f) {
    char path[LMB_CACHE_PATH_MAX];
    if (map_path(path, sizeof path, f, "truth")) return 0;
    struct stat st;
    return lstat(path, &st) == 0;
}

static int truth_load(RFile *f) {
    /* Without an out-of-band key the sidecar is only a cache hint: whoever
     * can rewrite the mirror could rewrite it too.  LUMABRI_REQUIRE_HASH
     * explicitly asks us not to accept that kind of authority. */
    if (getenv("LUMABRI_REQUIRE_HASH") && !g.trust.n) return -1;
    char path[LMB_CACHE_PATH_MAX];
    if (map_path(path, sizeof path, f, "truth")) return -1;
    int fd = real_open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 8 ||
        st.st_size > (off_t)LMB_MAX_PAY + LMB_PATH_MAX + 256) {
        real_close(fd); return -1;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *raw = (uint8_t *)malloc(len);
    if (!raw) { real_close(fd); return -1; }
    ssize_t got = real_pread(fd, raw, len, 0);
    real_close(fd);
    if (got != (ssize_t)len) { free(raw); return -1; }

    LmbCur c = { raw, len, 0 };
    uint8_t magic[8], sig[64];
    uint32_t version = 0, chunk = 0, nh = 0, has_sig = 0;
    uint64_t size = 0;
    char model[64], rel[LMB_PATH_MAX];
    const char *want_model = getenv("LUMABRI_MODEL");
    int bad = lmb_cur_bytes(&c, magic, 8) || memcmp(magic, TRUTH_MAGIC, 8) ||
        lmb_cur_u32(&c, &version) || version != TRUTH_VERSION ||
        lmb_cur_str(&c, model, sizeof model) || lmb_cur_str(&c, rel, sizeof rel) ||
        lmb_cur_u32(&c, &chunk) || lmb_cur_u64(&c, &size) ||
        lmb_cur_u32(&c, &nh) || lmb_cur_u32(&c, &has_sig) || has_sig > 1 ||
        (has_sig && lmb_cur_bytes(&c, sig, 64)) ||
        chunk != LMB_HASH_CHUNK || size != f->size || strcmp(rel, f->rel) ||
        (want_model && want_model[0] && strcmp(want_model, model)) ||
        nh != (uint32_t)(f->size / LMB_HASH_CHUNK +
                         (f->size % LMB_HASH_CHUNK != 0)) ||
        c.off > c.len || c.len - c.off != (size_t)nh * 32;
    if (bad) { free(raw); return -1; }
    uint8_t *hash = (uint8_t *)malloc((size_t)nh * 32);
    if (!hash) { free(raw); return -1; }
    memcpy(hash, raw + c.off, (size_t)nh * 32);
    int signed_ok = 0;
    if (g.trust.n) {
        size_t mlen = 0;
        uint8_t *msg = has_sig
            ? lmb_truth_msg(model, rel, chunk, size, hash, nh, &mlen) : NULL;
        signed_ok = msg && lmb_trust_verify(&g.trust, sig, msg, mlen) == 0;
        free(msg);
        if (!signed_ok) { free(hash); free(raw); return -1; }
    }
    f->hash = hash; f->nh = nh; f->signed_ = signed_ok;
    f->truth_has_sig = (int)has_sig;
    if (has_sig) memcpy(f->truth_sig, sig, 64);
    snprintf(f->truth_model, sizeof f->truth_model, "%s", model);
    free(raw);
    return 0;
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
    p->dial[0] = 0;
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
        if (!lmb_rel_ok(rel)) {      /* a name we would have to escape to honour */
            fprintf(stderr, "[lumabri] tracker offered an unsafe file name — "
                            "refusing the whole placement\n");
            rc = -1; break;
        }
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
        if (!lmb_rel_ok(rel)) {
            fprintf(stderr, "[lumabri] peer %s offered an unsafe file name — "
                            "refusing its manifest\n", addr);
            rc = -1; break;
        }
        file_link_peer(file_add(rel, size), pi);
    }
    lmb_msg_free(&m);
    return rc;
}

/* The manifest survives on disk so a warm cache keeps working with every
 * peer AND the tracker offline: reads of present blocks never need the
 * network; only a miss does, and a miss with no peers is a loud EIO. */
static int manifest_save(void) {
    char path[LMB_CACHE_PATH_MAX], tmp[LMB_CACHE_PATH_MAX];
    if (path_printf(path, sizeof path, "%s/manifest.txt", g.maps_dir) ||
        path_printf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()))
        return -1;
    FILE *fp = real_fopen(tmp, "w");
    if (!fp) return -1;
    for (int i = 0; i < g.nfiles; i++)
        fprintf(fp, "%llu\t%s\n", (unsigned long long)g.files[i].size, g.files[i].rel);
    int ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    if (fclose(fp)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return fsync_parent_dir(path);
}

static int manifest_load(void) {
    char path[LMB_CACHE_PATH_MAX];
    if (path_printf(path, sizeof path, "%s/manifest.txt", g.maps_dir)) return -1;
    FILE *fp = real_fopen(path, "r");
    if (!fp) return -1;
    int first = g.nfiles, bad = 0;
    char line[LMB_PATH_MAX + 64];
    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        if (!len || line[len - 1] != '\n') { bad = 1; break; }
        line[--len] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || strchr(tab + 1, '\t')) { bad = 1; break; }
        *tab++ = 0;
        char *end = NULL;
        errno = 0;
        unsigned long long size = strtoull(line, &end, 10);
        if (line[0] < '0' || line[0] > '9' || errno == ERANGE || !end || *end ||
            !lmb_rel_ok(tab) || !file_add(tab, (uint64_t)size)) {
            bad = 1;
            break;
        }
    }
    if (ferror(fp)) bad = 1;
    fclose(fp);
    if (bad) {
        g.nfiles = first;
        fprintf(stderr, "[lumabri] saved manifest contains an invalid entry - refusing it\n");
        return -1;
    }
    return g.nfiles > first ? 0 : -1;
}

typedef struct {
    uint32_t magic, version;
    char model[64];
    uint8_t root[32];
    uint32_t has_sig;
    uint8_t sig[64];
    uint32_t block;
} CacheIdentity;

static int cache_identity_load(CacheIdentity *rec) {
    char path[LMB_CACHE_PATH_MAX];
    if (path_printf(path, sizeof path, "%s/identity.bin", g.maps_dir)) return -1;
    FILE *fp = real_fopen(path, "rb");
    if (!fp) return -1;
    int ok = fread(rec, sizeof *rec, 1, fp) == 1 &&
             rec->magic == LMB_MODEL_ID_MAGIC && rec->version == 2 &&
             rec->has_sig <= 1 && rec->block >= (1u << 20);
    fclose(fp);
    return ok ? 0 : -1;
}

static int cache_identity_save(const LmbModelIdentity *id) {
    CacheIdentity rec;
    memset(&rec, 0, sizeof rec);
    rec.magic = LMB_MODEL_ID_MAGIC; rec.version = 2;
    snprintf(rec.model, sizeof rec.model, "%s", id->model);
    memcpy(rec.root, id->root, 32);
    rec.has_sig = id->has_sig ? 1u : 0u;
    if (id->has_sig) memcpy(rec.sig, id->sig, 64);
    rec.block = g.block;
    char path[LMB_CACHE_PATH_MAX], tmp[LMB_CACHE_PATH_MAX];
    if (path_printf(path, sizeof path, "%s/identity.bin", g.maps_dir) ||
        path_printf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()))
        return -1;
    FILE *fp = real_fopen(tmp, "wb");
    if (!fp) return -1;
    int ok = fwrite(&rec, sizeof rec, 1, fp) == 1 &&
             fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    if (fclose(fp)) ok = 0;
    if (!ok) { unlink(tmp); return -1; }
    if (rename(tmp, path)) return -1;
    return fsync_parent_dir(path);
}

static int model_identity_signature_ok(const LmbModelIdentity *id) {
    if (!g.trust.n) return 1;
    if (!id->has_sig) return 0;
    size_t n = 0;
    uint8_t *msg = lmb_model_id_msg(id->model, id->root, &n);
    int ok = msg && lmb_trust_verify(&g.trust, id->sig, msg, n) == 0;
    free(msg);
    return ok;
}

static int cache_identity_get(CacheIdentity *rec, LmbModelIdentity *id) {
    memset(rec, 0, sizeof *rec);
    memset(id, 0, sizeof *id);
    if (cache_identity_load(rec) || rec->block != g.block) return -1;
    snprintf(id->model, sizeof id->model, "%s", rec->model);
    memcpy(id->root, rec->root, sizeof id->root);
    id->has_sig = rec->has_sig != 0;
    if (id->has_sig) memcpy(id->sig, rec->sig, sizeof id->sig);
    return model_identity_signature_ok(id) ? 0 : -1;
}

static int cache_identity_differs(const LmbModelIdentity *current,
                                  const LmbModelIdentity *saved, int have_saved) {
    return !have_saved || strcmp(current->model, saved->model) ||
           memcmp(current->root, saved->root, sizeof current->root);
}

static int cache_lock_shared_open(void) {
    char path[LMB_CACHE_PATH_MAX];
    if (path_printf(path, sizeof path, "%s/cache.lock", g.maps_dir)) return -1;
    g.cache_lock_fd = real_open(path, O_RDWR | O_CREAT, 0644);
    return g.cache_lock_fd < 0 || cache_flock(LOCK_SH) ? -1 : 0;
}

static int cache_reset_lock(void) {
    char path[LMB_CACHE_PATH_MAX];
    if (path_printf(path, sizeof path, "%s/reset.lock", g.maps_dir)) return -1;
    g.reset_lock_fd = real_open(path, O_RDWR | O_CREAT, 0644);
    if (g.reset_lock_fd < 0) return -1;
    int rc;
    do rc = flock(g.reset_lock_fd, LOCK_EX); while (rc && errno == EINTR);
    return rc;
}

/* A nonzero file/map with a different expected length means somebody altered
 * the mirror outside Lumabri or an old layout survived.  Repair needs the
 * process-wide exclusive cache lock so a running chatter never observes the
 * truncate between its bitmap check and pread. */
static int cache_has_stale_layout(void) {
    char path[LMB_CACHE_PATH_MAX];
    struct stat st;
    for (int i = 0; i < g.nfiles; i++) {
        RFile *f = &g.files[i];
        if (data_path(path, sizeof path, f->rel)) return 1;
        int have_data = stat(path, &st) == 0;
        int bad_data = have_data && (uint64_t)st.st_size != f->size;
        uint64_t nb = (f->size + g.block - 1) / g.block;
        off_t want = (off_t)(nb ? nb : 1);
        if (map_path(path, sizeof path, f, "lmap")) return 1;
        int have_map = stat(path, &st) == 0;
        if (!have_data || !have_map || bad_data || st.st_size != want)
            return 1;
    }
    return 0;
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
    if (fd < 0) {
        /* Hairpin: a donor on THIS machine is advertised at a public IP we
         * cannot dial from inside the NAT. Its port answers on loopback —
         * knock there, bounded, before writing the peer off as relay-only.
         * (chat+donor on one box: the bytes it donated should arrive at
         * disk speed, not by a round trip through the tracker.) */
        const char *port_part = strrchr(p->addr, ':');
        if (port_part && strncmp(p->addr, "127.", 4) != 0) {
            char here[64];
            snprintf(here, sizeof here, "127.0.0.1%s", port_part);
            fd = lmb_connect_ms(here, 500);
            if (fd >= 0) snprintf(p->dial, sizeof p->dial, "%s", here);
        }
        if (fd < 0) return NULL;
    }
    if (lmb_auth(fd)) { lmb_close(fd); return NULL; }
    for (int i = 0; i < 2; i++) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        LmbMsg m = {0};
        if (lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) || lmb_recv(fd, &m)) {
            lmb_close(fd);
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
    if (fd >= 0) lmb_close(fd);
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
 * LUMABRI_PREFETCH sets the readahead depth in blocks (default 2, 0 = off).
 *
 * One exception the byte-mirror cannot see for itself: when the experts run
 * on peers, the chatter is meant to touch only the dense part, and a
 * readahead of 2 blocks past a dense read walks straight into the adjacent
 * expert region at 1 MiB granularity — pulling expert weights it will never
 * execute, a little more on every run. The engine's phase-2 client sets
 * LUMABRI_REMOTE_EXPERTS the moment delegation goes live (same process), and
 * from then on readahead is off, unless the operator asked for a specific
 * depth explicitly. A skip-range-aware prefetcher could restore the latency
 * hiding without entering declared expert ranges; this is the safe floor. */

#define PF_QLEN 64
static struct {
    RFile *f; uint32_t blk;
} pf_q[PF_QLEN];
static int pf_head, pf_tail, pf_depth;
static int pf_user_set;          /* LUMABRI_PREFETCH given explicitly: it wins */
static int pf_expert_off;        /* latches once remote experts are active */

static int prefetch_suppressed(void) {
    if (pf_user_set) return 0;
    if (pf_expert_off) return 1;
    const char *e = getenv("LUMABRI_REMOTE_EXPERTS");
    if (e && e[0] == '1') {
        pf_expert_off = 1;
        fprintf(stderr, "[lumabri] remote experts active: readahead off so the "
                        "chatter never pulls expert weights it will not run\n");
        return 1;
    }
    return 0;
}
static pthread_mutex_t pf_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pf_cv = PTHREAD_COND_INITIALIZER;

static int ensure_block(RFile *f, uint32_t blk);      /* fwd */
static int ensure_range(RFile *f, uint64_t off, uint64_t len);   /* fwd */

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
    if (!pf_depth || !f->nblocks || prefetch_suppressed()) return;
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

/* ---- the dense warm-up ----------------------------------------------------
 *
 * A MoE engine loads its dense weights layer by layer, on the first forward:
 * behind a cold mirror that made the FIRST REPLY a 7 GB download spread over
 * 43 layers, while "ready" had been printed half an hour earlier. The warm-up
 * reads every safetensors header, keeps the tensors that are not routed
 * experts (name without ".experts." — shared_experts stay, they run here),
 * and pulls their blocks through the ordinary verified path while the engine
 * boots. The engine's own reads of the same blocks dedup on the in-flight
 * map, so nothing is fetched twice. Progress is published as bytes so the
 * chatter can draw a bar with an ETA; "ready" then means ready. */
static int dense_is_expert(const char *name) {
    return strstr(name, ".experts.") != NULL;
}

/* one shard: header → tensor ranges → blocks to have; returns needed blocks */
static uint32_t dense_plan_file(RFile *f, uint8_t *need) {
    char cpath[LMB_CACHE_PATH_MAX];
    if (ensure_range(f, 0, 8) || data_path(cpath, sizeof cpath, f->rel)) return 0;
    int fd = real_open(cpath, O_RDONLY, 0);
    if (fd < 0) return 0;
    uint8_t hdr[8];
    uint64_t hlen = 0;
    if (real_pread(fd, hdr, 8, 0) != 8) { real_close(fd); return 0; }
    for (int i = 7; i >= 0; i--) hlen = (hlen << 8) | hdr[i];
    if (!hlen || hlen > (64u << 20) || 8 + hlen > f->size) { real_close(fd); return 0; }
    if (ensure_range(f, 8, hlen)) { real_close(fd); return 0; }
    char *json = (char *)malloc((size_t)hlen + 1);
    if (!json) { real_close(fd); return 0; }
    size_t got = 0;
    while (got < hlen) {
        ssize_t r = real_pread(fd, json + got, (size_t)hlen - got, (off_t)(8 + got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    real_close(fd);
    if (got != hlen) { free(json); return 0; }
    json[hlen] = 0;
    uint32_t needed = 0;
    for (char *p = strstr(json, "\"data_offsets\""); p; p = strstr(p + 1, "\"data_offsets\"")) {
        /* the tensor's object starts at the previous '{'; its name is the
         * quoted key right before it */
        char *ob = p;
        while (ob > json && *ob != '{') ob--;
        char *q2 = ob;
        while (q2 > json && *q2 != '"') q2--;           /* closing quote of the name */
        char *q1 = q2 > json ? q2 - 1 : json;
        while (q1 > json && *q1 != '"') q1--;
        if (q2 <= q1) continue;
        char name[256];
        size_t nl = (size_t)(q2 - q1 - 1);
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, q1 + 1, nl); name[nl] = 0;
        if (dense_is_expert(name)) continue;
        unsigned long long a = 0, b = 0;
        char *br = strchr(p, '[');
        if (!br || sscanf(br, "[%llu,%llu]", &a, &b) != 2 || b <= a) continue;
        uint64_t off = 8 + hlen + a, end = 8 + hlen + b;
        if (end > f->size) end = f->size;
        if (off >= end) continue;
        for (uint32_t blk = (uint32_t)(off / g.block);
             blk <= (uint32_t)((end - 1) / g.block) && blk < f->nblocks; blk++)
            if (!need[blk]) { need[blk] = 1; needed++; }
    }
    free(json);
    return needed;
}

static void *dense_prefetch_thread(void *arg) {
    (void)arg;
    uint8_t **plans = (uint8_t **)calloc((size_t)g.nfiles, sizeof *plans);
    if (!plans) { atomic_store(&g.dense_state, 3); return NULL; }
    uint64_t total = 0, done = 0;
    for (int i = 0; i < g.nfiles; i++) {
        RFile *f = &g.files[i];
        size_t n = strlen(f->rel);
        if (n < 12 || strcmp(f->rel + n - 12, ".safetensors")) continue;
        plans[i] = (uint8_t *)calloc(f->nblocks ? f->nblocks : 1, 1);
        if (!plans[i]) continue;
        uint32_t needed = dense_plan_file(f, plans[i]);
        if (!needed) { free(plans[i]); plans[i] = NULL; continue; }
        for (uint32_t b = 0; b < f->nblocks; b++)
            if (plans[i][b]) {
                uint64_t bytes = (uint64_t)b + 1 == f->nblocks
                    ? f->size - (uint64_t)b * g.block : g.block;
                total += bytes;
                if (f->map[b]) done += bytes;
            }
    }
    atomic_store(&g.dense_total, total);
    atomic_store(&g.dense_done, done);
    int failed = 0;
    for (int i = 0; i < g.nfiles; i++) {
        if (!plans[i]) continue;
        RFile *f = &g.files[i];
        for (uint32_t b = 0; b < f->nblocks; b++) {
            if (!plans[i][b] || f->map[b]) continue;
            uint64_t bytes = (uint64_t)b + 1 == f->nblocks
                ? f->size - (uint64_t)b * g.block : g.block;
            if (ensure_block(f, b)) { failed = 1; continue; }
            atomic_fetch_add(&g.dense_done, bytes);
        }
        free(plans[i]);
    }
    free(plans);
    atomic_store(&g.dense_state, failed ? 3 : 2);
    fprintf(stderr, "[lumabri] dense %s: %.0f MB of non-expert weights in the mirror%s\n",
            failed ? "warm-up incomplete" : "ready",
            (double)atomic_load(&g.dense_done) / 1e6,
            failed ? " (some blocks could not be fetched; the engine will retry them)" : "");
    return NULL;
}

static void *stats_thread(void *arg) {
    int period = (int)(intptr_t)arg;
    uint64_t last_bytes = 0, last_cas = 0, last_dense = 0;
    for (;;) {
        sleep((unsigned)period);
        if (atomic_load(&g.dense_state) == 1) {
            uint64_t dt = atomic_load(&g.dense_total), dd = atomic_load(&g.dense_done);
            fprintf(stderr, "[lumabri] dense %.1f/%.1f MB (%.1f MB/s)\n",
                    (double)dd / 1e6, (double)dt / 1e6,
                    (double)(dd > last_dense ? dd - last_dense : 0) / 1e6 / period);
            last_dense = dd;
        }
        uint64_t nb = atomic_load(&g.net_bytes);
        uint64_t blk = atomic_load(&g.net_blocks);
        uint64_t warm = atomic_load(&g.warm_reads);
        uint64_t cb = atomic_load(&g.cas_bytes), ch = atomic_load(&g.cas_hits);
        if (nb == last_bytes && cb == last_cas && !warm) continue;
        fprintf(stderr, "[lumabri] net %.1f MB in %llu blocks (%.1f MB/s) · "
                "CAS %.1f MB in %llu hits · warm preads %llu\n",
                (double)nb / 1e6, (unsigned long long)blk,
                (double)(nb - last_bytes) / 1e6 / period, (double)cb / 1e6,
                (unsigned long long)ch, (unsigned long long)warm);
        last_bytes = nb; last_cas = cb;
    }
    return NULL;
}

/* ---- init --------------------------------------------------------------- */

static void shim_init_impl(void) {
    shim_resolve();
    if (lmb_secure_init()) return; /* fail closed: hooks reject every network fd */
    const char *cache = getenv("LUMABRI_CACHE");
    if (!cache || !cache[0]) {
        fprintf(stderr, "[lumabri] LUMABRI_VROOT is set but LUMABRI_CACHE is not — disabled\n");
        return;
    }
    if (path_printf(g.data_dir, sizeof g.data_dir, "%s/data", cache) ||
        path_printf(g.maps_dir, sizeof g.maps_dir, "%s/maps", cache)) {
        fprintf(stderr, "[lumabri] LUMABRI_CACHE path is too long — disabled\n");
        return;
    }
    const char *cas = getenv("LUMABRI_CAS");
    if ((cas && cas[0] && path_printf(g.cas_dir, sizeof g.cas_dir, "%s", cas)) ||
        ((!cas || !cas[0]) && path_printf(g.cas_dir, sizeof g.cas_dir,
                                          "%s/cas", cache))) {
        fprintf(stderr, "[lumabri] content-store path is too long — disabled\n");
        return;
    }
    mkdir_p(g.data_dir);
    mkdir_p(g.maps_dir);
    mkdir_p(g.cas_dir);

    /* The operator trust set: one key, a comma list, or a keyring file.
     * This is the only thing a chatter must obtain out of band — with it,
     * nothing else in the swarm needs to be trusted. */
    const char *pkspec = getenv("LUMABRI_PUBKEY");
    if (pkspec) {
        FILE *pf = pkspec[0] ? real_fopen(pkspec, "r") : NULL;
        int bad = !pkspec[0];
        size_t trust_before = g.trust.n;
        if (pf) { bad = lmb_trust_load_stream(&g.trust, pf); fclose(pf); }
        else {
            char copy[2048];
            if (strlen(pkspec) >= sizeof copy) bad = -1;
            else {
                snprintf(copy, sizeof copy, "%s", pkspec);
                char *save = NULL;
                for (char *p = strtok_r(copy, ",", &save); p;
                     p = strtok_r(NULL, ",", &save))
                    if (lmb_trust_add_hex(&g.trust, p)) { bad = -1; break; }
            }
        }
        if (bad) g.trust.n = trust_before;
        if (bad || g.trust.n == trust_before) {
            fprintf(stderr, "[lumabri] LUMABRI_PUBKEY is not a valid key/keyring "
                            "— refusing the model\n");
            return;
        }
    }

    long mib = 8;
    const char *bs = getenv("LUMABRI_BLOCK_MIB");
    if (bs && atol(bs) >= 1 && atol(bs) <= 64) mib = atol(bs);
    g.block = (uint32_t)(mib << 20);

    /* Lock before reading either online placement or the offline manifest.
     * Otherwise an offline starter could observe an old inventory, wait for
     * an online reset, then pair that inventory with the newly saved root. */
    if (cache_lock_shared_open()) {
        fprintf(stderr, "[lumabri] cannot lock the shared mirror — disabled\n");
        cache_lock_release();
        return;
    }

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
    } else {
        if (manifest_load()) {
            fprintf(stderr, "[lumabri] no tracker, no peers, no saved manifest — disabled\n");
            cache_lock_release();
            return;
        }
        qsort(g.files, (size_t)g.nfiles, sizeof(RFile), rfile_cmp);
        fprintf(stderr, "[lumabri] offline: serving from the local mirror only "
                        "(a cold block will be EIO)\n");
    }

    /* Bind every persisted bitmap to the signed identity of the complete
     * model. File size alone cannot detect a checkpoint replaced in place;
     * the model root changes for any byte or inventory change. On the first
     * run after this format was introduced, old unbound maps are reset once.
     * Offline mode keeps using the last verified identity. */
    LmbModelIdentity current_id = {0}, saved_id = {0};
    CacheIdentity saved_rec = {0};
    const char *model = getenv("LUMABRI_MODEL");
    int online_placement = placed == 0;
    int have_current_id = online_placement && tracker && tracker[0] && model && model[0] &&
                          lmb_model_identity_get(tracker, model, &current_id) == 0;
    if (have_current_id && !model_identity_signature_ok(&current_id)) {
        fprintf(stderr, "[lumabri] model identity is not signed by the operator — disabled\n");
        cache_lock_release();
        return;
    }
    if (online_placement && g.trust.n && !have_current_id) {
        fprintf(stderr, "[lumabri] signed swarm has no complete model identity — disabled\n");
        cache_lock_release();
        return;
    }
    int have_saved_id = cache_identity_get(&saved_rec, &saved_id) == 0;
    if (!online_placement && g.trust.n && !have_saved_id) {
        fprintf(stderr, "[lumabri] offline mirror has no verified model identity — disabled\n");
        cache_lock_release();
        return;
    }
    int identity_reset = have_current_id &&
                         cache_identity_differs(&current_id, &saved_id, have_saved_id);
    int stale_layout = cache_has_stale_layout();

    /* Same identity: keep the shared lock for the whole process, allowing any
     * number of chatters.  reset.lock elects one upgrader. Contenders wait
     * there without holding cache.lock, then reacquire shared and recheck; if
     * the elected process already repaired the mirror they never queue behind
     * that process's full chat lifetime. */
    int exclusive = 0;
    if (identity_reset || stale_layout) {
        cache_flock(LOCK_UN);
        if (cache_reset_lock() || cache_flock(LOCK_SH)) {
            fprintf(stderr, "[lumabri] cannot coordinate mirror reset — disabled\n");
            cache_lock_release();
            return;
        }
        have_saved_id = cache_identity_get(&saved_rec, &saved_id) == 0;
        identity_reset = have_current_id &&
                         cache_identity_differs(&current_id, &saved_id, have_saved_id);
        stale_layout = cache_has_stale_layout();
        if (identity_reset || stale_layout) {
            cache_flock(LOCK_UN);
            if (cache_flock(LOCK_EX)) {
                fprintf(stderr, "[lumabri] cannot exclusively lock mirror reset — disabled\n");
                cache_lock_release();
                return;
            }
            /* The tracker may have restarted while an old-checkpoint chatter
             * kept us waiting. Never reset a placement using a stale root. */
            if (have_current_id) {
                LmbModelIdentity fresh = {0};
                if (lmb_model_identity_get(tracker, model, &fresh) ||
                    !model_identity_signature_ok(&fresh) ||
                    strcmp(fresh.model, current_id.model) ||
                    memcmp(fresh.root, current_id.root, sizeof fresh.root)) {
                    fprintf(stderr, "[lumabri] model identity changed during mirror init — disabled\n");
                    cache_lock_release();
                    return;
                }
            }
            have_saved_id = cache_identity_get(&saved_rec, &saved_id) == 0;
            identity_reset = have_current_id &&
                             cache_identity_differs(&current_id, &saved_id, have_saved_id);
            stale_layout = cache_has_stale_layout();
            exclusive = identity_reset || stale_layout;
            if (!exclusive && cache_flock(LOCK_SH)) {
                fprintf(stderr, "[lumabri] cannot retain the shared mirror lock — disabled\n");
                cache_lock_release();
                return;
            }
        }
        if (!exclusive) cache_reset_unlock();
    }
    if (identity_reset)
        fprintf(stderr, "[lumabri] model identity changed — cached block maps reset\n");
    if (placed == 0 && manifest_save()) {
        fprintf(stderr, "[lumabri] cannot persist the mirror manifest — disabled\n");
        cache_lock_release();
        return;
    }

    /* materialize the sparse mirror + load the block maps */
    uint64_t total = 0, have = 0;
    for (int i = 0; i < g.nfiles; i++) {
        RFile *f = &g.files[i];
        uint64_t nblocks = f->size / g.block + (f->size % g.block != 0);
        if (f->size > (uint64_t)INT64_MAX || nblocks > UINT32_MAX) {
            fprintf(stderr, "[lumabri] %s has unsupported mirror geometry — disabled\n",
                    f->rel);
            cache_lock_release(); return;
        }
        f->nblocks = (uint32_t)nblocks;
        pthread_mutex_init(&f->lk, NULL);
        pthread_mutex_init(&f->commit_lk, NULL);
        pthread_cond_init(&f->cv, NULL);
        f->wfd = -1;

        char dpath[LMB_CACHE_PATH_MAX], mpath[LMB_CACHE_PATH_MAX];
        struct stat before;
        if (data_path(dpath, sizeof dpath, f->rel) ||
            map_path(mpath, sizeof mpath, f, "lmap")) {
            fprintf(stderr, "[lumabri] mirror path too long for %s — disabled\n",
                    f->rel);
            cache_lock_release(); return;
        }
        mkdir_parent(dpath);
        int data_new = stat(dpath, &before) != 0;
        int fd = real_open(dpath, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "[lumabri] cannot create mirror %s\n", dpath);
            cache_lock_release(); return;
        }
        struct stat st;
        int stale = !data_new && fstat(fd, &st) == 0 &&
                    (uint64_t)st.st_size != f->size;

        mkdir_parent(mpath);
        int map_new = stat(mpath, &before) != 0;
        f->map = (uint8_t *)calloc(f->nblocks ? f->nblocks : 1, 1);
        f->verified = (uint8_t *)calloc(f->nblocks ? f->nblocks : 1, 1);
        f->inflight = (uint8_t *)calloc(f->nblocks ? f->nblocks : 1, 1);
        f->map_fd = real_open(mpath, O_RDWR | O_CREAT, 0644);
        off_t map_size = (off_t)(f->nblocks ? f->nblocks : 1);
        struct stat mst;
        int map_stale = !map_new && f->map_fd >= 0 &&
                        fstat(f->map_fd, &mst) == 0 && mst.st_size != map_size;
        int pair_stale = data_new != map_new;
        if (!f->map || !f->verified || !f->inflight || f->map_fd < 0) {
            fprintf(stderr, "[lumabri] map alloc failed for %s\n", f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        if ((stale || map_stale || pair_stale) && !exclusive) {
            fprintf(stderr, "[lumabri] mirror layout changed while shared — disabled\n");
            real_close(fd); cache_lock_release(); return;
        }
        if (stale || map_stale || pair_stale || identity_reset) {
            /* size or signed content identity changed: cached blocks suspect */
            if (stale)
                fprintf(stderr, "[lumabri] %s changed size upstream — mirror reset\n", f->rel);
            if (ftruncate(f->map_fd, 0)) {
                fprintf(stderr, "[lumabri] cannot clear map for %s — disabled\n", f->rel);
                real_close(fd); cache_lock_release(); return;
            }
        }
        if (identity_reset) {
            /* the checkpoint itself changed: a persisted truth for the old
             * bytes must not survive into the new identity */
            char tpath[LMB_CACHE_PATH_MAX];
            if (map_path(tpath, sizeof tpath, f, "truth")) {
                fprintf(stderr, "[lumabri] truth path too long for %s — disabled\n",
                        f->rel);
                real_close(fd); cache_lock_release(); return;
            }
            unlink(tpath);
        }
        if ((map_new || stale || map_stale || pair_stale || identity_reset) &&
            ftruncate(f->map_fd, map_size)) {
            fprintf(stderr, "[lumabri] cannot size map for %s — disabled\n", f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        /* Publish the cleared bitmap before resizing data.  If the machine
         * dies between these operations, no future process can mistake
         * sparse/truncated bytes for a durable warm block. */
        if ((map_new || stale || map_stale || pair_stale || identity_reset) &&
            fdatasync(f->map_fd)) {
            fprintf(stderr, "[lumabri] cannot durably reset map for %s — disabled\n",
                    f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        if (map_new && fsync_parent_dir(mpath)) {
            fprintf(stderr, "[lumabri] cannot persist map directory for %s — disabled\n",
                    f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        if ((data_new || stale) && ftruncate(fd, (off_t)f->size)) {
            fprintf(stderr, "[lumabri] ftruncate mirror data for %s failed\n", f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        if ((data_new || stale) && fdatasync(fd)) {
            fprintf(stderr, "[lumabri] cannot persist mirror data for %s — disabled\n",
                    f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        if (data_new && fsync_parent_dir(dpath)) {
            fprintf(stderr, "[lumabri] cannot persist mirror directory for %s — disabled\n",
                    f->rel);
            real_close(fd); cache_lock_release(); return;
        }
        real_close(fd);
        flock(f->map_fd, LOCK_SH);
        ssize_t r = real_pread ? real_pread(f->map_fd, f->map, f->nblocks, 0) : -1;
        flock(f->map_fd, LOCK_UN);
        if (r < 0) memset(f->map, 0, f->nblocks);
        total += f->size;
        for (uint32_t b = 0; b < f->nblocks; b++)
            if (f->map[b]) have += b + 1 == f->nblocks ? f->size - (uint64_t)b * g.block : g.block;
    }
    /* Written last: a crash halfway through clearing maps leaves the old
     * identity in place, so the next start repeats the reset instead of
     * blessing a half-reset cache. */
    if (have_current_id && identity_reset && cache_identity_save(&current_id)) {
        fprintf(stderr, "[lumabri] cannot persist model identity — disabled\n");
        cache_lock_release();
        return;
    }
    if (exclusive && cache_flock(LOCK_SH)) {
        fprintf(stderr, "[lumabri] cannot retain mirror lock after reset — disabled\n");
        cache_lock_release();
        return;
    }
    if (exclusive) cache_reset_unlock();

    /* measure the distance map, then start the readahead workers */
    probe_peers();
    for (int i = 0; i < g.npeers; i++) {
        if (g.peers[i].rtt_us == LONG_MAX)
            fprintf(stderr, "[lumabri] peer %s: unreachable directly (relay or failover)\n",
                    g.peers[i].addr);
        else if (g.peers[i].dial[0])
            fprintf(stderr, "[lumabri] peer %s: this machine — reading its "
                            "blocks on loopback (rtt %.2f ms)\n",
                    g.peers[i].addr, (double)g.peers[i].rtt_us / 1000.0);
        else
            fprintf(stderr, "[lumabri] peer %s: rtt %.2f ms\n",
                    g.peers[i].addr, (double)g.peers[i].rtt_us / 1000.0);
    }
    pf_depth = 2;
    const char *pf = getenv("LUMABRI_PREFETCH");
    if (pf) { pf_depth = atoi(pf); pf_user_set = 1; }
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

    {
        pthread_t t;
        if (pthread_create(&t, NULL, mirror_commit_thread, NULL) == 0)
            pthread_detach(t);
    }

    const char *stats = getenv("LUMABRI_STATS");
    if (stats && atoi(stats) > 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, stats_thread, (void *)(intptr_t)atoi(stats)) == 0)
            pthread_detach(t);
    }
    /* The chatter asks for the dense warm-up on a swarm-fed model: it needs
     * peers (or a CAS) to pull from, and it must not run for a local copy. */
    if (getenv("LUMABRI_PREFETCH_DENSE") && (g.npeers || g.cas_dir[0])) {
        pthread_t t;
        atomic_store(&g.dense_state, 1);
        if (pthread_create(&t, NULL, dense_prefetch_thread, NULL) == 0)
            pthread_detach(t);
        else atomic_store(&g.dense_state, 3);
    }
    fprintf(stderr, "[lumabri] %d files · %.1f GB · %.1f%% already local · "
                    "%d peer(s) · block %u MiB · prefetch %d%s\n",
            g.nfiles, (double)total / 1e9,
            total ? 100.0 * (double)have / (double)total : 0.0,
            g.npeers, g.block >> 20, pf_depth,
            g.trust.n ? " · verifying against the operator trust set" : "");
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
            fd = lmb_connect_ms(p->dial[0] ? p->dial : p->addr, 2500);
            if (fd < 0) return NULL;             /* unreachable: relay decides */
            /* A block is 8 MiB: a read that has not completed in a minute is
             * a wedged peer, not a slow one, and the next peer should get the
             * block instead of the chatter watching "0 MB/s" for the general
             * five-minute I/O timeout. */
            lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_READ_TIMEOUT_MS",
                                               60000, 1000, 3600000));
            if (lmb_auth(fd)) { lmb_close(fd); return NULL; }
        }

        LmbBuf b = {0};
        lmb_buf_str(&b, rel); lmb_buf_u64(&b, off); lmb_buf_u32(&b, len);
        int rc = lmb_send(fd, LMB_READ, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        LmbMsg m = {0};
        if (rc == 0) rc = lmb_recv(fd, &m);
        if (rc != 0) { lmb_close(fd); continue; }   /* dead socket: retry fresh */

        if (m.op == LMB_READ_R && m.pay_len == len) {
            uint8_t *data = lmb_msg_take_pay(&m);
            lmb_msg_free(&m);
            pthread_mutex_lock(&p->lk);
            if (p->nidle < POOL_SOCKS) p->idle[p->nidle++] = fd;
            else { pthread_mutex_unlock(&p->lk); lmb_close(fd); return data; }
            pthread_mutex_unlock(&p->lk);
            return data;
        }
        /* protocol-level refusal (ERR, short pay): the stream is consistent,
         * keep the socket, but this peer cannot serve the block */
        lmb_msg_free(&m);
        pthread_mutex_lock(&p->lk);
        if (p->nidle < POOL_SOCKS) p->idle[p->nidle++] = fd;
        else { pthread_mutex_unlock(&p->lk); lmb_close(fd); return NULL; }
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
    uint8_t *data = lmb_msg_take_pay(&m);
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
    int signed_ok = 0;
    int has_sig_ok = 0, sidecar_unusable = 0;
    uint8_t saved_sig[64];
    char saved_model[64] = "";
    const char *tracker = getenv("LUMABRI_TRACKER");
    if (tracker && tracker[0]) {
        const char *model = getenv("LUMABRI_MODEL");
        const char *requested_model = model ? model : "";
        LmbBuf b = {0};
        lmb_buf_str(&b, model ? model : "");
        lmb_buf_str(&b, f->rel);
        LmbMsg m = {0};
        int rc = lmb_request(tracker, LMB_HASHES, b.p, (uint32_t)b.len, &m);
        free(b.p);
        if (rc == 0 && m.op == LMB_HASHES_R) {
            LmbCur c = { m.body, m.body_len, 0 };
            char tmodel[64] = "";
            uint32_t chunk = 0, n = 0, has_sig = 0;
            uint64_t tsize = 0;
            uint8_t sig[64];
            if (!lmb_cur_str(&c, tmodel, sizeof tmodel) &&
                !lmb_cur_u32(&c, &chunk) && !lmb_cur_u32(&c, &n) &&
                !lmb_cur_u64(&c, &tsize) && !lmb_cur_u32(&c, &has_sig) &&
                (!has_sig || !lmb_cur_bytes(&c, sig, 64)) && c.off == c.len &&
                (!requested_model[0] || !strcmp(tmodel, requested_model)) &&
                chunk == LMB_HASH_CHUNK && n <= LMB_MAX_PAY / 32 &&
                m.pay_len == n * 32 && tsize == f->size &&
                n == (uint32_t)(f->size / LMB_HASH_CHUNK +
                                (f->size % LMB_HASH_CHUNK != 0))) {
                /* The signature is what makes the tracker a courier: we
                 * rebuild the signed message ourselves and check it against
                 * a key obtained out of band, so a compromised tracker can
                 * withhold the truth but never rewrite it. */
                if (g.trust.n) {
                    size_t mlen = 0;
                    uint8_t *sm = has_sig
                        ? lmb_truth_msg(tmodel, f->rel, chunk, tsize, m.pay, n, &mlen)
                        : NULL;
                    if (sm && lmb_trust_verify(&g.trust, sig, sm, mlen) == 0)
                        signed_ok = 1;
                    free(sm);
                    if (!signed_ok)
                        fprintf(stderr, "[lumabri] %s: integrity data %s — "
                                "refusing it\n", f->rel,
                                has_sig ? "carries a BAD SIGNATURE"
                                        : "is not signed by the operator key");
                }
                if (!g.trust.n || signed_ok) {
                    hash = lmb_msg_take_pay(&m); nh = n;
                    has_sig_ok = (int)has_sig;
                    if (has_sig) memcpy(saved_sig, sig, 64);
                    snprintf(saved_model, sizeof saved_model, "%s", tmodel);
                }
            }
        }
        lmb_msg_free(&m);
    }
    if (!hash && truth_load(f) == 0) {
        hash = f->hash; nh = f->nh; signed_ok = f->signed_;
        f->hash = NULL; f->nh = 0;
        fprintf(stderr, "[lumabri] %s: using persisted%s integrity truth\n",
                f->rel, signed_ok ? " signed" : "");
    }
    if (!hash && truth_sidecar_exists(f)) {
        /* A sidecar that exists but cannot be believed is not the same as no
         * sidecar at all: this cache once had a truth, so its warm blocks do
         * not get the legacy benefit of the doubt. */
        sidecar_unusable = 1;
        fprintf(stderr, "[lumabri] %s: persisted truth exists but is unusable — "
                        "warm blocks will not be trusted\n", f->rel);
    }
    pthread_mutex_lock(&f->lk);
    f->hash = hash; f->nh = nh; f->signed_ = signed_ok;
    f->truth_invalid = sidecar_unusable;
    if (saved_model[0]) {
        snprintf(f->truth_model, sizeof f->truth_model, "%s", saved_model);
        f->truth_has_sig = has_sig_ok;
        if (has_sig_ok) memcpy(f->truth_sig, saved_sig, 64);
    }
    f->hstate = hash ? 1 : 2;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->lk);
    if (hash && saved_model[0] && truth_save(f))
        fprintf(stderr, "[lumabri] %s: could not persist integrity truth\n", f->rel);
    if (!hash)
        fprintf(stderr, "[lumabri] no usable integrity data for %s — fetches are "
                        "UNVERIFIED%s\n", f->rel,
                getenv("LUMABRI_REQUIRE_HASH") ? " and LUMABRI_REQUIRE_HASH is set"
                                               : "");
    else if (signed_ok)
        fprintf(stderr, "[lumabri] %s: truth signed by the operator key ✓\n", f->rel);
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

/* 0 = the bytes of one mirror block, as they sit on disk, match the truth */
static int local_block_verify(RFile *f, uint32_t blk, int fd) {
    uint64_t off = (uint64_t)blk * g.block;
    uint32_t len = (uint32_t)(off + g.block <= f->size ? g.block : f->size - off);
    uint8_t *data = (uint8_t *)malloc(len ? len : 1);
    if (!data) return -1;
    uint32_t got = 0;
    while (got < len) {
        ssize_t n = real_pread(fd, data + got, len - got, (off_t)(off + got));
        if (n < 0) { if (errno == EINTR) continue; free(data); return -1; }
        if (n == 0) { free(data); return -1; }
        got += (uint32_t)n;
    }
    int rc = block_verify(f, off, data, len);
    free(data);
    return rc;
}

/* ---- local content-addressed store ------------------------------------
 *
 * Verified 1 MiB truth chunks are also published under sha256 names.  A
 * second checkpoint/mirror using the same bytes can assemble its sparse file
 * locally without downloading them again.  The hash is checked again on
 * every CAS read: a filename is a hint, never authority.  `lumabri chat`
 * points LUMABRI_CAS at ~/.lumabri/cas; direct shim users may choose another
 * local directory, including a fast shared volume.
 */
static int cas_path(char *dst, size_t cap, const uint8_t hash[32]) {
    char hex[65]; lmb_hex(hex, hash, 32);
    return path_printf(dst, cap, "%s/%.2s/%s", g.cas_dir, hex, hex);
}

static int cas_read_full(int fd, uint8_t *p, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        ssize_t r = real_pread(fd, p + got, n - got, (off_t)got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (!r) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

static uint8_t *cas_load(RFile *f, uint64_t off, uint32_t len) {
    if (!g.cas_dir[0] || f->hstate != 1 || off % LMB_HASH_CHUNK) return NULL;
    uint8_t *data = (uint8_t *)malloc(len ? len : 1);
    if (!data) return NULL;
    for (uint32_t o = 0; o < len; o += LMB_HASH_CHUNK) {
        uint32_t ci = (uint32_t)((off + o) / LMB_HASH_CHUNK);
        uint32_t n = len - o < LMB_HASH_CHUNK ? len - o : LMB_HASH_CHUNK;
        char path[LMB_CACHE_PATH_MAX];
        if (ci >= f->nh) { free(data); return NULL; }
        if (cas_path(path, sizeof path, f->hash + (size_t)ci * 32)) {
            free(data); return NULL;
        }
        int fd = real_open(path, O_RDONLY | O_NOFOLLOW);
        struct stat st;
        uint8_t got[32];
        int bad = fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
                  st.st_size != (off_t)n || cas_read_full(fd, data + o, n);
        if (fd >= 0) close(fd);
        if (!bad) { lmb_sha256(data + o, n, got); bad = memcmp(got, f->hash + (size_t)ci * 32, 32); }
        if (bad) { free(data); return NULL; }
    }
    atomic_fetch_add(&g.cas_hits, 1);
    atomic_fetch_add(&g.cas_bytes, len);
    return data;
}

static void cas_publish(RFile *f, uint64_t off, const uint8_t *data, uint32_t len) {
    if (!g.cas_dir[0] || f->hstate != 1 || off % LMB_HASH_CHUNK) return;
    for (uint32_t o = 0; o < len; o += LMB_HASH_CHUNK) {
        uint32_t ci = (uint32_t)((off + o) / LMB_HASH_CHUNK);
        uint32_t n = len - o < LMB_HASH_CHUNK ? len - o : LMB_HASH_CHUNK;
        if (ci >= f->nh) return;
        char path[LMB_CACHE_PATH_MAX], tmp[LMB_CACHE_PATH_MAX];
        if (cas_path(path, sizeof path, f->hash + (size_t)ci * 32)) return;
        if (!access(path, R_OK)) continue;
        mkdir_parent(path);
        if (path_printf(tmp, sizeof tmp, "%s.tmp.%ld.%lu", path, (long)getpid(),
                        (unsigned long)pthread_self())) return;
        int fd = real_open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) continue;
        uint32_t put = 0;
        while (put < n) {
            ssize_t w = write(fd, data + o + put, n - put);
            if (w < 0) { if (errno == EINTR) continue; break; }
            put += (uint32_t)w;
        }
        int ok = put == n && fsync(fd) == 0;
        if (ok) fchmod(fd, 0444);
        close(fd);
        if (ok && !rename(tmp, path)) fsync_parent_dir(path);
        else unlink(tmp);
    }
}

/* Make one block present in the mirror. 0 on success.
 *
 * A set presence bit — ours from the startup snapshot or another process's
 * published byte — is only a hint that bytes may be there.  The first touch
 * in each process hashes the local bytes against the current truth before
 * serving them; a mismatch clears the bit and falls through to an ordinary
 * fetch, which is what repair is.  None of this takes a cross-process lock:
 * a block can only pass verification if every chunk equals the signed truth,
 * so no interleaving of writers can make wrong bytes verifiable — at worst a
 * torn view fails the hash and is fetched again. */
static int ensure_block(RFile *f, uint32_t blk) {
    pthread_mutex_lock(&f->lk);
    for (;;) {
        if (f->map[blk] && f->verified[blk]) { pthread_mutex_unlock(&f->lk); return 0; }
        if (!f->inflight[blk]) break;
        pthread_cond_wait(&f->cv, &f->lk);
    }
    /* Another process sharing this checkpoint may have published the block
     * since our startup snapshot.  The single-byte read needs no flock: the
     * committer makes data durable before setting the byte, a one-byte write
     * cannot tear, and merges only ever add bits while we hold the shared
     * cache lock that excludes resets. */
    if (!f->map[blk] && f->map_fd >= 0) {
        uint8_t shared = 0;
        if (real_pread(f->map_fd, &shared, 1, (off_t)blk) == 1 && shared == 1)
            f->map[blk] = 1;
    }
    int present = f->map[blk];
    f->inflight[blk] = 1;
    if (f->wfd < 0) {
        char path[LMB_CACHE_PATH_MAX];
        if (!data_path(path, sizeof path, f->rel))
            f->wfd = real_open(path, O_RDWR);
    }
    int wfd = f->wfd;
    pthread_mutex_unlock(&f->lk);

    uint64_t off = (uint64_t)blk * g.block;
    uint32_t len = (uint32_t)(off + g.block <= f->size ? g.block : f->size - off);
    uint8_t *data = NULL;
    int from_cas = 0;
    hashes_ensure(f);
    /* a configured public key implies strict mode: the whole point of
     * carrying one is refusing to run on bytes nobody signed */
    int require = getenv("LUMABRI_REQUIRE_HASH") != NULL || g.trust.n;

    if (present && wfd >= 0) {
        int trust_it = 0;
        if (f->hstate == 1) {
            trust_it = local_block_verify(f, blk, wfd) == 0;
            if (!trust_it) {
                /* the bytes on disk are not the bytes the operator signed:
                 * withdraw the bit in memory first so our own committer stops
                 * republishing it, then withdraw the published byte, then
                 * repair like any cold block.  No fdatasync: if the zero is
                 * lost in a crash, the next process verifies and re-clears. */
                pthread_mutex_lock(&f->lk);
                f->map[blk] = 0;
                pthread_mutex_unlock(&f->lk);
                uint8_t zero = 0;
                if (f->map_fd >= 0) {
                    ssize_t wrote;
                    do wrote = pwrite(f->map_fd, &zero, 1, (off_t)blk);
                    while (wrote < 0 && errno == EINTR);
                    if (wrote != 1)
                        fprintf(stderr, "[lumabri] cannot persist invalidation "
                                        "for block %u of %s\n", blk, f->rel);
                }
                fprintf(stderr, "[lumabri] local integrity failure: block %u of %s "
                                "does not match authenticated truth — invalidated\n",
                        blk, f->rel);
            }
        } else if (!require && !f->truth_invalid) {
            static _Atomic int legacy_warned;
            if (!atomic_exchange(&legacy_warned, 1))
                fprintf(stderr, "[lumabri] WARNING: legacy warm cache has no "
                                "authenticated truth; trusting present blocks\n");
            trust_it = 1;
        }
        if (trust_it) {
            pthread_mutex_lock(&f->lk);
            f->verified[blk] = 1;
            f->inflight[blk] = 0;
            pthread_cond_broadcast(&f->cv);
            pthread_mutex_unlock(&f->lk);
            return 0;
        }
    }

    if (f->hstate == 1 && (data = cas_load(f, off, len)) != NULL) {
        from_cas = 1;
    } else if (require && f->hstate != 1) {
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
    if (data && !from_cas) cas_publish(f, off, data, len);
    int ok = 0, werr = 0;
    if (data && wfd >= 0) {
        uint32_t put = 0;
        while (put < len) {
            ssize_t w = pwrite(wfd, data + put, len - put, (off_t)(off + put));
            if (w < 0) { if (errno == EINTR) continue; werr = errno; break; }
            put += (uint32_t)w;
        }
        /* The background committer makes data durable before publishing this
         * bit on disk.  Until then the bit exists only in memory, so a crash
         * causes a safe refetch rather than exposing sparse zeros. */
        if (put == len && !werr) ok = 1;
        if (!from_cas) {
            atomic_fetch_add(&g.net_bytes, len);
            atomic_fetch_add(&g.net_blocks, 1);
        }
    }
    free(data);
    /* A peer that had the bytes and a disk that could not keep them are two
     * completely different problems, and reporting the second as the first
     * sends people to look at their network for hours. The bytes arrived; it
     * is the mirror that failed. */
    if (!ok && werr == ENOSPC)
        fprintf(stderr, "[lumabri] DISCO PIENO scrivendo il mirror in %s — "
                        "il peer aveva i byte, non c'e' spazio per tenerli "
                        "(blocco %u di %s)\n", g.data_dir, blk, f->rel);
    else if (!ok && werr)
        fprintf(stderr, "[lumabri] mirror write failed (%s): block %u of %s\n",
                strerror(werr), blk, f->rel);
    else if (!ok)
        fprintf(stderr, "[lumabri] block %u of %s: no peer could serve it\n", blk, f->rel);

    pthread_mutex_lock(&f->lk);
    f->inflight[blk] = 0;
    if (ok) { f->map[blk] = 1; f->verified[blk] = 1; f->dirty_gen++; }
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
    char cpath[LMB_CACHE_PATH_MAX];
    if (data_path(cpath, sizeof cpath, rel)) return -1;
    RFile *f = rfind(rel);
    if (f && (flags & (O_WRONLY | O_RDWR | O_TRUNC))) { errno = EROFS; return -1; }
    if (!f && (flags & O_CREAT)) mkdir_parent(cpath);
    /* O_DIRECT on a mirror file is not what the caller thinks it is. The
     * engine asks for it to skip the page cache on a whole model that it
     * reads once; here the file is filled by our own buffered writes as
     * blocks arrive, and a direct read may not see them — the kernel makes
     * no coherency promise when the two are mixed on one file. Dropping the
     * flag returns the same bytes through the cache, which is also where the
     * block we just fetched already is. Silent by design: it changes how the
     * bytes are read, never which. */
#ifdef O_DIRECT
    if (f && (flags & O_DIRECT)) {
        flags &= ~O_DIRECT;
        static _Atomic int said;
        if (!atomic_exchange(&said, 1))
            fprintf(stderr, "[lumabri] O_DIRECT dropped on mirrored files "
                            "(the mirror is filled through the page cache)\n");
    }
#endif
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
    if (path[0] == '/' && vrel(path)) return open_common(path, flags, mode);
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
    if (path[0] == '/' && vrel(path)) return open_common(path, flags | O_LARGEFILE, mode);
    return real_openat64 ? real_openat64(dirfd, path, flags, mode)
                         : real_openat(dirfd, path, flags, mode);
}

static FILE *fopen_common(const char *path, const char *mode, FILE *(*fn)(const char *, const char *)) {
    const char *rel = vrel(path);
    if (!rel) return fn(path, mode);
    shim_init();
    if (!g.ok) { errno = EIO; return NULL; }
    char cpath[LMB_CACHE_PATH_MAX];
    if (data_path(cpath, sizeof cpath, rel)) return NULL;
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
    char cpath[LMB_CACHE_PATH_MAX];
    if (data_path(cpath, sizeof cpath, rel)) return NULL;
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
    lmb_sec_forget_hook(fd);
    return real_close(fd);
}
