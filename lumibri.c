/* lumibri.c — the lumibri front end: one binary, two roles.
 *
 *   lumibri serve --model DIR      share a model with the swarm
 *   lumibri chat                   chat with a model that lives on the swarm
 *
 * `serve` runs the tracker and a maintainer for the given directory.
 * `chat` asks the tracker what is available, mounts the chosen model through
 * the LD_PRELOAD shim (nothing is downloaded up front; blocks arrive on
 * first touch and stay in the local mirror), spawns the UNMODIFIED colibri
 * engine in its interactive CHAT mode, and wraps it in a terminal UI.
 *
 * In-chat commands: /swarm (anonymous network status), /model (list and
 * switch model, restarting the engine), /reset, /quit.
 *
 * The engine protocol is the one run_chat() already speaks: a "\n> " prompt
 * on stdout marks readiness, one line in, the whole reply out. No engine
 * changes, no extra daemon: the TUI is just a careful parent process.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "lumibri_proto.h"

/* ---- terminal ----------------------------------------------------------- */

static int g_tty = 0;
#define C_DIM   (g_tty ? "\x1b[2m"  : "")
#define C_BOLD  (g_tty ? "\x1b[1m"  : "")
#define C_GRN   (g_tty ? "\x1b[32m" : "")
#define C_RED   (g_tty ? "\x1b[31m" : "")
#define C_R     (g_tty ? "\x1b[0m"  : "")
#define C_CORAL (g_tty ? "\x1b[38;5;209m" : "")   /* the accent */
#define C_GRAY  (g_tty ? "\x1b[38;5;242m" : "")   /* borders */

