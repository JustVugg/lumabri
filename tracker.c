/* tracker.c — the Napster part of lumibri: an index of who holds what.
 *
 * Maintainers REGISTER the list of files they hold and re-register every few
 * seconds as a heartbeat; chatters ask for a PLACEMENT: every file with the
 * live peers that hold it. No model bytes ever cross the tracker — transfers
 * are strictly peer-to-peer.
 *
 * State is in-memory only. A tracker restart costs nothing: every maintainer
 * re-registers within one heartbeat.
 *
 *   ./tracker [--port 7300]
 */
#include <pthread.h>
#include <time.h>

#include "lumibri_proto.h"

#define MAX_PEERS  64
#define MAX_FILES  4096
#define STALE_S    30.0     /* silent this long → dropped from placements */

typedef struct { char path[LMB_PATH_MAX]; uint64_t size; } PFile;

typedef struct {
    char name[64], addr[64];
    PFile *files; uint32_t nfiles;
    double ts;
    int used;
} Peer;

static Peer g_peers[MAX_PEERS];
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void send_err(int fd, const char *msg) {
    LmbBuf b = {0};
    lmb_buf_str(&b, msg);
    lmb_send(fd, LMB_ERR, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
}

static int handle_register(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char name[64], addr[64];
    uint32_t n;
    if (lmb_cur_str(&c, name, sizeof name) || lmb_cur_str(&c, addr, sizeof addr) ||
        lmb_cur_u32(&c, &n) || n > MAX_FILES) { send_err(fd, "bad register"); return -1; }
    PFile *files = (PFile *)calloc(n ? n : 1, sizeof *files);
    if (!files) { send_err(fd, "oom"); return -1; }
    for (uint32_t i = 0; i < n; i++)
        if (lmb_cur_str(&c, files[i].path, sizeof files[i].path) ||
            lmb_cur_u64(&c, &files[i].size)) {
            free(files); send_err(fd, "bad register entry"); return -1;
        }
    int fresh = 0;
    pthread_mutex_lock(&g_lk);
    Peer *slot = NULL;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; break; }
    if (!slot)
        for (int i = 0; i < MAX_PEERS; i++)
            if (!g_peers[i].used) { slot = &g_peers[i]; fresh = 1; break; }
    if (slot) {
        free(slot->files);
        slot->used = 1; slot->files = files; slot->nfiles = n; slot->ts = now_s();
        snprintf(slot->name, sizeof slot->name, "%s", name);
        snprintf(slot->addr, sizeof slot->addr, "%s", addr);
    }
    pthread_mutex_unlock(&g_lk);
    if (!slot) { free(files); send_err(fd, "peer table full"); return -1; }
    if (fresh)
        printf("[tracker] + %s @ %s (%u files)\n", name, addr, n), fflush(stdout);
    return lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
}

static int handle_placement(int fd) {
    /* merge live peers' manifests: path → {size, [addr...]} */
    typedef struct { const PFile *f; char addrs[8][64]; int naddr; } Entry;
    Entry *ent = (Entry *)calloc(MAX_FILES, sizeof *ent);
    int nent = 0;
    if (!ent) { send_err(fd, "oom"); return -1; }
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int p = 0; p < MAX_PEERS; p++) {
        if (!g_peers[p].used || now - g_peers[p].ts > STALE_S) continue;
        for (uint32_t i = 0; i < g_peers[p].nfiles; i++) {
            const PFile *f = &g_peers[p].files[i];
            Entry *e = NULL;
            for (int j = 0; j < nent; j++)
                if (!strcmp(ent[j].f->path, f->path)) { e = &ent[j]; break; }
            if (!e) {
                if (nent == MAX_FILES) continue;
                e = &ent[nent++]; e->f = f;
            }
            if (e->naddr < 8)
                snprintf(e->addrs[e->naddr++], 64, "%s", g_peers[p].addr);
        }
    }
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)nent);
    for (int j = 0; j < nent; j++) {
        lmb_buf_str(&b, ent[j].f->path);
        lmb_buf_u64(&b, ent[j].f->size);
        lmb_buf_u16(&b, (uint16_t)ent[j].naddr);
        for (int a = 0; a < ent[j].naddr; a++) lmb_buf_str(&b, ent[j].addrs[a]);
    }
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_PLACEMENT_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p); free(ent);
    return rc;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    LmbMsg m;
    while (lmb_recv(fd, &m) == 0) {
        int rc;
        switch (m.op) {
        case LMB_PING:      rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0); break;
        case LMB_REGISTER:  rc = handle_register(fd, &m); break;
        case LMB_PLACEMENT: rc = handle_placement(fd); break;
        default:            send_err(fd, "unknown op"); rc = -1; break;
        }
        lmb_msg_free(&m);
        if (rc) break;
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = 7300;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--port N]\n", argv[0]); return 2; }
    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("[tracker] listen"); return 1; }
    printf("[tracker] listening on :%d\n", port); fflush(stdout);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("[tracker] accept"); break; }
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(t);
        else
            close(fd);
    }
    return 0;
}
