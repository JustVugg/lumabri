/* maintainer.c — a lumabri peer that holds (part of) a model and serves
 * byte ranges of it.
 *
 * Scans --root once at startup, keeps the files matching --include (all of
 * them when no --include is given), registers with the tracker every 10 s as
 * a heartbeat, and answers MANIFEST / READ on persistent connections. Reads
 * are plain pread on per-file cached fds: the OS page cache is the only
 * caching layer a serving peer needs.
 *
 *   ./maintainer --root DIR --port 7301 [--tracker host:port]
 *                [--name peer-a] [--advertise host:port]
 *                [--include 'model-000*[02468]-of-*.safetensors'] ...
 *
 * --include patterns are fnmatch(3) without FNM_PATHNAME, so '*' crosses '/'
 * and one pattern can pick files in subdirectories too.
 */
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>

#include "lumabri_proto.h"
#include "lumabri_sha.h"
#include "lumabri_sign.h"

#define MAX_FILES          4096
#define MAX_INCLUDES       64
#define MAX_READ_LEN       LMB_MAX_PAY
#define HEARTBEAT_S        10
#define LMB_LOCAL_PATH_MAX (LMB_PATH_MAX * 2 + 64)

typedef struct {
    char rel[LMB_PATH_MAX]; uint64_t size; int fd;
    uint64_t mtime_ns, ctime_ns; /* invalidate same-size hash sidecars */
    uint8_t *hash; uint32_t nh;   /* sha256 per LMB_HASH_CHUNK */
    uint8_t sig[64]; int signed_;  /* operator signature over the truth */
} MFile;

static struct {
    char root[LMB_PATH_MAX];
    char name[64], advertise[64], tracker[64], model[64], token[128];
    uint8_t sk[64]; int have_key;   /* --key: this peer is the origin */
    uint8_t peer_sk[64], peer_pk[32];  /* this machine's identity to the tracker */
    LmbTrustKeys trust;             /* --pubkey: old+new during manual rotation */
    LmbModelIdentity identity; int have_identity;
    MFile files[MAX_FILES]; int nfiles;
    const char *includes[MAX_INCLUDES]; int nincl;
    pthread_mutex_t fd_lk;
    _Atomic uint64_t served_bytes, served_reads;
} g = { .fd_lk = PTHREAD_MUTEX_INITIALIZER };
static LmbConnGate g_conn_gate = LMB_CONN_GATE_INIT;

static int want(const char *rel) {
    if (!g.nincl) return 1;
    for (int i = 0; i < g.nincl; i++)
        if (fnmatch(g.includes[i], rel, 0) == 0) return 1;
    return 0;
}

static void scan_dir(const char *dir) {
    char full[LMB_PATH_MAX * 2];
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, ".lumabri_hashes")) continue;   /* our sidecars */
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) { scan_dir(full); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        const char *rel = full + strlen(g.root) + 1;
        if (strlen(rel) >= LMB_PATH_MAX || !want(rel)) continue;
        if (g.nfiles == MAX_FILES) { fprintf(stderr, "[maintainer] file table full\n"); return; }
        MFile *f = &g.files[g.nfiles++];
        snprintf(f->rel, sizeof f->rel, "%s", rel);
        f->size = (uint64_t)st.st_size;
        f->mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
                      (uint64_t)st.st_mtim.tv_nsec;
        f->ctime_ns = (uint64_t)st.st_ctim.tv_sec * 1000000000ull +
                      (uint64_t)st.st_ctim.tv_nsec;
        f->fd = -1;
    }
    closedir(d);
}

static MFile *find_file(const char *rel) {
    for (int i = 0; i < g.nfiles; i++)
        if (!strcmp(g.files[i].rel, rel)) return &g.files[i];
    return NULL;
}

static int file_fd(MFile *f) {
    pthread_mutex_lock(&g.fd_lk);
    if (f->fd < 0) {
        char full[LMB_PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", g.root, f->rel);
        f->fd = open(full, O_RDONLY);
    }
    int fd = f->fd;
    pthread_mutex_unlock(&g.fd_lk);
    return fd;
}

static void send_err(int fd, const char *msg) {
    LmbBuf b = {0};
    lmb_buf_str(&b, msg);
    lmb_send(fd, LMB_ERR, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
}

/* ---- integrity ----------------------------------------------------------
 * sha256 per LMB_HASH_CHUNK of every held file, computed once and cached in
 * <root>/.lumabri_hashes/<rel>.sha (invalidated by size change). These go
 * to the tracker inside REGISTER: the first announcement of a file becomes
 * the swarm's ground truth, later mismatching announcements are rejected. */

/* Hashing a big model is minutes of a completely silent process, and on a
 * first start it happens before anything is served — so from the outside it
 * looks like a hang. Report it: total, progress, rate, and what it is for. */
static uint64_t g_hash_total, g_hash_done;
static double g_hash_t0, g_hash_last;

static double hash_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void hash_tick(uint64_t bytes, const char *rel) {
    g_hash_done += bytes;
    double t = hash_now();
    if (t - g_hash_last < 2.0 || !g_hash_total) return;
    g_hash_last = t;
    double dt = t - g_hash_t0, rate = dt > 0 ? (double)g_hash_done / dt : 0;
    double left = rate > 0 ? (double)(g_hash_total - g_hash_done) / rate : 0;
    printf("[maintainer %s] hashing %.1f/%.1f GB (%.0f%%) · %.0f MB/s · "
           "~%.0f min left · %s\n",
           g.name, (double)g_hash_done / 1e9, (double)g_hash_total / 1e9,
           100.0 * (double)g_hash_done / (double)g_hash_total,
           rate / 1e6, left / 60.0, rel);
    fflush(stdout);
}

/* Where a file's truth lives on disk: the hash vector beside the bytes and,
 * for a donor — which did not compute that truth but was given it — the
 * origin's signature over it. A donor that forgot the signature on restart
 * would announce unsigned bytes, a --pubkey tracker would refuse them, and
 * the donation would quietly amount to nothing. */
static int path_printf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) out[0] = 0;
        return -1;
    }
    return 0;
}