static double nowd(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int term_w(void) {
    struct winsize ws;
    if (g_tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) return ws.ws_col;
    return 80;
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

/* ---- the logo ------------------------------------------------------------
 * ANSI-Shadow block wordmark, warm gradient from coral to sand, one tint
 * per row. The same lettering every serious CLI splash uses. */
static const char *WORDMARK[6] = {
    "██╗     ██╗   ██╗███╗   ███╗██╗██████╗ ██████╗ ██╗",
    "██║     ██║   ██║████╗ ████║██║██╔══██╗██╔══██╗██║",
    "██║     ██║   ██║██╔████╔██║██║██████╔╝██████╔╝██║",
    "██║     ██║   ██║██║╚██╔╝██║██║██╔══██╗██╔══██╗██║",
    "███████╗╚██████╔╝██║ ╚═╝ ██║██║██████╔╝██║  ██║██║",
    "╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝╚═════╝ ╚═╝  ╚═╝╚═╝",
};
static const int WORD_TINT[6] = { 203, 209, 209, 215, 216, 223 };

static void hline(const char *l, const char *r, int w) {
    printf("%s%s", C_GRAY, l);
    for (int i = 0; i < w - 2; i++) printf("\xe2\x94\x80");
    printf("%s%s\n", r, C_R);
}

/* visible width of a string carrying ANSI escapes and UTF-8 */
static int vis_len(const char *s) {
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\x1b') { while (*p && *p != 'm') p++; continue; }
        if ((*p & 0xC0) != 0x80) v++;
    }
    return v;
}

static void panel_row(int w, const char *left, const char *right) {
    int pad = w - 2 - 2 - vis_len(left) - 3 - vis_len(right);
    if (pad < 0) pad = 0;
    printf("%s\xe2\x94\x82%s  %s   %s%*s%s\xe2\x94\x82%s\n",
           C_GRAY, C_R, left, right, pad, "", C_GRAY, C_R);
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
    const char *model = NULL, *join = NULL, *mname = NULL;
    int port = 7300;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--model-name") && i + 1 < argc) mname = argv[++i];
        else { fprintf(stderr, "usage: lumibri serve --model DIR [--port N] "
                               "[--join TRACKER] [--model-name S]\n"); return 2; }
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
    if (join) snprintf(taddr, sizeof taddr, "%s", join);
    else      snprintf(taddr, sizeof taddr, "127.0.0.1:%d", port);

    if (!join) {
        char *targv[] = { tracker_bin, "--port", portstr, NULL };
        g_children[g_nchildren++] = spawn_argv(targv);
        usleep(300 * 1000);
    }
    char *margv[16];
    int a = 0;
    margv[a++] = maint_bin;
    margv[a++] = "--root"; margv[a++] = (char *)model;
    margv[a++] = "--port"; margv[a++] = mport;
    margv[a++] = "--tracker"; margv[a++] = taddr;
    if (mname) { margv[a++] = "--model-name"; margv[a++] = (char *)mname; }
    margv[a] = NULL;
    g_children[g_nchildren++] = spawn_argv(margv);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("\n%sserving%s %s %s(tracker %s)%s\n", C_GRN, C_R, model, C_DIM, taddr, C_R);
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

/* ---- swarm inspection --------------------------------------------------- */

typedef struct {
    char peers[8][64]; int npeers;
    uint64_t total_bytes; int nfiles;
    char config_peer[64];
    char model_type[64];
} Swarm;

static int swarm_inspect(const char *tracker, const char *model, Swarm *s) {
    memset(s, 0, sizeof *s);
    LmbMsg m = {0};
    LmbBuf fb = {0};
    if (model && model[0]) lmb_buf_str(&fb, model);
    int rc = lmb_request(tracker, LMB_PLACEMENT, fb.p, (uint32_t)fb.len, &m);
    free(fb.p);
    if (rc || m.op != LMB_PLACEMENT_R) return -1;
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

    int fd = lmb_connect(s->config_peer);
    if (fd < 0) return -1;
    LmbBuf b = {0};
    lmb_buf_str(&b, "config.json"); lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 1 << 20);
    rc = lmb_send(fd, LMB_READ, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    LmbMsg r = {0};
    if (rc == 0) rc = lmb_recv(fd, &r);
    close(fd);
    if (rc || r.op != LMB_READ_R || !r.pay_len) { lmb_msg_free(&r); return -1; }
    char *mt = memmem((char *)r.pay, r.pay_len, "\"model_type\"", 12);
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

typedef struct {
    char model[64];
    uint64_t held, served_bytes, served_reads;
    uint32_t age_s, nfiles;
} SwarmRow;

static int swarm_stats(const char *tracker, SwarmRow *rows, int cap) {
    LmbMsg m = {0};
    if (lmb_request(tracker, LMB_SWARM, NULL, 0, &m) || m.op != LMB_SWARM_R) return -1;
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) { lmb_msg_free(&m); return -1; }
    int out = 0;
    for (uint32_t i = 0; i < n && out < cap; i++) {
        SwarmRow *r = &rows[out];
        if (lmb_cur_str(&c, r->model, sizeof r->model) ||
            lmb_cur_u64(&c, &r->held) || lmb_cur_u64(&c, &r->served_bytes) ||
            lmb_cur_u64(&c, &r->served_reads) || lmb_cur_u32(&c, &r->age_s) ||
            lmb_cur_u32(&c, &r->nfiles)) break;
        out++;
    }
    lmb_msg_free(&m);
    return out;
}

/* /swarm: the network, anonymous. Peers are numbered, never named. */
static void render_swarm(const char *tracker) {
    SwarmRow rows[64];
    int n = swarm_stats(tracker, rows, 64);
    if (n < 0) { printf("  %stracker unreachable%s\n", C_RED, C_R); return; }
    char lines[65][256];
    snprintf(lines[0], sizeof lines[0], "%s%sla rete adesso%s  %s%d peer vivi%s",
             C_BOLD, C_CORAL, C_R, C_DIM, n, C_R);
    for (int i = 0; i < n; i++)
        snprintf(lines[i + 1], sizeof lines[0],
                 "%speer-%d%s  %-12.12s %5.1f GB  %s%.0f MB out · %llu req · hb %us%s",
                 C_BOLD, i + 1, C_R, rows[i].model, (double)rows[i].held / 1e9,
                 C_DIM, (double)rows[i].served_bytes / 1e6,
                 (unsigned long long)rows[i].served_reads, rows[i].age_s, C_R);
    /* the box fits its widest line; the terminal caps it */
    int w = 0;
    for (int i = 0; i <= n; i++)
        if (vis_len(lines[i]) + 6 > w) w = vis_len(lines[i]) + 6;
    if (w > term_w() - 2) w = term_w() - 2;
    printf("\n");
    hline("\xe2\x95\xad", "\xe2\x95\xae", w);
    for (int i = 0; i <= n; i++) {
        int pad = w - 4 - vis_len(lines[i]);
        printf("%s\xe2\x94\x82%s  %s%*s%s\xe2\x94\x82%s\n", C_GRAY, C_R, lines[i],
               pad > 0 ? pad : 0, "", C_GRAY, C_R);
    }
    hline("\xe2\x95\xb0", "\xe2\x95\xaf", w);
}

/* distinct model names on the swarm; returns count */
static int swarm_models(const char *tracker, char names[][64], int cap) {
    SwarmRow rows[64];
    int n = swarm_stats(tracker, rows, 64), out = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < out; j++) if (!strcmp(names[j], rows[i].model)) seen = 1;
        if (!seen && out < cap) snprintf(names[out++], 64, "%s", rows[i].model);
    }
    return out;
}

