/* lumibri.c — the lumibri front end: one binary, two roles.
 *
 *   lumibri serve --model DIR      share a model with the swarm
 *   lumibri chat                   chat with a model that lives on the swarm
 *
 * `serve` runs the tracker and a maintainer for the given directory.
 * `chat` asks the tracker what is available, mounts the model through the
 * LD_PRELOAD shim (nothing is downloaded up front; blocks arrive on first
 * touch and stay in the local mirror), spawns the UNMODIFIED colibri engine
 * in its interactive CHAT mode, and wraps it in a terminal UI.
 *
 * The engine protocol is the one run_chat() already speaks: a "\n> " prompt
 * on stdout marks readiness, one line in, the whole reply out. No engine
 * changes, no extra daemon: the TUI is just a careful parent process.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "lumibri_proto.h"

/* ---- terminal ----------------------------------------------------------- */

static int g_tty = 0;
#define C_DIM   (g_tty ? "\x1b[2m"  : "")
#define C_BOLD  (g_tty ? "\x1b[1m"  : "")
#define C_TEAL  (g_tty ? "\x1b[36m" : "")
#define C_GRN   (g_tty ? "\x1b[32m" : "")
#define C_RED   (g_tty ? "\x1b[31m" : "")
#define C_R     (g_tty ? "\x1b[0m"  : "")

static double nowd(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void exe_dir(char *dst, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", dst, cap - 1);
    if (n <= 0) { snprintf(dst, cap, "."); return; }
    dst[n] = 0;
    char *slash = strrchr(dst, '/');
    if (slash) *slash = 0;
}

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* ---- serve -------------------------------------------------------------- */

static pid_t spawn_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], argv); perror(argv[0]); _exit(127); }
    return pid;
}

static pid_t g_children[8];
static int g_nchildren = 0;

static void on_sigint(int sig) {
    (void)sig;
    for (int i = 0; i < g_nchildren; i++) kill(g_children[i], SIGTERM);
}

static int cmd_serve(int argc, char **argv) {
    const char *model = NULL;
    int port = 7300;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else { fprintf(stderr, "usage: lumibri serve --model DIR [--port N]\n"); return 2; }
    }
    if (!model) { fprintf(stderr, "usage: lumibri serve --model DIR [--port N]\n"); return 2; }
    struct stat st;
    if (stat(model, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: not a directory\n", model); return 1;
    }

    char dir[1024], tracker_bin[1200], maint_bin[1200], portstr[16], mport[16], taddr[64];
    exe_dir(dir, sizeof dir);
    snprintf(tracker_bin, sizeof tracker_bin, "%s/tracker", dir);
    snprintf(maint_bin, sizeof maint_bin, "%s/maintainer", dir);
    snprintf(portstr, sizeof portstr, "%d", port);
    snprintf(mport, sizeof mport, "%d", port + 1);
    snprintf(taddr, sizeof taddr, "127.0.0.1:%d", port);

    char *targv[] = { tracker_bin, "--port", portstr, NULL };
    char *margv[] = { maint_bin, "--root", (char *)model, "--port", mport,
                      "--tracker", taddr, "--name", "origin", NULL };
    g_children[g_nchildren++] = spawn_argv(targv);
    usleep(300 * 1000);
    g_children[g_nchildren++] = spawn_argv(margv);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("\n%sserving%s %s on port %d\n", C_GRN, C_R, model, port);
    printf("%schat from this machine:   lumibri chat%s\n", C_DIM, C_R);
    printf("%schat from another one:    lumibri chat --tracker <this-ip>:%d%s\n\n",
           C_DIM, port, C_R);
    while (g_nchildren) {
        int status;
        pid_t p = wait(&status);
        if (p < 0 && errno == EINTR) continue;
        if (p < 0) break;
        for (int i = 0; i < g_nchildren; i++)
            if (g_children[i] == p) g_children[i] = g_children[--g_nchildren];
    }
    return 0;
}

/* ---- chat: swarm inspection --------------------------------------------- */

typedef struct {
    char peers[8][64]; int npeers;
    uint64_t total_bytes; int nfiles;
    char config_peer[64];       /* a peer that holds config.json */
    char model_type[64];
} Swarm;