static int sidecar_path(const MFile *f, const char *ext, char *out, size_t cap) {
    return path_printf(out, cap, "%s/.lumabri_hashes/%s.%s",
                       g.root, f->rel, ext);
}

/* Stage under a name unique to this process. Two maintainers may share one
 * --root (phase5_test does, and so does any machine donating from the tree it
 * already serves): with a single fixed .tmp they collide on the same staging
 * file, one truncating the other mid-write, and the loser's rename then fails
 * on a file that is no longer its own. The rename is what makes the sidecar
 * appear atomically, so per-process staging removes the race entirely rather
 * than tolerating it — every other staging site in the tree already
 * uniquifies by pid this way. */
static int sidecar_write(const char *path, const void *a, size_t na,
                         const void *b, size_t nb) {
    char tmp[LMB_LOCAL_PATH_MAX + 32], dir[LMB_LOCAL_PATH_MAX];
    if (path_printf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()) ||
        path_printf(dir, sizeof dir, "%s", path)) return -1;
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0;
        for (char *p = strchr(dir + 1, '/'); p; p = strchr(p + 1, '/'))
            { *p = 0; mkdir(dir, 0755); *p = '/'; }
        mkdir(dir, 0755);
    }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    int ok = (!na || fwrite(a, 1, na, fp) == na) &&
             (!nb || fwrite(b, 1, nb, fp) == nb) &&
             fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    if (fclose(fp)) ok = 0;
    if (!ok) { unlink(tmp); return -1; }
    return rename(tmp, path);
}

static int save_truth(const MFile *f) {
    struct {
        uint32_t magic, version;
        uint64_t size, mtime_ns, ctime_ns;
        uint32_t nh, reserved;
    } hdr = { 0x3148534Cu, 1, f->size, f->mtime_ns, f->ctime_ns,
              f->nh, 0 }; /* "LSH1" */
    char p[LMB_LOCAL_PATH_MAX];
    if (f->hash) {
        if (sidecar_path(f, "sha", p, sizeof p) ||
            sidecar_write(p, &hdr, sizeof hdr,
                          f->hash, (size_t)f->nh * 32)) return -1;
    }
    if (f->signed_) {
        if (sidecar_path(f, "sig", p, sizeof p) ||
            sidecar_write(p, f->sig, 64, NULL, 0)) return -1;
    }
    return 0;
}

static int load_sig(MFile *f) {
    char p[LMB_LOCAL_PATH_MAX];
    if (sidecar_path(f, "sig", p, sizeof p)) return -1;
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    if (fread(f->sig, 1, 64, fp) == 64) f->signed_ = 1;
    fclose(fp);
    return 0;
}

static int hash_file(MFile *f) {
    char full[LMB_LOCAL_PATH_MAX], sc[LMB_LOCAL_PATH_MAX];
    char sigp[LMB_LOCAL_PATH_MAX];
    if (path_printf(full, sizeof full, "%s/%s", g.root, f->rel) ||
        sidecar_path(f, "sha", sc, sizeof sc) ||
        sidecar_path(f, "sig", sigp, sizeof sigp)) return -1;
    int fd = open(full, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    f->size = (uint64_t)st.st_size;
    f->mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
                  (uint64_t)st.st_mtim.tv_nsec;
    f->ctime_ns = (uint64_t)st.st_ctim.tv_sec * 1000000000ull +
                  (uint64_t)st.st_ctim.tv_nsec;
    f->nh = (uint32_t)((f->size + LMB_HASH_CHUNK - 1) / LMB_HASH_CHUNK);
    free(f->hash);
    f->hash = malloc((size_t)f->nh * 32 + 1);
    if (!f->hash) { close(fd); return -1; }
    FILE *fp = fopen(sc, "rb");
    if (fp) {
        struct {
            uint32_t magic, version;
            uint64_t size, mtime_ns, ctime_ns;
            uint32_t nh, reserved;
        } hdr = {0};
        if (fread(&hdr, sizeof hdr, 1, fp) == 1 &&
            hdr.magic == 0x3148534Cu && hdr.version == 1 &&
            hdr.size == f->size && hdr.mtime_ns == f->mtime_ns &&
            hdr.ctime_ns == f->ctime_ns && hdr.nh == f->nh &&
            fread(f->hash, 32, f->nh, fp) == f->nh) {
            struct stat after;
            int stable = fstat(fd, &after) == 0 &&
                (uint64_t)after.st_size == f->size &&
                (uint64_t)after.st_mtim.tv_sec * 1000000000ull +
                    (uint64_t)after.st_mtim.tv_nsec == f->mtime_ns &&
                (uint64_t)after.st_ctim.tv_sec * 1000000000ull +
                    (uint64_t)after.st_ctim.tv_nsec == f->ctime_ns;
            fclose(fp); close(fd);
            if (stable) { g_hash_done += f->size; return 0; }
            free(f->hash); f->hash = NULL;
            return -1;
        }
        fclose(fp);
    }
    /* A rehash invalidates any signature stored beside the old vector.  An
     * origin with the secret key will create a new one after this returns; a
     * donor must obtain the new proof from the swarm, never reuse the old. */
    unlink(sigp);
    f->signed_ = 0;
    uint8_t *buf = malloc(LMB_HASH_CHUNK);
    if (!buf) { free(f->hash); f->hash = NULL; close(fd); return -1; }
    for (uint32_t c = 0; c < f->nh; c++) {
        uint64_t off = (uint64_t)c * LMB_HASH_CHUNK;
        uint32_t len = (uint32_t)(f->size - off < LMB_HASH_CHUNK ? f->size - off
                                                                 : LMB_HASH_CHUNK);
        uint32_t got = 0;
        while (got < len) {
            ssize_t r = pread(fd, buf + got, len - got, (off_t)(off + got));
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
            got += (uint32_t)r;
        }
        if (got != len) {
            free(buf); free(f->hash); f->hash = NULL; close(fd); return -1;
        }
        lmb_sha256(buf, len, f->hash + (size_t)c * 32);
        hash_tick(len, f->rel);
    }
    free(buf);
    struct stat after;
    uint64_t after_mtime = 0, after_ctime = 0;
    int after_ok = fstat(fd, &after) == 0;
    if (after_ok) {
        after_mtime = (uint64_t)after.st_mtim.tv_sec * 1000000000ull +
                      (uint64_t)after.st_mtim.tv_nsec;
        after_ctime = (uint64_t)after.st_ctim.tv_sec * 1000000000ull +
                      (uint64_t)after.st_ctim.tv_nsec;
    }
    if (!after_ok || after_mtime != f->mtime_ns || after_ctime != f->ctime_ns ||
        (uint64_t)after.st_size != f->size) {
        free(f->hash); f->hash = NULL; close(fd); return -1;
    }
    close(fd);
    /* The sidecar is only a cache that avoids re-hashing next start: the
     * hashes are already in memory, which is what serves and registers. A
     * failure to persist it (a too-long path, or two maintainers racing on
     * one root) must not drop the file or kill the peer — warn and go on. */
    if (save_truth(f))
        fprintf(stderr, "[maintainer %s] could not cache integrity data for %s "
                        "(will re-hash next start)\n", g.name, f->rel);
    return 0;
}

/* TEST ONLY — LUMABRI_CORRUPT_PPM makes this maintainer serve corrupted
 * bytes for that fraction of reads, so the integrity tests can prove that a
 * lying peer is caught. Announced loudly at startup; never set it outside a
 * test. The hashes are computed from the true file, so the lie is exactly
 * the one an adversarial peer would tell: correct manifest, wrong bytes. */
static long g_corrupt_ppm;
static __thread unsigned t_cseed;

static void maybe_corrupt(uint8_t *buf, uint32_t len) {
    if (!g_corrupt_ppm || !len) return;
    if (!t_cseed) t_cseed = (unsigned)(uintptr_t)&t_cseed | 1u;
    t_cseed = t_cseed * 1664525u + 1013904223u;
    if (t_cseed % 1000000u < (unsigned)g_corrupt_ppm) buf[0] ^= 0xFF;
}

/* One range read into a fresh buffer; NULL on any failure. Shared by the
 * direct path and the relayed path so both serve identical bytes. */
static uint8_t *read_range(const char *rel, uint64_t off, uint32_t *len_io) {
    MFile *f = find_file(rel);
    if (!f) return NULL;
    uint32_t len = *len_io;
    if (len > MAX_READ_LEN) return NULL;
    if (off >= f->size) { *len_io = 0; return (uint8_t *)malloc(1); }
    if (off + len > f->size) len = (uint32_t)(f->size - off);
    int ffd = file_fd(f);
    if (ffd < 0) return NULL;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) return NULL;
    uint32_t got = 0;
    while (got < len) {
        ssize_t r = pread(ffd, buf + got, len - got, (off_t)(off + got));
        if (r < 0) { if (errno == EINTR) continue; free(buf); return NULL; }
        if (r == 0) break;
        got += (uint32_t)r;
    }
    if (got != len) { free(buf); return NULL; }
    maybe_corrupt(buf, len);
    atomic_fetch_add(&g.served_bytes, len);
    atomic_fetch_add(&g.served_reads, 1);
    *len_io = len;
    return buf;
}

