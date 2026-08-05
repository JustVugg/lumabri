/* maintainer.c — a lumibri peer that holds (part of) a model and serves
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

#include "lumibri_proto.h"

#define MAX_FILES     4096
#define MAX_INCLUDES  64
#define MAX_READ_LEN  LMB_MAX_PAY
#define HEARTBEAT_S   10

typedef struct { char rel[LMB_PATH_MAX]; uint64_t size; int fd; } MFile;

static struct {
    char root[LMB_PATH_MAX];
    char name[64], advertise[64], tracker[64], model[64];
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

static void manifest_body(LmbBuf *b) {
    lmb_buf_u32(b, (uint32_t)g.nfiles);
    for (int i = 0; i < g.nfiles; i++) {
        lmb_buf_str(b, g.files[i].rel);
        lmb_buf_u64(b, g.files[i].size);
    }
}

static int handle_read(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char rel[LMB_PATH_MAX];
    uint64_t off; uint32_t len;
    if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &off) ||
        lmb_cur_u32(&c, &len) || len > MAX_READ_LEN) {
        send_err(fd, "bad read request"); return -1;
    }
    MFile *f = find_file(rel);
    if (!f) { send_err(fd, "unknown file"); return 0; }
    if (off >= f->size) return lmb_send(fd, LMB_READ_R, NULL, 0, NULL, 0);
    if (off + len > f->size) len = (uint32_t)(f->size - off);
    int ffd = file_fd(f);
    if (ffd < 0) { send_err(fd, "cannot open file"); return 0; }
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { send_err(fd, "oom"); return 0; }
    uint32_t got = 0;
    while (got < len) {
        ssize_t r = pread(ffd, buf + got, len - got, (off_t)(off + got));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        got += (uint32_t)r;
    }
    int rc;
    if (got != len) {
        send_err(fd, "short read"); rc = 0;
    } else {
        rc = lmb_send(fd, LMB_READ_R, NULL, 0, buf, len);
        atomic_fetch_add(&g.served_bytes, len);
        atomic_fetch_add(&g.served_reads, 1);
    }
    free(buf);
    return rc;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    LmbMsg m;
    while (lmb_recv(fd, &m) == 0) {
        int rc;
        switch (m.op) {
        case LMB_PING:     rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0); break;
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

static void *heartbeat_thread(void *arg) {
    (void)arg;
    uint64_t held = 0;
    for (int i = 0; i < g.nfiles; i++) held += g.files[i].size;
    int warned = 0;
    for (;;) {
        LmbBuf b = {0};   /* rebuilt each beat: the served counters move */
        lmb_buf_str(&b, g.name);
        lmb_buf_str(&b, g.advertise);
        lmb_buf_str(&b, g.model);
        lmb_buf_u64(&b, held);
        lmb_buf_u64(&b, atomic_load(&g.served_bytes));
        lmb_buf_u64(&b, atomic_load(&g.served_reads));
        manifest_body(&b);
        LmbMsg resp = {0};
        int ok = lmb_request(g.tracker, LMB_REGISTER, b.p, (uint32_t)b.len, &resp) == 0;
        free(b.p);
        if (ok) {
            lmb_msg_free(&resp);
            warned = 0;
        } else if (!warned) {
            fprintf(stderr, "[maintainer %s] tracker %s unreachable (will retry)\n",
                    g.name, g.tracker);
            warned = 1;
        }
        sleep(HEARTBEAT_S);
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
            snprintf(g.model, sizeof g.model, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--advertise") && i + 1 < argc)
            snprintf(g.advertise, sizeof g.advertise, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--include") && i + 1 < argc && g.nincl < MAX_INCLUDES)
            g.includes[g.nincl++] = argv[++i];
        else {
            fprintf(stderr, "usage: %s --root DIR [--port N] [--tracker H:P]"
                            " [--name S] [--advertise H:P] [--include PAT]...\n", argv[0]);
            return 2;
        }
    }
    if (!g.root[0]) { fprintf(stderr, "[maintainer] --root is required\n"); return 2; }
    size_t rl = strlen(g.root);
    while (rl > 1 && g.root[rl - 1] == '/') g.root[--rl] = 0;
    if (!g.name[0]) snprintf(g.name, sizeof g.name, "peer-%d", port);
    if (!g.model[0]) {   /* default model name: the directory's basename */
        const char *base = strrchr(g.root, '/');
        snprintf(g.model, sizeof g.model, "%s", base ? base + 1 : g.root);
    }
    if (!g.advertise[0]) snprintf(g.advertise, sizeof g.advertise, "127.0.0.1:%d", port);

    scan_dir(g.root);
    if (!g.nfiles) { fprintf(stderr, "[maintainer %s] nothing to serve under %s\n", g.name, g.root); return 1; }
    uint64_t total = 0;
    for (int i = 0; i < g.nfiles; i++) total += g.files[i].size;
    printf("[maintainer %s] holding %d files, %.1f GB, from %s\n",
           g.name, g.nfiles, (double)total / 1e9, g.root);
    fflush(stdout);

    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("[maintainer] listen"); return 1; }
    pthread_t t;
    if (g.tracker[0]) { pthread_create(&t, NULL, heartbeat_thread, NULL); pthread_detach(t); }
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
