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
#include <stdatomic.h>
#include <sys/stat.h>

#include "lumabri_proto.h"
#include "lumabri_sha.h"
#include "lumabri_sign.h"

#define MAX_FILES     4096
#define MAX_INCLUDES  64
#define MAX_READ_LEN  LMB_MAX_PAY
#define HEARTBEAT_S   10

typedef struct {
    char rel[LMB_PATH_MAX]; uint64_t size; int fd;
    uint8_t *hash; uint32_t nh;   /* sha256 per LMB_HASH_CHUNK */
    uint8_t sig[64]; int signed_;  /* operator signature over the truth */
} MFile;

static struct {
    char root[LMB_PATH_MAX];
    char name[64], advertise[64], tracker[64], model[64], token[128];
    uint8_t sk[64]; int have_key;   /* --key: this peer is the origin */
    MFile files[MAX_FILES]; int nfiles;
    const char *includes[MAX_INCLUDES]; int nincl;
    pthread_mutex_t fd_lk;
    _Atomic uint64_t served_bytes, served_reads;
} g = { .fd_lk = PTHREAD_MUTEX_INITIALIZER };

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

static int hash_file(MFile *f) {
    f->nh = (uint32_t)((f->size + LMB_HASH_CHUNK - 1) / LMB_HASH_CHUNK);
    f->hash = malloc((size_t)f->nh * 32 + 1);
    if (!f->hash) return -1;
    char sc[LMB_PATH_MAX * 2];
    snprintf(sc, sizeof sc, "%s/.lumabri_hashes/%s.sha", g.root, f->rel);
    FILE *fp = fopen(sc, "rb");
    if (fp) {
        uint64_t sz = 0;
        if (fread(&sz, 8, 1, fp) == 1 && sz == f->size &&
            fread(f->hash, 32, f->nh, fp) == f->nh)
            { fclose(fp); g_hash_done += f->size; return 0; }   /* cached: free */
        fclose(fp);
    }
    char full[LMB_PATH_MAX * 2];
    snprintf(full, sizeof full, "%s/%s", g.root, f->rel);
    int fd = open(full, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t *buf = malloc(LMB_HASH_CHUNK);
    if (!buf) { close(fd); return -1; }
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
        if (got != len) { free(buf); close(fd); return -1; }
        lmb_sha256(buf, len, f->hash + (size_t)c * 32);
        hash_tick(len, f->rel);
    }
    free(buf);
    close(fd);
    char tmp[LMB_PATH_MAX * 2 + 4];
    snprintf(tmp, sizeof tmp, "%s.tmp", sc);
    char dir[LMB_PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s", sc);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0;
        for (char *p = strchr(dir + 1, '/'); p; p = strchr(p + 1, '/'))
            { *p = 0; mkdir(dir, 0755); *p = '/'; }
        mkdir(dir, 0755);
    }
    fp = fopen(tmp, "wb");
    if (fp) {
        int ok = fwrite(&f->size, 8, 1, fp) == 1 &&
                 fwrite(f->hash, 32, f->nh, fp) == f->nh;
        fclose(fp);
        if (ok) rename(tmp, sc);
    }
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
    return NULL;
}

/* ---- pull mode: "the server decides" -------------------------------------
 * With --donate G the maintainer asks the tracker which slice of the model
 * it should hold (ASSIGN, rarest-first) and pulls it from the swarm before
 * serving: direct from a holding peer when reachable, through the tracker's
 * relay when not. Every donated gigabyte lands where the swarm is thinnest. */

static void mkdir_parents(const char *path) {
    char tmp[LMB_PATH_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = strchr(tmp + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0; mkdir(tmp, 0755); *p = '/';
    }
}

#define PULL_CHUNK (8u << 20)

/* the swarm's ground truth for one file, from the tracker; NULL when the
 * tracker has none (a swarm without integrity — the pull proceeds, warned) */
static uint8_t *fetch_truth(const char *rel, uint32_t *nh_out) {
    LmbBuf b = {0};
    lmb_buf_str(&b, g.model);
    lmb_buf_str(&b, rel);
    LmbMsg m = {0};
    int rc = lmb_request(g.tracker, LMB_HASHES, b.p, (uint32_t)b.len, &m);
    free(b.p);
    uint8_t *hashes = NULL;
    if (rc == 0 && m.op == LMB_HASHES_R) {
        LmbCur c = { m.body, m.body_len, 0 };
        uint32_t chunk = 0, n = 0;
        if (!lmb_cur_u32(&c, &chunk) && !lmb_cur_u32(&c, &n) &&
            chunk == LMB_HASH_CHUNK && m.pay_len == n * 32) {
            hashes = m.pay; m.pay = NULL;
            *nh_out = n;
        }
    }
    lmb_msg_free(&m);
    return hashes;
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

static int pull_file(const char *rel, uint64_t size) {
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

    char final[LMB_PATH_MAX * 2], part[LMB_PATH_MAX * 2];
    snprintf(final, sizeof final, "%s/%s", g.root, rel);
    snprintf(part, sizeof part, "%s.part", final);
    mkdir_parents(final);
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return -1;

    uint32_t truth_n = 0;
    uint8_t *truth = fetch_truth(rel, &truth_n);
    if (!truth)
        fprintf(stderr, "[maintainer %s] no integrity data for %s — "
                        "pulling unverified\n", g.name, rel);

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
        if (!data && want) { free(truth); close(out); if (src >= 0) close(src); return -1; }
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
    free(truth);
    if (src >= 0) close(src);
    close(out);
    return rename(part, final);
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
        if (pull_file(rel, size) == 0 && g.nfiles < MAX_FILES) {
            MFile *f = &g.files[g.nfiles++];
            snprintf(f->rel, sizeof f->rel, "%s", rel);
            f->size = size; f->fd = -1;
            pulled += size; done++;
        } else
            fprintf(stderr, "[maintainer %s] pull failed for %s\n", g.name, rel);
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
static int register_body_send(int fd, uint64_t held) {
    LmbBuf b = {0};
    lmb_buf_str(&b, g.name);
    lmb_buf_str(&b, g.advertise);
    lmb_buf_str(&b, g.model);
    lmb_buf_u64(&b, held);
    lmb_buf_u64(&b, atomic_load(&g.served_bytes));
    lmb_buf_u64(&b, atomic_load(&g.served_reads));
    manifest_body(&b);
    hash_section(&b);
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
        struct timeval tv = { HEARTBEAT_S, 0 };   /* recv timeout = beat cadence */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (register_body_send(fd, held)) { close(fd); sleep(HEARTBEAT_S); continue; }
        for (;;) {
            LmbMsg m;
            if (lmb_recv(fd, &m) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {   /* quiet: beat */
                    if (register_body_send(fd, held)) break;
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
        else
            sign_truth(&g.files[i]);
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

    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("[maintainer] listen"); return 1; }
    pthread_t t;
    if (g.tracker[0]) { pthread_create(&t, NULL, control_thread, NULL); pthread_detach(t); }
    pthread_create(&t, NULL, stats_thread, NULL); pthread_detach(t);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("[maintainer] accept"); break; }
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(t);
        else
            close(fd);
    }
    return 0;
}