static void manifest_body(LmbBuf *b) {
    lmb_buf_u32(b, (uint32_t)g.nfiles);
    for (int i = 0; i < g.nfiles; i++) {
        lmb_buf_str(b, g.files[i].rel);
        lmb_buf_u64(b, g.files[i].size);
    }
}

/* the optional integrity section a REGISTER carries after the file list:
 * per file the hash vector and, when this peer holds the operator key, the
 * signature over it — the tracker forwards both and can forge neither */
static void hash_section(LmbBuf *b) {
    lmb_buf_u32(b, LMB_HASH_MAGIC);
    for (int i = 0; i < g.nfiles; i++) {
        lmb_buf_u32(b, g.files[i].hash ? g.files[i].nh : 0);
        if (g.files[i].hash)
            lmb_buf_bytes(b, g.files[i].hash, (size_t)g.files[i].nh * 32);
        lmb_buf_u32(b, g.files[i].signed_ ? 1u : 0u);
        if (g.files[i].signed_) lmb_buf_bytes(b, g.files[i].sig, 64);
    }
    if (g.have_identity) {
        lmb_buf_u32(b, LMB_MODEL_ID_MAGIC);
        lmb_buf_bytes(b, g.identity.root, sizeof g.identity.root);
        lmb_buf_u32(b, g.identity.has_sig ? 1u : 0u);
        if (g.identity.has_sig)
            lmb_buf_bytes(b, g.identity.sig, sizeof g.identity.sig);
    }
}

/* sign one file's truth with the operator key (origin peers only) */
static void sign_truth(MFile *f) {
    if (!g.have_key || !f->hash) return;
    size_t mlen = 0;
    uint8_t *msg = lmb_truth_msg(g.model, f->rel, LMB_HASH_CHUNK, f->size,
                                 f->hash, f->nh, &mlen);
    if (!msg) return;
    lmb_sign(f->sig, msg, mlen, g.sk);
    free(msg);
    f->signed_ = 1;
}

static int model_identity_valid(const LmbModelIdentity *id) {
    if (strcmp(id->model, g.model)) return 0;
    if (!g.trust.n) return 1;
    if (!id->has_sig) return 0;
    size_t n = 0;
    uint8_t *msg = lmb_model_id_msg(id->model, id->root, &n);
    int ok = msg && lmb_trust_verify(&g.trust, id->sig, msg, n) == 0;
    free(msg);
    return ok;
}

static int model_identity_local(LmbModelIdentity *id) {
    LmbModelItem *items = (LmbModelItem *)calloc(g.nfiles ? (size_t)g.nfiles : 1,
                                                 sizeof *items);
    if (!items) return -1;
    for (int i = 0; i < g.nfiles; i++) {
        if (!g.files[i].hash) { free(items); return -1; }
        items[i].path = g.files[i].rel;
        items[i].size = g.files[i].size;
        items[i].nh = g.files[i].nh;
        items[i].hashes = g.files[i].hash;
    }
    memset(id, 0, sizeof *id);
    snprintf(id->model, sizeof id->model, "%s", g.model);
    int rc = lmb_model_root(g.model, items, (size_t)g.nfiles, id->root);
    free(items);
    return rc;
}