/* ---- the engine child --------------------------------------------------- */

static struct {
    volatile double net_mb;
    volatile int spinning;
} g_eng = {0};

static void *stderr_thread(void *arg) {
    FILE *f = fdopen((int)(intptr_t)arg, "r");
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
    const char *verb = arg ? (const char *)arg : "thinking";
    const char *star[] = { "\xe2\x9c\xbb", "\xe2\x9c\xb2", "\xe2\x9c\xb3", "\xe2\x9c\xb2" };
    const char *tint[] = { "\x1b[38;5;209m", "\x1b[38;5;216m",
                           "\x1b[38;5;223m", "\x1b[38;5;216m" };
    int i = 0;
    while (g_eng.spinning) {
        fprintf(stderr, "\r%s%s%s %s%s\xe2\x80\xa6%s ",
                tint[i & 3], star[i & 3], C_R, C_DIM, verb, C_R);
        fflush(stderr);
        i++;
        usleep(160 * 1000);
    }
    fprintf(stderr, "\r\x1b[2K");
    return NULL;
}

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

typedef struct { pid_t pid; int to, from; } Engine;

static int engine_spawn(const char *engine, const char *shim, const char *tracker,
                        const char *model, int ctx, int max_new, Engine *e) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char vroot[1024], cache[1024];
    snprintf(vroot, sizeof vroot, "%s/.lumibri/%s/vroot", home, model);
    snprintf(cache, sizeof cache, "%s/.lumibri/%s/cache", home, model);
    mkdir_p(cache);   /* vroot stays virtual on purpose */

    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) return -1;
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
        setenv("LUMIBRI_MODEL", model, 1);
        setenv("LUMIBRI_STATS", "5", 1);
        setenv("SNAP", vroot, 1);
        setenv("CHAT", "1", 1);
        setenv("CTX", env_ctx, 1);
        setenv("MAX_NEW", env_new, 1);
        char *eargv[] = { (char *)engine, "64", "8", NULL };
        execv(engine, eargv);
        perror(engine);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    e->pid = pid; e->to = in_pipe[1]; e->from = out_pipe[0];
    pthread_t t;
    pthread_create(&t, NULL, stderr_thread, (void *)(intptr_t)err_pipe[0]);
    pthread_detach(t);
    return 0;
}

static void engine_stop(Engine *e) {
    if (e->pid <= 0) return;
    close(e->to); close(e->from);
    kill(e->pid, SIGTERM);
    waitpid(e->pid, NULL, 0);
    e->pid = 0;
}

/* ---- chat --------------------------------------------------------------- */