static int swarm_inspect(const char *tracker, Swarm *s) {
    memset(s, 0, sizeof *s);
    LmbMsg m = {0};
    if (lmb_request(tracker, LMB_PLACEMENT, NULL, 0, &m) || m.op != LMB_PLACEMENT_R)
        return -1;
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) { lmb_msg_free(&m); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        char rel[LMB_PATH_MAX], addr[64];
        uint64_t size; uint16_t np;
        if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &size) ||
            lmb_cur_u16(&c, &np)) { lmb_msg_free(&m); return -1; }
        s->nfiles++; s->total_bytes += size;
        int is_cfg = !strcmp(rel, "config.json");
        for (uint16_t p = 0; p < np; p++) {
            if (lmb_cur_str(&c, addr, sizeof addr)) { lmb_msg_free(&m); return -1; }
            if (is_cfg && !s->config_peer[0])
                snprintf(s->config_peer, sizeof s->config_peer, "%s", addr);
            int seen = 0;
            for (int k = 0; k < s->npeers; k++) if (!strcmp(s->peers[k], addr)) seen = 1;
            if (!seen && s->npeers < 8)
                snprintf(s->peers[s->npeers++], 64, "%s", addr);
        }
    }
    lmb_msg_free(&m);
    if (!s->nfiles || !s->config_peer[0]) return -1;

    /* read config.json straight off the peer to learn the architecture */
    int fd = lmb_connect(s->config_peer);
    if (fd < 0) return -1;
    LmbBuf b = {0};
    lmb_buf_str(&b, "config.json"); lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 1 << 20);
    int rc = lmb_send(fd, LMB_READ, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    LmbMsg r = {0};
    if (rc == 0) rc = lmb_recv(fd, &r);
    close(fd);
    if (rc || r.op != LMB_READ_R || !r.pay_len) { lmb_msg_free(&r); return -1; }
    char *cfg = (char *)r.pay;
    char *mt = memmem(cfg, r.pay_len, "\"model_type\"", 12);
    if (mt) {
        mt = strchr(mt + 12, '"');
        if (mt) {
            char *end = strchr(mt + 1, '"');
            if (end && end - mt - 1 < (long)sizeof s->model_type)
                { memcpy(s->model_type, mt + 1, (size_t)(end - mt - 1));
                  s->model_type[end - mt - 1] = 0; }
        }
    }
    lmb_msg_free(&r);
    return 0;
}

/* ---- chat: engine child ------------------------------------------------- */

static struct {
    volatile double net_mb;
    volatile int spinning;
    int err_fd;
} g_eng = {0};

static void *stderr_thread(void *arg) {
    (void)arg;
    FILE *f = fdopen(g_eng.err_fd, "r");
    if (!f) return NULL;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        double mb;
        if (sscanf(line, "[lumibri] net %lf MB", &mb) == 1) { g_eng.net_mb = mb; continue; }
        if (strstr(line, "[lumibri]") || strstr(line, "resident weights") ||
            strstr(line, "[chat]") || strstr(line, "[USAGE]"))
            fprintf(stderr, "%s  %s%s", C_DIM, line, C_R);
    }
    fclose(f);
    return NULL;
}

static void *spinner_thread(void *arg) {
    (void)arg;
    const char *f[] = { "|", "/", "-", "\\" };
    int i = 0;
    while (g_eng.spinning) {
        fprintf(stderr, "\r%s  %s thinking%s ", C_DIM, f[i++ & 3], C_R);
        fflush(stderr);
        usleep(120 * 1000);
    }
    fprintf(stderr, "\r                    \r");
    return NULL;
}

/* Read engine stdout until the "\n> " prompt (or "> " at stream start).
 * Everything before it is the reply. Returns malloc'd reply or NULL on EOF. */
static char *read_until_prompt(int fd) {
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 512 > cap) { cap *= 2; buf = realloc(buf, cap); }
        ssize_t r = read(fd, buf + len, 512);
        if (r <= 0) { free(buf); return NULL; }
        len += (size_t)r;
        buf[len] = 0;
        if ((len >= 3 && !memcmp(buf + len - 3, "\n> ", 3)) ||
            (len == 2 && !memcmp(buf, "> ", 2))) {
            buf[len >= 3 ? len - 3 : 0] = 0;
            return buf;
        }
    }
}

static const char *engine_for(const char *model_type) {
    if (strstr(model_type, "olmoe")) return "olmoe";
    if (strstr(model_type, "deepseek")) return "deepseek";
    if (strstr(model_type, "kimi")) return "kimi_k3";
    if (strstr(model_type, "inkling")) return "inkling";
    return "colibri";
}