static void model_identity_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/.lumabri_hashes/model.mid", g.root);
}

static int model_identity_load(LmbModelIdentity *id) {
    struct {
        uint32_t magic, version;
        char model[64];
        uint8_t root[32];
        uint32_t has_sig;
        uint8_t sig[64];
    } rec;
    char path[LMB_PATH_MAX * 2];
    model_identity_path(path, sizeof path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    int ok = fread(&rec, sizeof rec, 1, fp) == 1 &&
             rec.magic == LMB_MODEL_ID_MAGIC && rec.version == 1 &&
             rec.has_sig <= 1 && !strcmp(rec.model, g.model);
    fclose(fp);
    if (!ok) return -1;
    memset(id, 0, sizeof *id);
    snprintf(id->model, sizeof id->model, "%s", rec.model);
    memcpy(id->root, rec.root, 32);
    id->has_sig = rec.has_sig != 0;
    if (id->has_sig) memcpy(id->sig, rec.sig, 64);
    return model_identity_valid(id) ? 0 : -1;
}

static void model_identity_save(const LmbModelIdentity *id) {
    struct {
        uint32_t magic, version;
        char model[64];
        uint8_t root[32];
        uint32_t has_sig;
        uint8_t sig[64];
    } rec;
    memset(&rec, 0, sizeof rec);
    rec.magic = LMB_MODEL_ID_MAGIC; rec.version = 1;
    snprintf(rec.model, sizeof rec.model, "%s", id->model);
    memcpy(rec.root, id->root, 32);
    rec.has_sig = id->has_sig ? 1u : 0u;
    if (id->has_sig) memcpy(rec.sig, id->sig, 64);
    char path[LMB_PATH_MAX * 2];
    model_identity_path(path, sizeof path);
    sidecar_write(path, &rec, sizeof rec, NULL, 0);
}

static int model_identity_prepare(int disk_donor) {
    LmbModelIdentity local = {0}, saved = {0}, remote = {0};
    int complete_local = !disk_donor && g.nincl == 0;
    int have_local = complete_local && model_identity_local(&local) == 0;
    int have_saved = model_identity_load(&saved) == 0;
    int have_remote = g.tracker[0] &&
                      lmb_model_identity_get(g.tracker, g.model, &remote) == 0 &&
                      model_identity_valid(&remote);

    if (g.have_key && complete_local) {
        if (!have_local) return -1;
        size_t n = 0;
        uint8_t *msg = lmb_model_id_msg(g.model, local.root, &n);
        if (!msg) return -1;
        lmb_sign(local.sig, msg, n, g.sk);
        free(msg);
        local.has_sig = 1;
        g.identity = local; g.have_identity = 1;
    } else if (disk_donor || !complete_local) {
        if (have_remote) { g.identity = remote; g.have_identity = 1; }
        else if (have_saved) { g.identity = saved; g.have_identity = 1; }
    } else if (have_local) {
        if (have_remote && memcmp(local.root, remote.root, 32)) {
            fprintf(stderr, "[maintainer %s] local model root differs from the "
                            "swarm identity — refusing to advertise it\n", g.name);
            return -1;
        }
        if (have_remote) local = remote;
        else if (have_saved && !memcmp(local.root, saved.root, 32)) local = saved;
        g.identity = local; g.have_identity = 1;
    }
    if (g.trust.n && (!g.have_identity || !model_identity_valid(&g.identity))) {
        fprintf(stderr, "[maintainer %s] no operator-signed identity for model %s\n",
                g.name, g.model);
        return -1;
    }
    if (g.have_identity) model_identity_save(&g.identity);
    return 0; /* unsigned legacy/partial origins may have no complete root */
}

static int handle_read(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char rel[LMB_PATH_MAX];
    uint64_t off; uint32_t len;
    if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &off) ||
        lmb_cur_u32(&c, &len)) {
        send_err(fd, "bad read request"); return -1;
    }
    uint8_t *buf = read_range(rel, off, &len);
    if (!buf) { send_err(fd, "unreadable range"); return 0; }
    int rc = lmb_send(fd, LMB_READ_R, NULL, 0, buf, len);
    free(buf);
    return rc;
}

/* The same invite rule as the tracker: on a private swarm (LUMABRI_TOKEN
 * set) the first frame must be a matching AUTH — otherwise anyone who can
 * reach this port could pull the whole model, and the token would guard
 * only the index while the bytes stayed public. */
static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    int authed = g.token[0] ? 0 : 1;
    LmbMsg m;
    while (lmb_recv(fd, &m) == 0) {
        int rc;
        lmb_emu_delay();           /* emulated flight time, when configured */
        if (!authed && m.op != LMB_AUTH && m.op != LMB_PING) {
            send_err(fd, "this swarm needs an invite token");
            lmb_msg_free(&m);
            break;
        }
        switch (m.op) {
        case LMB_PING:     rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0); break;
        case LMB_AUTH: {
            char tok[128] = "";
            LmbCur c = { m.body, m.body_len, 0 };
            lmb_cur_str(&c, tok, sizeof tok);
            if (!g.token[0] || !strcmp(tok, g.token)) {
                authed = 1;
                rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            } else { send_err(fd, "bad token"); rc = -1; }
            break;
        }
        case LMB_MANIFEST: {
            LmbBuf b = {0};
            manifest_body(&b);
            rc = lmb_send(fd, LMB_MANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
            free(b.p);
            break;
        }
        case LMB_READ:     rc = handle_read(fd, &m); break;
        default:           send_err(fd, "unknown op"); rc = -1; break;
        }
        lmb_msg_free(&m);
        if (rc) break;
    }
    close(fd);
    lmb_conn_gate_leave(&g_conn_gate);
    return NULL;
}

/* ---- pull mode: "the server decides" -------------------------------------
 * With --donate G the maintainer asks the tracker which slice of the model
 * it should hold (ASSIGN, rarest-first) and pulls it from the swarm before
 * serving: direct from a holding peer when reachable, through the tracker's
 * relay when not. Every donated gigabyte lands where the swarm is thinnest. */