static int resolve_engine(const char *engines_dir, const char *engine_path,
                          const char *model_type, char *out, size_t cap) {
    if (engine_path) snprintf(out, cap, "%s", engine_path);
    else snprintf(out, cap, "%s/%s",
                  engines_dir ? engines_dir : "../moe-stream/c", engine_for(model_type));
    return access(out, X_OK);
}

/* boot one model: inspect, resolve, spawn, wait for the prompt */
static int model_boot(const char *tracker, const char *model, const char *shim,
                      const char *engines_dir, const char *engine_path,
                      int ctx, int max_new, Engine *e, Swarm *sw) {
    if (swarm_inspect(tracker, model, sw)) {
        printf("  %smodel %s: nobody on the swarm has it%s\n", C_RED, model, C_R);
        return -1;
    }
    char engine[1200];
    if (resolve_engine(engines_dir, engine_path, sw->model_type, engine, sizeof engine)) {
        printf("  %sengine not found: %s%s\n"
               "  point me at a colibri build with --engine or --engines-dir\n",
               C_RED, engine, C_R);
        return -1;
    }
    if (engine_spawn(engine, shim, tracker, model, ctx, max_new, e)) return -1;

    g_eng.spinning = 1;
    pthread_t tspin;
    if (g_tty) pthread_create(&tspin, NULL, spinner_thread, (void *)"lo sciame si scalda");
    double t0 = nowd();
    char *banner = read_until_prompt(e->from);
    g_eng.spinning = 0;
    if (g_tty) pthread_join(tspin, NULL);
    if (!banner) {
        printf("  %sengine did not start%s\n", C_RED, C_R);
        engine_stop(e);
        return -1;
    }
    free(banner);
    printf("  %s\xe2\x9c\x93 %s pronto in %.1fs%s%s · net %.0f MB · "
           "/swarm /model /reset /quit%s\n",
           C_GRN, model, nowd() - t0, C_R, C_DIM, g_eng.net_mb, C_R);
    return 0;
}