static int cmd_chat(int argc, char **argv) {
    const char *tracker = "127.0.0.1:7300";
    const char *engine_path = NULL, *engines_dir = getenv("LUMIBRI_ENGINES");
    const char *name = "default";
    int max_new = 256, ctx = 2048;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--tracker") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--engines-dir") && i + 1 < argc) engines_dir = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--max-new") && i + 1 < argc) max_new = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctx") && i + 1 < argc) ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--plain")) g_tty = 0;
        else { fprintf(stderr, "usage: lumibri chat [--tracker H:P] [--engine BIN] "
                               "[--engines-dir DIR] [--name S] [--max-new N] [--ctx N]\n");
               return 2; }
    }

    printf("\n%s%slumibri%s %s· colibri over the swarm%s\n", C_BOLD, C_TEAL, C_R, C_DIM, C_R);

    Swarm sw;
    if (swarm_inspect(tracker, &sw)) {
        fprintf(stderr, "%sno model on the swarm at %s%s\n"
                        "start one with:  lumibri serve --model <dir>\n", C_RED, tracker, C_R);
        return 1;
    }
    const char *eng_name = engine_for(sw.model_type);
    printf("%s  tracker %s · %d file, %.1f GB · %d peer · engine %s%s\n\n",
           C_DIM, tracker, sw.nfiles, (double)sw.total_bytes / 1e9, sw.npeers, eng_name, C_R);

    char dir[1024], shim[1200], engine[1200], vroot[1024], cache[1024];
    exe_dir(dir, sizeof dir);
    snprintf(shim, sizeof shim, "%s/liblumibri.so", dir);
    if (access(shim, R_OK)) { fprintf(stderr, "%s missing; run make\n", shim); return 1; }
    if (engine_path) snprintf(engine, sizeof engine, "%s", engine_path);
    else snprintf(engine, sizeof engine, "%s/%s",
                  engines_dir ? engines_dir : "../moe-stream/c", eng_name);
    if (access(engine, X_OK)) {
        fprintf(stderr, "%sengine not found: %s%s\n"
                        "point me at a colibri build with --engine or --engines-dir\n",
                C_RED, engine, C_R);
        return 1;
    }

    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    snprintf(vroot, sizeof vroot, "%s/.lumibri/%s/vroot", home, name);
    snprintf(cache, sizeof cache, "%s/.lumibri/%s/cache", home, name);
    mkdir_p(cache);   /* vroot stays virtual on purpose */

    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], 0); dup2(out_pipe[1], 1); dup2(err_pipe[1], 2);
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        char env_ctx[32], env_new[32];
        snprintf(env_ctx, sizeof env_ctx, "%d", ctx);
        snprintf(env_new, sizeof env_new, "%d", max_new);
        setenv("LD_PRELOAD", shim, 1);
        setenv("LUMIBRI_VROOT", vroot, 1);
        setenv("LUMIBRI_CACHE", cache, 1);
        setenv("LUMIBRI_TRACKER", tracker, 1);
        setenv("LUMIBRI_STATS", "5", 1);
        setenv("SNAP", vroot, 1);
        setenv("CHAT", "1", 1);
        setenv("CTX", env_ctx, 1);
        setenv("MAX_NEW", env_new, 1);
        char *eargv[] = { engine, "64", "8", NULL };   /* cache slots, int8 */
        execv(engine, eargv);
        perror(engine);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    g_eng.err_fd = err_pipe[0];
    pthread_t terr;
    pthread_create(&terr, NULL, stderr_thread, NULL);

    /* engine start: dense weights arrive through the swarm on first run */
    g_eng.spinning = 1;
    pthread_t tspin;
    if (g_tty) pthread_create(&tspin, NULL, spinner_thread, NULL);
    double t0 = nowd();
    char *banner = read_until_prompt(out_pipe[0]);
    g_eng.spinning = 0;
    if (g_tty) pthread_join(tspin, NULL);
    if (!banner) { fprintf(stderr, "%sengine did not start%s\n", C_RED, C_R); return 1; }
    char *nl = banner;                       /* dense-load line, worth showing */
    while (*nl == '\n') nl++;
    if (*nl) printf("%s  %s%s\n", C_DIM, nl, C_R);
    free(banner);
    printf("%s  ready in %.1fs · net %.0f MB · type a message, /reset, /quit%s\n",
           C_DIM, nowd() - t0, g_eng.net_mb, C_R);

    char line[4096];
    for (;;) {
        if (g_tty) printf("\n%s%s>%s ", C_BOLD, C_TEAL, C_R);
        else       printf("\n> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) continue;
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;

        line[L] = '\n';
        if (write(in_pipe[1], line, L + 1) < 0) break;
        line[L] = 0;
        if (!strcmp(line, "/reset")) continue;

        double m0 = g_eng.net_mb, r0 = nowd();
        g_eng.spinning = 1;
        if (g_tty) pthread_create(&tspin, NULL, spinner_thread, NULL);
        char *reply = read_until_prompt(out_pipe[0]);
        g_eng.spinning = 0;
        if (g_tty) pthread_join(tspin, NULL);
        if (!reply) { fprintf(stderr, "%sengine exited%s\n", C_RED, C_R); break; }

        char *text = reply;
        while (*text == '\n') text++;
        printf("%s%s◆%s %s\n", C_BOLD, C_TEAL, C_R, text);
        printf("%s  %.1fs · net +%.0f MB (tot %.0f)%s\n",
               C_DIM, nowd() - r0, g_eng.net_mb - m0, g_eng.net_mb, C_R);
        free(reply);
    }

    close(in_pipe[1]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("\n");
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
    g_tty = isatty(1);
    if (argc >= 2 && !strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "chat"))  return cmd_chat(argc - 2, argv + 2);
    fprintf(stderr,
        "lumibri: run huge models from a swarm of peers\n\n"
        "  lumibri serve --model DIR [--port 7300]   share a model\n"
        "  lumibri chat  [--tracker HOST:7300]       chat with it\n");
    return 2;
}