static int mkdir_parents(const char *path) {
    char tmp[LMB_LOCAL_PATH_MAX];
    if (path_printf(tmp, sizeof tmp, "%s", path)) return -1;
    for (char *p = strchr(tmp + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0; mkdir(tmp, 0755); *p = '/';
    }
    return 0;
}

#define PULL_CHUNK (8u << 20)

/* The swarm's ground truth for one file: which model it belongs to, the hash
 * vector, the size those hashes cover, and — on a signed swarm — the
 * operator's signature over all of it. */
typedef struct {
    uint8_t *hash; uint32_t nh;
    uint64_t size;
    uint8_t sig[64]; int has_sig;
    char model[64];
} Truth;

static void truth_free(Truth *t) { free(t->hash); memset(t, 0, sizeof *t); }

/* HASHES_R grew a model name and a signature when the tracker stopped being
 * an authority and became a courier. This parser did not grow with it: it
 * read the first bytes of the model string as a chunk size, concluded the
 * reply was malformed, and reported "no truth" — which chunks_ok() reads as
 * "nothing to check against, so everything is fine". A parse failure
 * degraded silently into an unverified pull, and the donor then announced
 * unsigned bytes that a --pubkey tracker refused, so it held nothing.
 *
 * Returns 0 with the truth filled, or -1 having said exactly what is wrong. */
static int fetch_truth(const char *rel, Truth *t) {
    memset(t, 0, sizeof *t);
    LmbBuf b = {0};
    lmb_buf_str(&b, g.model);
    lmb_buf_str(&b, rel);
    LmbMsg m = {0};
    int rc = lmb_request(g.tracker, LMB_HASHES, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_HASHES_R) { lmb_msg_free(&m); return -1; }

    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t chunk = 0, has_sig = 0;
    int bad = lmb_cur_str(&c, t->model, sizeof t->model) ||
              lmb_cur_u32(&c, &chunk) || lmb_cur_u32(&c, &t->nh) ||
              lmb_cur_u64(&c, &t->size) || lmb_cur_u32(&c, &has_sig) ||
              (has_sig && lmb_cur_bytes(&c, t->sig, 64));
    if (bad || chunk != LMB_HASH_CHUNK || m.pay_len != (uint64_t)t->nh * 32) {
        fprintf(stderr, "[maintainer %s] the tracker's integrity record for %s "
                        "is not one I can read — refusing to guess\n", g.name, rel);
        lmb_msg_free(&m);
        memset(t, 0, sizeof *t);
        return -1;
    }
    t->has_sig = has_sig != 0;
    t->hash = m.pay; m.pay = NULL;
    lmb_msg_free(&m);

    /* With the operator's public key we check the signature ourselves, so a
     * tracker that invents hashes to match corrupt bytes is caught here and
     * not three hops later. Without the key we can still carry a signature
     * we cannot read: the chatter checks it against its own copy. */
    if (g.trust.n) {
        if (!t->has_sig) {
            fprintf(stderr, "[maintainer %s] %s is not signed by the operator — "
                            "refusing to hold it\n", g.name, rel);
            truth_free(t);
            return -1;
        }
        size_t ml = 0;
        uint8_t *msg = lmb_truth_msg(t->model, rel, LMB_HASH_CHUNK, t->size,
                                     t->hash, t->nh, &ml);
        int ok = msg && lmb_trust_verify(&g.trust, t->sig, msg, ml) == 0;
        free(msg);
        if (!ok) {
            fprintf(stderr, "[maintainer %s] the signature on %s does not match "
                            "the operator key — refusing to hold it\n", g.name, rel);
            truth_free(t);
            return -1;
        }
    }
    return 0;
}

/* verify `len` bytes at `off` (chunk-aligned) against the truth */
static int chunks_ok(const uint8_t *truth, uint32_t nh,
                     const uint8_t *data, uint64_t off, uint32_t len) {
    if (!truth) return 1;
    for (uint32_t o = 0; o < len; o += LMB_HASH_CHUNK) {
        uint32_t ci = (uint32_t)((off + o) / LMB_HASH_CHUNK);
        uint32_t pl = len - o < LMB_HASH_CHUNK ? len - o : LMB_HASH_CHUNK;
        uint8_t h[32];
        if (ci >= nh) return 0;
        lmb_sha256(data + o, pl, h);
        if (memcmp(h, truth + (size_t)ci * 32, 32)) return 0;
    }
    return 1;
}

static int pull_file(const char *rel, uint64_t size, Truth *tr) {
    /* who has it, right now */
    LmbBuf b = {0};
    lmb_buf_str(&b, g.model);
    LmbMsg m = {0};
    char addrs[8][64];
    int naddr = 0;
    if (lmb_request(g.tracker, LMB_PLACEMENT, b.p, (uint32_t)b.len, &m) == 0 &&
        m.op == LMB_PLACEMENT_R) {
        LmbCur c = { m.body, m.body_len, 0 };
        uint32_t n = 0;
        if (!lmb_cur_u32(&c, &n))
            for (uint32_t i = 0; i < n; i++) {
                char path[LMB_PATH_MAX], a[64];
                uint64_t sz; uint16_t np;
                if (lmb_cur_str(&c, path, sizeof path) || lmb_cur_u64(&c, &sz) ||
                    lmb_cur_u16(&c, &np)) break;
                for (uint16_t p = 0; p < np; p++) {
                    if (lmb_cur_str(&c, a, sizeof a)) break;
                    if (!strcmp(path, rel) && naddr < 8)
                        snprintf(addrs[naddr++], 64, "%s", a);
                }
            }
    }
    free(b.p); lmb_msg_free(&m);

    /* the truth first: there is no point spending bandwidth on bytes we have
     * already decided we would not be allowed to serve */
    memset(tr, 0, sizeof *tr);
    int have_truth = fetch_truth(rel, tr) == 0;
    if (!have_truth && g.trust.n) return -1;       /* said why, in fetch_truth */
    if (!have_truth)
        fprintf(stderr, "[maintainer %s] no integrity data for %s — "
                        "pulling unverified\n", g.name, rel);
    if (have_truth && tr->size != size) {
        fprintf(stderr, "[maintainer %s] %s: the tracker offers %llu bytes but "
                        "the signed truth covers %llu — refusing\n", g.name, rel,
                (unsigned long long)size, (unsigned long long)tr->size);
        truth_free(tr);
        return -1;
    }
    const uint8_t *truth = tr->hash;
    uint32_t truth_n = tr->nh;

    char final[LMB_LOCAL_PATH_MAX], part[LMB_LOCAL_PATH_MAX];
    if (path_printf(final, sizeof final, "%s/%s", g.root, rel) ||
        path_printf(part, sizeof part, "%s.part", final)) {
        fprintf(stderr, "[maintainer %s] local path is too long for %s\n",
                g.name, rel);
        truth_free(tr);
        return -1;
    }
    if (mkdir_parents(final)) {
        fprintf(stderr, "[maintainer %s] local path is too long for %s\n",
                g.name, rel);
        truth_free(tr);
        return -1;
    }
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { truth_free(tr); return -1; }

    int src = -1, ai = 0;
    for (uint64_t off = 0; off < size || (size == 0 && off == 0); ) {
        uint32_t want = (uint32_t)(size - off < PULL_CHUNK ? size - off : PULL_CHUNK);
        uint8_t *data = NULL;
        uint32_t got = 0;
        /* direct, resuming across peers on failure */
        while (!data && ai < naddr) {
            if (src < 0) {
                src = lmb_connect_ms(addrs[ai], 2500);
                if (src >= 0 && lmb_auth(src)) { close(src); src = -1; }
            }
            if (src < 0) { ai++; continue; }
            LmbBuf rb = {0};
            lmb_buf_str(&rb, rel); lmb_buf_u64(&rb, off); lmb_buf_u32(&rb, want);
            LmbMsg rm = {0};
            int rc = lmb_send(src, LMB_READ, rb.p, (uint32_t)rb.len, NULL, 0);
            if (rc == 0) rc = lmb_recv(src, &rm);
            free(rb.p);
            if (rc == 0 && rm.op == LMB_READ_R && rm.pay_len == want) {
                if (chunks_ok(truth, truth_n, rm.pay, off, want)) {
                    data = rm.pay; got = rm.pay_len; rm.pay = NULL;
                } else {
                    fprintf(stderr, "[maintainer %s] %s served corrupt bytes of "
                            "%s — rejected\n", g.name, addrs[ai], rel);
                    close(src); src = -1; ai++;
                }
            } else { close(src); src = -1; ai++; }
            lmb_msg_free(&rm);
        }
        if (!data) {                                   /* relay floor */
            LmbBuf rb = {0};
            lmb_buf_str(&rb, g.model); lmb_buf_str(&rb, rel);
            lmb_buf_u64(&rb, off); lmb_buf_u32(&rb, want);
            LmbMsg rm = {0};
            if (lmb_request(g.tracker, LMB_RREAD, rb.p, (uint32_t)rb.len, &rm) == 0 &&
                rm.op == LMB_RREAD_R && rm.pay_len == want &&
                chunks_ok(truth, truth_n, rm.pay, off, want)) {
                data = rm.pay; got = rm.pay_len; rm.pay = NULL;
            }
            free(rb.p); lmb_msg_free(&rm);
        }
        if (!data && want) { truth_free(tr); close(out); if (src >= 0) close(src); return -1; }
        uint32_t put = 0;
        while (put < got) {
            ssize_t w = pwrite(out, data + put, got - put, (off_t)(off + put));
            if (w < 0) { if (errno == EINTR) continue; free(data); close(out); return -1; }
            put += (uint32_t)w;
        }
        free(data);
        off += want;
        if (size == 0) break;
    }
    if (src >= 0) close(src);
    close(out);
    if (rename(part, final)) { truth_free(tr); return -1; }
    return 0;                        /* the caller now owns tr */
}

static void pull_slice(uint64_t budget) {
    LmbBuf b = {0};
    lmb_buf_str(&b, g.model);
    lmb_buf_u64(&b, budget);
    lmb_buf_u32(&b, (uint32_t)g.nfiles);
    for (int i = 0; i < g.nfiles; i++) lmb_buf_str(&b, g.files[i].rel);
    LmbMsg m = {0};
    int rc = lmb_request(g.tracker, LMB_ASSIGN, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_ASSIGN_R) {
        fprintf(stderr, "[maintainer %s] tracker did not assign (is it running?)\n", g.name);
        lmb_msg_free(&m);
        return;
    }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) { lmb_msg_free(&m); return; }
    uint64_t pulled = 0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++) {
        char rel[LMB_PATH_MAX];
        uint64_t size;
        if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &size)) break;
        if (!lmb_rel_ok(rel)) {      /* the tracker does not get to name our disk */
            fprintf(stderr, "[maintainer %s] refusing unsafe file name from the "
                            "tracker\n", g.name);
            break;
        }
        printf("[maintainer %s] pull %u/%u: %s (%.1f MB)\n",
               g.name, i + 1, n, rel, (double)size / 1e6);
        fflush(stdout);
        Truth t = {0};
        if (pull_file(rel, size, &t) == 0 && g.nfiles < MAX_FILES) {
            char full[LMB_LOCAL_PATH_MAX];
            if (path_printf(full, sizeof full, "%s/%s", g.root, rel)) {
                fprintf(stderr, "[maintainer %s] local path is too long for %s\n",
                        g.name, rel);
                truth_free(&t);
                continue;
            }
            MFile *f = &g.files[g.nfiles];
            memset(f, 0, sizeof *f);
            snprintf(f->rel, sizeof f->rel, "%s", rel);
            f->size = size; f->fd = -1;
            /* keep the truth we were given, signature and all: a donor that
             * re-hashed the bytes itself could prove they are consistent but
             * not that they are the operator's, and an unsigned announcement
             * is refused by a signed swarm */
            f->hash = t.hash; f->nh = t.nh;
            f->signed_ = t.has_sig;
            if (t.has_sig) memcpy(f->sig, t.sig, 64);
            struct stat st;
            if (!stat(full, &st)) {
                f->mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
                              (uint64_t)st.st_mtim.tv_nsec;
                f->ctime_ns = (uint64_t)st.st_ctim.tv_sec * 1000000000ull +
                              (uint64_t)st.st_ctim.tv_nsec;
            }
            if (save_truth(f))
                fprintf(stderr, "[maintainer %s] could not cache integrity data "
                                "for %s (will re-hash next start)\n", g.name, rel);
            g.nfiles++;
            pulled += size; done++;
        } else {
            truth_free(&t);
            fprintf(stderr, "[maintainer %s] pull failed for %s\n", g.name, rel);
        }
    }
    lmb_msg_free(&m);
    printf("[maintainer %s] assigned slice: %u/%u files, %.2f GB pulled\n",
           g.name, done, n, (double)pulled / 1e9);
    fflush(stdout);
}