static int cmd_chat(int argc, char **argv) {
    const char *tracker = "127.0.0.1:7300";
    const char *engine_path = NULL, *engines_dir = getenv("LUMIBRI_ENGINES");
    const char *want_model = NULL;
    int max_new = 256, ctx = 2048;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--tracker") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--engines-dir") && i + 1 < argc) engines_dir = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) want_model = argv[++i];
        else if (!strcmp(argv[i], "--max-new") && i + 1 < argc) max_new = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctx") && i + 1 < argc) ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--plain")) g_tty = 0;
        else { fprintf(stderr, "usage: lumibri chat [--tracker H:P] [--model NAME] "
                               "[--engine BIN] [--engines-dir DIR] [--max-new N] [--ctx N]\n");
               return 2; }
    }

    char models[16][64];
    int nmodels = swarm_models(tracker, models, 16);
    if (nmodels <= 0) {
        fprintf(stderr, "%sno swarm at %s%s\n"
                        "start one with:  lumibri serve --model <dir>\n", C_RED, tracker, C_R);
        return 1;
    }
    char model[64];
    snprintf(model, sizeof model, "%s", want_model ? want_model : models[0]);

    char dir[1024], shim[1200];
    exe_dir(dir, sizeof dir);
    snprintf(shim, sizeof shim, "%s/liblumibri.so", dir);
    if (access(shim, R_OK)) { fprintf(stderr, "%s missing; run make\n", shim); return 1; }

    Swarm sw;
    Engine eng = {0};

    /* welcome panel: the wordmark, then the spark line */
    int W = term_w() - 2;
    if (W > 58) W = 58;
    printf("\n");
    hline("\xe2\x95\xad", "\xe2\x95\xae", W);
    panel_row(W, "", "");
    for (int r = 0; r < 6; r++) {
        char row[512];
        if (g_tty) snprintf(row, sizeof row, "\x1b[38;5;%dm%s\x1b[0m",
                            WORD_TINT[r], WORDMARK[r]);
        else       snprintf(row, sizeof row, "%s", WORDMARK[r]);
        panel_row(W, row, "");
    }
    char tag[256];
    snprintf(tag, sizeof tag, "%s\xe2\x9c\xbb%s %stiny engine, immense swarm%s",
             C_CORAL, C_R, C_DIM, C_R);
    panel_row(W, "", "");
    panel_row(W, tag, "");
    hline("\xe2\x95\xb0", "\xe2\x95\xaf", W);
    if (nmodels > 1) {
        printf("  %s%d modelli sullo sciame:%s", C_DIM, nmodels, C_R);
        for (int i = 0; i < nmodels; i++) printf(" %s%s%s", C_BOLD, models[i], C_R);
        printf("  %s(/model per cambiare)%s\n", C_DIM, C_R);
    }

    if (model_boot(tracker, model, shim, engines_dir, engine_path, ctx, max_new, &eng, &sw))
        return 1;

    char line[4096];
    for (;;) {
        int w = term_w() - 2;
        if (g_tty) {
            printf("\n");
            hline("\xe2\x95\xad", "\xe2\x95\xae", w);
            printf("%s\xe2\x94\x82%s %s%s\xe2\x80\xba%s ", C_GRAY, C_R, C_CORAL, C_BOLD, C_R);
        } else
            printf("\n> ");
        fflush(stdout);
        int got = fgets(line, sizeof line, stdin) != NULL;
        if (g_tty) hline("\xe2\x95\xb0", "\xe2\x95\xaf", w);
        if (!got) break;
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) continue;
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
        if (!strcmp(line, "/swarm")) { render_swarm(tracker); continue; }
        if (!strncmp(line, "/model", 6)) {
            const char *arg = line + 6;
            while (*arg == ' ') arg++;
            nmodels = swarm_models(tracker, models, 16);
            if (!*arg) {
                printf("  %smodelli:%s", C_DIM, C_R);
                for (int i = 0; i < nmodels; i++)
                    printf(" %s%s%s%s", strcmp(models[i], model) ? "" : "*",
                           C_BOLD, models[i], C_R);
                printf("  %s/model <nome> per cambiare%s\n", C_DIM, C_R);
                continue;
            }
            if (!strcmp(arg, model)) { printf("  %sgià su %s%s\n", C_DIM, model, C_R); continue; }
            engine_stop(&eng);
            snprintf(model, sizeof model, "%s", arg);
            if (model_boot(tracker, model, shim, engines_dir, engine_path,
                           ctx, max_new, &eng, &sw))
                return 1;
            continue;
        }

        line[L] = '\n';
        if (write(eng.to, line, L + 1) < 0) break;
        line[L] = 0;
        if (!strcmp(line, "/reset")) {
            printf("  %s\xe2\x9c\xa6 nuova conversazione%s\n", C_DIM, C_R);
            continue;
        }

        double m0 = g_eng.net_mb, r0 = nowd();
        g_eng.spinning = 1;
        pthread_t tspin;
        if (g_tty) pthread_create(&tspin, NULL, spinner_thread, NULL);
        char *reply = read_until_prompt(eng.from);
        g_eng.spinning = 0;
        if (g_tty) pthread_join(tspin, NULL);
        if (!reply) { fprintf(stderr, "%sengine exited%s\n", C_RED, C_R); break; }

        char *text = reply;
        while (*text == '\n') text++;
        printf("%s%s\xe2\x97\x86 %s%s\n", C_BOLD, C_CORAL, model, C_R);
        printf("  %s\n", text);
        double dmb = g_eng.net_mb - m0;
        if (dmb > 0.5)
            printf("%s  %.1fs · %.0f MB dallo sciame · mirror %.0f MB%s\n",
                   C_DIM, nowd() - r0, dmb, g_eng.net_mb, C_R);
        else
            printf("%s  %.1fs · mirror caldo, zero rete%s\n", C_DIM, nowd() - r0, C_R);
        free(reply);
    }

    engine_stop(&eng);
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
        "  lumibri serve --model DIR [--port 7300] [--join TRACKER]   share a model\n"
        "  lumibri chat  [--tracker HOST:7300] [--model NAME]         chat with it\n");
    return 2;
}