/* The control channel: ONE persistent outbound connection to the tracker.
 * It heartbeats REGISTER every 10 s, and — because it is outbound — it works
 * from behind any NAT with zero router configuration. The tracker uses the
 * same connection to push RREAD_FWD when a chatter could not reach us
 * directly: we answer with the bytes and the tracker relays them back. */
static int register_body_send(int fd, uint64_t held, const uint8_t nonce[32]) {
    LmbBuf b = {0};
    lmb_buf_str(&b, g.name);
    lmb_buf_str(&b, g.advertise);
    lmb_buf_str(&b, g.model);
    lmb_buf_u64(&b, held);
    lmb_buf_u64(&b, atomic_load(&g.served_bytes));
    lmb_buf_u64(&b, atomic_load(&g.served_reads));
    manifest_body(&b);
    hash_section(&b);
    /* prove the name is ours: sign the tracker's nonce (bound to this
     * connection) together with the identity this REGISTER claims */
    uint8_t msg[512], sig[64];
    size_t ml = lmb_peer_auth_msg(nonce, g.name, g.model, g.advertise, msg, sizeof msg);
    lmb_sign(sig, msg, ml, g.peer_sk);
    lmb_buf_peer_auth(&b, g.peer_pk, sig);
    int rc = lmb_send(fd, LMB_REGISTER, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

static void *control_thread(void *arg) {
    (void)arg;
    uint64_t held = 0;
    for (int i = 0; i < g.nfiles; i++) held += g.files[i].size;
    int warned = 0;
    for (;;) {
        int fd = lmb_connect(g.tracker);
        if (fd >= 0 && lmb_auth(fd)) { close(fd); fd = -1; }
        if (fd < 0) {
            if (!warned)
                fprintf(stderr, "[maintainer %s] tracker %s unreachable (will retry)\n",
                        g.name, g.tracker);
            warned = 1;
            sleep(HEARTBEAT_S);
            continue;
        }
        warned = 0;
        /* one identity nonce for this connection, reused by every heartbeat */
        uint8_t nonce[32];
        if (lmb_request_challenge(fd, nonce)) { close(fd); sleep(HEARTBEAT_S); continue; }
        struct timeval tv = { HEARTBEAT_S, 0 };   /* recv timeout = beat cadence */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (register_body_send(fd, held, nonce)) { close(fd); sleep(HEARTBEAT_S); continue; }
        for (;;) {
            LmbMsg m;
            if (lmb_recv(fd, &m) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {   /* quiet: beat */
                    if (register_body_send(fd, held, nonce)) break;
                    continue;
                }
                break;                                            /* dead socket */
            }
            int rc = 0;
            if (m.op == LMB_RREAD_FWD) {
                LmbCur c = { m.body, m.body_len, 0 };
                char rel[LMB_PATH_MAX];
                uint32_t id = 0, len = 0;
                uint64_t off = 0;
                if (lmb_cur_u32(&c, &id) || lmb_cur_str(&c, rel, sizeof rel) ||
                    lmb_cur_u64(&c, &off) || lmb_cur_u32(&c, &len)) rc = -1;
                else {
                    uint8_t *buf = read_range(rel, off, &len);
                    lmb_emu_delay();       /* the relayed path pays flight time too */
                    LmbBuf rb = {0};
                    lmb_buf_u32(&rb, id);
                    lmb_buf_u32(&rb, buf ? 1u : 0u);
                    rc = lmb_send(fd, LMB_RREAD_R, rb.p, (uint32_t)rb.len,
                                  buf, buf ? len : 0);
                    free(rb.p); free(buf);
                }
            }
            /* LMB_OK = heartbeat ack; anything else is ignored, same
             * forward-compat stance as the engine's serve protocol */
            lmb_msg_free(&m);
            if (rc) break;
        }
        close(fd);
        sleep(1);
    }
    return NULL;
}

static void *stats_thread(void *arg) {
    (void)arg;
    uint64_t last = 0;
    for (;;) {
        sleep(5);
        uint64_t b = atomic_load(&g.served_bytes), r = atomic_load(&g.served_reads);
        if (b != last) {
            printf("[maintainer %s] served %.1f MB in %llu reads\n",
                   g.name, (double)b / 1e6, (unsigned long long)r);
            fflush(stdout);
            last = b;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    int port = 7301;
    double donate_gb = 0;
    int model_explicit = 0;
    g.root[0] = g.name[0] = g.advertise[0] = g.tracker[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            snprintf(g.root, sizeof g.root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tracker") && i + 1 < argc)
            snprintf(g.tracker, sizeof g.tracker, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(g.name, sizeof g.name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--model-name") && i + 1 < argc)
            { snprintf(g.model, sizeof g.model, "%s", argv[++i]); model_explicit = 1; }
        else if (!strcmp(argv[i], "--advertise") && i + 1 < argc)
            snprintf(g.advertise, sizeof g.advertise, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--include") && i + 1 < argc && g.nincl < MAX_INCLUDES)
            g.includes[g.nincl++] = argv[++i];
        else if (!strcmp(argv[i], "--donate") && i + 1 < argc)
            donate_gb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            char hex[200] = "";
            FILE *kf = fopen(argv[++i], "r");
            if (!kf || fscanf(kf, "%198s", hex) != 1 || strlen(hex) != 128 ||
                lmb_unhex(g.sk, hex, 64)) {
                fprintf(stderr, "[maintainer] cannot read a 64-byte secret key "
                                "from %s (make one with: lumabri key)\n", argv[i]);
                if (kf) fclose(kf);
                return 2;
            }
            fclose(kf);
            g.have_key = 1;
        }
        /* The public half may be one key or an old+new keyring during a
         * manual rotation. Repeating --pubkey appends to the trust set. */
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc) {
            const char *spec = argv[++i];
            if (lmb_trust_load_spec(&g.trust, spec)) {
                fprintf(stderr, "[maintainer] cannot read public key/keyring "
                                "from %s\n", spec);
                return 2;
            }
        }
        else {
            fprintf(stderr, "usage: %s --root DIR [--port N] [--tracker H:P]"
                            " [--name S] [--advertise H:P] [--include PAT]..."
                            " [--key FILE] [--donate GB]\n", argv[0]);
            return 2;
        }
    }
    if (!g.root[0]) { fprintf(stderr, "[maintainer] --root is required\n"); return 2; }
    const char *tok = getenv("LUMABRI_TOKEN");
    if (tok) snprintf(g.token, sizeof g.token, "%s", tok);
    signal(SIGPIPE, SIG_IGN);   /* a vanished chatter must not kill the peer */
    lmb_conn_gate_init(&g_conn_gate);
    size_t rl = strlen(g.root);
    while (rl > 1 && g.root[rl - 1] == '/') g.root[--rl] = 0;
    if (!g.name[0]) snprintf(g.name, sizeof g.name, "peer-%d", port);
    if (!g.model[0]) {   /* default model name: the directory's basename */
        const char *base = strrchr(g.root, '/');
        snprintf(g.model, sizeof g.model, "%s", base ? base + 1 : g.root);
    }
    if (!g.advertise[0]) snprintf(g.advertise, sizeof g.advertise, "127.0.0.1:%d", port);

    scan_dir(g.root);
    if (donate_gb > 0) {
        if (!g.tracker[0]) { fprintf(stderr, "[maintainer] --donate needs --tracker\n"); return 2; }
        if (!model_explicit) {
            fprintf(stderr, "[maintainer] --donate needs --model-name "
                            "(which model to help hold)\n");
            return 2;
        }
        pull_slice((uint64_t)(donate_gb * 1e9));
    }
    if (!g.nfiles) { fprintf(stderr, "[maintainer %s] nothing to serve under %s\n", g.name, g.root); return 1; }
    { const char *cp = getenv("LUMABRI_CORRUPT_PPM");
      if (cp && atol(cp) > 0) {
          g_corrupt_ppm = atol(cp);
          printf("[maintainer %s] *** TEST MODE: serving corrupt bytes at "
                 "%ld ppm ***\n", g.name, g_corrupt_ppm);
      } }
    uint64_t total = 0;
    for (int i = 0; i < g.nfiles; i++) total += g.files[i].size;
    g_hash_total = total;
    g_hash_done = 0;
    g_hash_t0 = g_hash_last = hash_now();
    double h0 = g_hash_t0;
    if (total > 4e9)
        printf("[maintainer %s] integrity: hashing %d files, %.1f GB. "
               "Only the first start pays this — the result is cached in "
               "%s/.lumabri_hashes.\n",
               g.name, g.nfiles, (double)total / 1e9, g.root);
    fflush(stdout);
    for (int i = 0; i < g.nfiles; i++) {
        if (hash_file(&g.files[i]))
            fprintf(stderr, "[maintainer %s] cannot hash %s — served unverified\n",
                    g.name, g.files[i].rel);
        else {
            if (load_sig(&g.files[i])) {
                fprintf(stderr, "[maintainer %s] sidecar path is too long for %s\n",
                        g.name, g.files[i].rel);
                return 1;
            }
            sign_truth(&g.files[i]);
            if (save_truth(&g.files[i]))
                fprintf(stderr, "[maintainer %s] could not cache integrity data "
                                "for %s (will re-hash next start)\n",
                        g.name, g.files[i].rel);
        }
    }
    if (model_identity_prepare(donate_gb > 0)) {
        fprintf(stderr, "[maintainer %s] cannot establish the complete model "
                        "identity — refusing to serve an ambiguous checkpoint\n",
                g.name);
        return 1;
    }
    if (g.have_key) {
        char pub[70];
        lmb_hex(pub, g.sk + 32, 32);
        printf("[maintainer %s] ORIGIN: signed the truth of %d files with %s\n",
               g.name, g.nfiles, pub);
    }
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      double dh = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 - h0;
      if (dh > 1.0)
          printf("[maintainer %s] integrity: hashed %.1f GB in %.1fs "
                 "(cached for next start)\n", g.name, (double)total / 1e9, dh); }
    printf("[maintainer %s] holding %d files, %.1f GB, from %s\n",
           g.name, g.nfiles, (double)total / 1e9, g.root);
    if (lmb_emu_active())
        printf("[maintainer %s] emulated network: rtt %ld us ± %ld, loss %ld ppm\n",
               g.name, lmb_emu_rtt_us, lmb_emu_jitter_us, lmb_emu_loss_ppm);
    fflush(stdout);

    if (g.tracker[0]) {
        char kp[512];
        if (lmb_peer_identity(lmb_peer_key_path(kp, sizeof kp), g.peer_sk, g.peer_pk)) {
            fprintf(stderr, "[maintainer %s] cannot load or create a peer key at %s\n",
                    g.name, kp);
            return 1;
        }
    }
    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("[maintainer] listen"); return 1; }
    pthread_t t;
    if (g.tracker[0]) { pthread_create(&t, NULL, control_thread, NULL); pthread_detach(t); }
    pthread_create(&t, NULL, stats_thread, NULL); pthread_detach(t);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("[maintainer] accept"); break; }
        lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                                           LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000));
        if (!lmb_conn_gate_enter(&g_conn_gate)) { close(fd); continue; }
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(t);
        else { lmb_conn_gate_leave(&g_conn_gate); close(fd); }
    }
    return 0;
}
