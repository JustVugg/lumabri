/* lumabri.c — the lumabri front end: one binary, two roles.
 *
 *   lumabri serve --model DIR      share a model with the swarm
 *   lumabri chat                   chat with a model that lives on the swarm
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
 * The engines are taken exactly as they are, which means speaking both of
 * the protocols colibri ships: olmoe's line dialect (CHAT=1, a "> " prompt)
 * and everyone else's framed SERVE dialect (\x01\x01READY\x01\x01, streamed
 * tokens, \x01\x01END\x01\x01 + STAT). Which one is in front of us is
 * decided by whichever sentinel arrives first. No engine changes, no extra
 * daemon: the TUI is just a careful parent process.
 *
 * `chat --local DIR` skips the swarm entirely and reads a model that is
 * already on this disk — the right mode on the machine that serves it,
 * where mirroring would mean a second copy of the same bytes.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>

#include "lumabri_proto.h"
#include "lumabri_sign.h"

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
    "██╗     ██╗   ██╗███╗   ███╗ █████╗ ██████╗ ██████╗ ██╗",
    "██║     ██║   ██║████╗ ████║██╔══██╗██╔══██╗██╔══██╗██║",
    "██║     ██║   ██║██╔████╔██║███████║██████╔╝██████╔╝██║",
    "██║     ██║   ██║██║╚██╔╝██║██╔══██║██╔══██╗██╔══██╗██║",
    "███████╗╚██████╔╝██║ ╚═╝ ██║██║  ██║██████╔╝██║  ██║██║",
    "╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝  ╚═╝╚═════╝ ╚═╝  ╚═╝╚═╝",
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

/* Which expert-node binary can execute this model's experts, or NULL when
 * that engine has no phase-2 build yet. One per engine family: the engines
 * do not share an expert shape, so neither can the peers. */
static const char *expert_node_for(const char *model_type) {
    if (strstr(model_type, "olmoe"))    return "expert_node";
    if (strstr(model_type, "glm"))      return "expert_node_glm";
    if (strstr(model_type, "inkling"))  return "expert_node_inkling";
    if (strstr(model_type, "kimi"))     return "expert_node_kimi";
    /* deepseek_v4 chats fine — phase 1 serves its bytes and it speaks the
     * framed dialect — but it has no expert node: see the README. */
    return NULL;
}

/* model_type from a local config.json; "" when absent or unparseable */
static void local_model_type(const char *model_dir, char *out, size_t cap) {
    out[0] = 0;
    char p[1200], buf[4096];
    snprintf(p, sizeof p, "%s/config.json", model_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *mt = strstr(buf, "\"model_type\"");
    if (!mt) return;
    mt = strchr(mt + 12, '"');
    if (!mt) return;
    char *end = strchr(mt + 1, '"');
    if (end && (size_t)(end - mt - 1) < cap) {
        memcpy(out, mt + 1, (size_t)(end - mt - 1));
        out[end - mt - 1] = 0;
    }
}

static int cmd_serve(int argc, char **argv) {
    const char *model = NULL, *join = NULL, *mname = NULL, *donate = NULL;
    const char *key = NULL, *pubkey = NULL;
    int port = 7300, no_exec = 0, cache_slots = 128;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--model-name") && i + 1 < argc) mname = argv[++i];
        else if (!strcmp(argv[i], "--donate") && i + 1 < argc) donate = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc) pubkey = argv[++i];
        else if (!strcmp(argv[i], "--no-exec")) no_exec = 1;
        else if (!strcmp(argv[i], "--exec-cache") && i + 1 < argc) cache_slots = atoi(argv[++i]);
        else { fprintf(stderr, "usage: lumabri serve --model DIR [--port N] "
                               "[--join TRACKER] [--model-name S] [--donate GB] "
                               "[--key FILE] [--pubkey FILE] [--no-exec] "
                               "[--exec-cache N]\n"); return 2; }
    }
    if (!model) { fprintf(stderr, "usage: lumabri serve --model DIR [--port N]\n"); return 2; }
    if (donate && (!join || !mname)) {
        fprintf(stderr, "--donate needs --join TRACKER and --model-name NAME "
                        "(whose model to help hold)\n");
        return 2;
    }
    struct stat st;
    if (stat(model, &st)) mkdir_p(model);        /* a donor starts empty */
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
        /* LUMABRI_TOKEN makes the whole serve private: the spawned tracker
         * requires it, the maintainer inherits it from the environment */
        const char *tok = getenv("LUMABRI_TOKEN");
        char *targv[10];
        int t = 0;
        targv[t++] = tracker_bin;
        targv[t++] = "--port"; targv[t++] = portstr;
        if (tok && tok[0]) { targv[t++] = "--token"; targv[t++] = (char *)tok; }
        /* signing without also telling the tracker the public half would
         * leave it accepting unsigned claims from anyone: derive it here */
        if (pubkey) { targv[t++] = "--pubkey"; targv[t++] = (char *)pubkey; }
        else if (key) {
            static char pub[80];
            char hex[200] = "";
            FILE *kf = fopen(key, "r");
            uint8_t sk[64];
            if (kf && fscanf(kf, "%198s", hex) == 1 && strlen(hex) == 128 &&
                !lmb_unhex(sk, hex, 64)) {
                lmb_hex(pub, sk + 32, 32);
                targv[t++] = "--pubkey"; targv[t++] = pub;
            }
            if (kf) fclose(kf);
        }
        targv[t] = NULL;
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
    if (donate) { margv[a++] = "--donate"; margv[a++] = (char *)donate; }
    if (key) { margv[a++] = "--key"; margv[a++] = (char *)key; }
    margv[a] = NULL;
    g_children[g_nchildren++] = spawn_argv(margv);

    /* The bootstrap executor: when the model family has an expert node
     * build, serve also runs one on the whole model with an SSD-streaming
     * cache — so a brand-new swarm can chat phase-2 from minute zero with
     * this server executing every expert. Donors that join later are
     * discovered by the chatters and win the calls they are nearest for;
     * this node stays the replica of last resort. */
    char exec_bin[1200], mtype[64];
    local_model_type(model, mtype, sizeof mtype);
    /* one node binary per engine family — they do not share an expert shape */
    const char *node = expert_node_for(mtype);
    snprintf(exec_bin, sizeof exec_bin, "%s/%s", dir, node);
    int with_exec = 0;
    if (!no_exec && !node && mtype[0])
        printf("  %sfase 2 non disponibile per il motore %s: questo server "
               "serve i byte, gli esperti li esegue il chatter%s\n",
               C_DIM, mtype, C_R);
    if (!no_exec && node && access(exec_bin, X_OK))
        printf("  %s%s non è compilato: nessun esperto eseguito qui "
               "(make %s ENGINE=/path/to/colibri/c)%s\n", C_DIM, node, node, C_R);
    if (!no_exec && node && access(exec_bin, X_OK) == 0) {
        char eport[16], cachestr[16], ename[32];
        snprintf(eport, sizeof eport, "%d", port + 2);
        snprintf(cachestr, sizeof cachestr, "%d", cache_slots);
        snprintf(ename, sizeof ename, "exec-%d", port);
        char *eargv[16];
        a = 0;
        eargv[a++] = exec_bin;
        eargv[a++] = "--model"; eargv[a++] = (char *)model;
        eargv[a++] = "--port"; eargv[a++] = eport;
        eargv[a++] = "--tracker"; eargv[a++] = taddr;
        eargv[a++] = "--cache"; eargv[a++] = cachestr;
        eargv[a++] = "--name"; eargv[a++] = ename;
        if (mname) { eargv[a++] = "--model-name"; eargv[a++] = (char *)mname; }
        eargv[a] = NULL;
        g_children[g_nchildren++] = spawn_argv(eargv);
        with_exec = 1;
    }
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("\n%sserving%s %s %s(tracker %s%s)%s\n", C_GRN, C_R, model, C_DIM, taddr,
           with_exec ? " · executing experts for the swarm" : "", C_R);
    printf("%schat from this machine:   lumabri chat%s\n", C_DIM, C_R);
    printf("%schat from another one:    lumabri chat --tracker <this-ip>:%d%s\n\n",
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

    /* config.json: direct from the peer, else relayed through the tracker
     * (the peer may be behind a NAT and reachable only outbound) */
    LmbMsg r = {0};
    int fd = lmb_connect_ms(s->config_peer, 3000);
    if (fd >= 0 && lmb_auth(fd)) { close(fd); fd = -1; }
    if (fd >= 0) {
        LmbBuf b = {0};
        lmb_buf_str(&b, "config.json"); lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 1 << 20);
        rc = lmb_send(fd, LMB_READ, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        if (rc == 0) rc = lmb_recv(fd, &r);
        close(fd);
    } else rc = -1;
    if (rc || r.op != LMB_READ_R || !r.pay_len) {
        lmb_msg_free(&r);
        memset(&r, 0, sizeof r);
        LmbBuf b = {0};
        lmb_buf_str(&b, model ? model : "");
        lmb_buf_str(&b, "config.json");
        lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 1 << 20);
        rc = lmb_request(tracker, LMB_RREAD, b.p, (uint32_t)b.len, &r);
        free(b.p);
        if (rc || r.op != LMB_RREAD_R || !r.pay_len) { lmb_msg_free(&r); return -1; }
    }
    /* the payload is not NUL-terminated: parse a terminated copy, so a
     * truncated config can never send strchr past the allocation */
    char *cfg = malloc((size_t)r.pay_len + 1);
    if (cfg) {
        memcpy(cfg, r.pay, r.pay_len);
        cfg[r.pay_len] = 0;
        char *mt = strstr(cfg, "\"model_type\"");
        if (mt) {
            mt = strchr(mt + 12, '"');
            if (mt) {
                char *end = strchr(mt + 1, '"');
                if (end && end - mt - 1 < (long)sizeof s->model_type)
                    { memcpy(s->model_type, mt + 1, (size_t)(end - mt - 1));
                      s->model_type[end - mt - 1] = 0; }
            }
        }
        free(cfg);
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

/* ---- the engine child ----------------------------------------------------
 * Loading a model out of a swarm can take minutes, and for most of them the
 * only honest thing to show is what the engine and the shim are actually
 * doing. So: every line the child writes is kept (the last ETAIL of them),
 * the interesting numbers are parsed out of it, and if the child dies we
 * print that tail instead of a shrug. A silent failure here used to read as
 * "engine did not start", which is true and useless. */

#define ETAIL 120

static struct {
    volatile double net_mb;          /* fetched from the swarm this session */
    volatile double rate_mbs;
    volatile double total_gb;        /* the whole model, from the shim */
    volatile double local_gb;        /* already in the mirror when we started */
    volatile int    spinning;
    volatile int    booting;
    volatile double last_out;        /* when the child last said anything */
    char            phase[160];      /* its own words for what it is doing */
    char            tail[ETAIL][256];
    int             ntail;
    pthread_mutex_t lk;
} g_eng = { .lk = PTHREAD_MUTEX_INITIALIZER };

static void tail_push(const char *line) {
    pthread_mutex_lock(&g_eng.lk);
    snprintf(g_eng.tail[g_eng.ntail % ETAIL], sizeof g_eng.tail[0], "%s", line);
    g_eng.ntail++;
    pthread_mutex_unlock(&g_eng.lk);
}

/* what the child said before it died — the only thing worth printing then */
static void tail_dump(int max) {
    pthread_mutex_lock(&g_eng.lk);
    int n = g_eng.ntail < ETAIL ? g_eng.ntail : ETAIL;
    if (n > max) n = max;
    int first = g_eng.ntail - n;
    for (int i = first; i < g_eng.ntail; i++)
        fprintf(stderr, "  %s│%s %s\n", C_GRAY, C_R, g_eng.tail[i % ETAIL]);
    pthread_mutex_unlock(&g_eng.lk);
}

static void *stderr_thread(void *arg) {
    FILE *f = fdopen((int)(intptr_t)arg, "r");
    if (!f) return NULL;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        tail_push(line);
        g_eng.last_out = nowd();

        double mb, rate, gb, pct;
        if (sscanf(line, "[lumabri] net %lf MB", &mb) == 1) {
            g_eng.net_mb = mb;
            if (sscanf(strstr(line, "(") ? strstr(line, "(") : "", "(%lf MB/s", &rate) == 1)
                g_eng.rate_mbs = rate;
            continue;                       /* the spinner already shows this */
        }
        if (sscanf(line, "[lumabri] %*d files \xc2\xb7 %lf GB \xc2\xb7 %lf%%", &gb, &pct) == 2) {
            g_eng.total_gb = gb;
            g_eng.local_gb = gb * pct / 100.0;
        }
        /* while booting, everything: this is exactly when you need to see it */
        if (g_eng.booting) {
            snprintf(g_eng.phase, sizeof g_eng.phase, "%s", line);
            if (!g_tty) fprintf(stderr, "  %s%s%s\n", C_DIM, line, C_R);
            continue;
        }
        if (strstr(line, "[lumabri]") || strstr(line, "resident weights") ||
            strstr(line, "[chat]") || strstr(line, "[USAGE]"))
            fprintf(stderr, "%s  %s%s\n", C_DIM, line, C_R);
    }
    fclose(f);
    return NULL;
}

/* one line, rewritten in place: the star, what it is doing, how far along */
static void *spinner_thread(void *arg) {
    const char *verb = arg ? (const char *)arg : "thinking";
    const char *star[] = { "\xe2\x9c\xbb", "\xe2\x9c\xb2", "\xe2\x9c\xb3", "\xe2\x9c\xb2" };
    const char *tint[] = { "\x1b[38;5;209m", "\x1b[38;5;216m",
                           "\x1b[38;5;223m", "\x1b[38;5;216m" };
    double t0 = nowd();
    int i = 0, stalled = 0;
    while (g_eng.spinning) {
        char what[200] = "";
        if (g_eng.booting && g_eng.phase[0]) {
            const char *p = g_eng.phase;
            if (!strncmp(p, "[lumabri] ", 10)) p += 10;
            snprintf(what, sizeof what, "%.*s", 68, p);
        } else
            snprintf(what, sizeof what, "%s", verb);

        char prog[160] = "";
        double got = g_eng.local_gb + g_eng.net_mb / 1000.0;
        if (g_eng.booting && g_eng.total_gb > 0)
            snprintf(prog, sizeof prog, " %s\xc2\xb7 %.1f/%.0f GB \xc2\xb7 %.0f MB/s%s",
                     C_DIM, got, g_eng.total_gb, g_eng.rate_mbs, C_R);
        else if (g_eng.booting && g_eng.net_mb > 0)
            snprintf(prog, sizeof prog, " %s\xc2\xb7 %.0f MB%s", C_DIM, g_eng.net_mb, C_R);

        fprintf(stderr, "\r\x1b[2K%s%s%s %s%s\xe2\x80\xa6%s%s %s%.0fs%s",
                tint[i & 3], star[i & 3], C_R, C_DIM, what, C_R, prog,
                C_GRAY, nowd() - t0, C_R);
        fflush(stderr);

        /* nothing from the child and nothing off the wire: say so once, with
         * the two things that actually explain it */
        if (!stalled && g_eng.booting && g_eng.last_out > 0 &&
            nowd() - g_eng.last_out > 90 && g_eng.rate_mbs < 0.05) {
            fprintf(stderr, "\r\x1b[2K  %s90s senza un byte n\xc3\xa9 una riga dal motore. "
                            "Se \xc3\xa8 la prima volta pu\xc3\xb2 essere l'hashing del modello "
                            "lato server; altrimenti guarda `df -h` e `dmesg | tail`.%s\n",
                    C_DIM, C_R);
            stalled = 1;
        }
        i++;
        usleep(160 * 1000);
    }
    fprintf(stderr, "\r\x1b[2K");
    return NULL;
}

/* ---- the two engine dialects ---------------------------------------------
 * colibri ships several engines and they do NOT speak the same protocol:
 *
 *   olmoe            CHAT=1. Readiness and end of turn are both a "> "
 *                    prompt; one line in, the whole reply out.
 *   colibri (glm),   SERVE=1. Framed and streaming: \x01\x01READY\x01\x01
 *   deepseek,        once after the load, then every turn streams its tokens
 *   kimi_k3,         and closes with \x01\x01END\x01\x01 plus a STAT line.
 *   inkling          Reset is the control byte line \x02RESET.
 *
 * We set both variables — each engine ignores the one that is not its own —
 * and learn which dialect we are hearing from whichever sentinel arrives
 * first. This used to assume olmoe unconditionally, so with any other engine
 * we waited for a "> " that would never come, until the child exited: the
 * "engine did not start" that had nothing to do with starting.
 */
#define FRAME_READY "\x01\x01" "READY" "\x01\x01"
#define FRAME_END   "\x01\x01" "END" "\x01\x01"

typedef enum { PROTO_UNKNOWN = 0, PROTO_LINE, PROTO_FRAMED } Proto;
typedef struct { pid_t pid; int to, from; Proto proto; } Engine;

static char *read_until_prompt(int fd) {
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 512 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
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

/* Wait for readiness in either dialect, and remember which one it was.
 * Returns 0, or -1 if the child died first. */
static int engine_wait_ready(Engine *e) {
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    for (;;) {
        if (len + 512 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t r = read(e->from, buf + len, 512);
        if (r <= 0) { free(buf); return -1; }
        len += (size_t)r;
        buf[len] = 0;
        if (memmem(buf, len, FRAME_READY, strlen(FRAME_READY)))
            { e->proto = PROTO_FRAMED; free(buf); return 0; }
        if ((len >= 3 && !memcmp(buf + len - 3, "\n> ", 3)) ||
            (len == 2 && !memcmp(buf, "> ", 2)))
            { e->proto = PROTO_LINE; free(buf); return 0; }
    }
}

/* Framed turn: print the tokens as they arrive, stop at the END sentinel,
 * then pick up the STAT line that follows it. */
static int stream_until_end(Engine *e, char *statline, size_t scap) {
    const char *S = FRAME_END;
    size_t SL = strlen(S), cap = 8192, len = 0, shown = 0;
    char *buf = malloc(cap), *hit = NULL;
    if (statline && scap) statline[0] = 0;
    if (!buf) return -1;
    for (;;) {
        if (len + 1024 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t r = read(e->from, buf + len, 1024);
        if (r <= 0) { free(buf); return -1; }
        len += (size_t)r;
        buf[len] = 0;
        hit = memmem(buf, len, S, SL);
        /* hold back SL-1 bytes: a sentinel may straddle two reads */
        size_t safe = hit ? (size_t)(hit - buf) : (len > SL ? len - SL : 0);
        if (safe > shown) {
            fwrite(buf + shown, 1, safe - shown, stdout);
            fflush(stdout);
            shown = safe;
        }
        if (hit) break;
    }
    size_t after = (size_t)(hit - buf) + SL;
    char rest[256];
    size_t rl = len > after ? len - after : 0;
    if (rl > sizeof rest - 1) rl = sizeof rest - 1;
    if (rl) memcpy(rest, buf + after, rl);
    rest[rl] = 0;
    free(buf);
    for (;;) {
        char *st = strstr(rest, "STAT "), *nl = st ? strchr(st, '\n') : NULL;
        if (nl) {
            if (statline && scap) snprintf(statline, scap, "%.*s", (int)(nl - st), st);
            return 0;
        }
        if (rl + 1 >= sizeof rest) return 0;             /* no STAT: harmless */
        ssize_t r = read(e->from, rest + rl, sizeof rest - 1 - rl);
        if (r <= 0) return 0;
        rl += (size_t)r;
        rest[rl] = 0;
    }
}

static const char *engine_for(const char *model_type) {
    if (strstr(model_type, "olmoe")) return "olmoe";
    if (strstr(model_type, "deepseek")) return "deepseek";
    if (strstr(model_type, "kimi")) return "kimi_k3";
    if (strstr(model_type, "inkling")) return "inkling";
    return "colibri";
}

/* `local_dir` non-NULL: the model is already on this disk, so no shim, no
 * mirror, no second copy. That is the right mode on the machine that serves
 * the model — otherwise chatting there downloads it from itself. */
static int engine_spawn(const char *engine, const char *shim, const char *tracker,
                        const char *model, const char *local_dir,
                        int ctx, int max_new, int cap_experts, Engine *e) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char vroot[1024], cache[1024];
    snprintf(vroot, sizeof vroot, "%s/.lumabri/%s/vroot", home, model);
    snprintf(cache, sizeof cache, "%s/.lumabri/%s/cache", home, model);
    if (!local_dir) mkdir_p(cache);   /* vroot stays virtual on purpose */

    /* olmoe takes <cap> <bits>; the SERVE-mode engines take <cap> alone and
     * read the quantization out of the file — passing bits there would
     * override what the model actually is */
    int line_proto = strstr(engine, "olmoe") != NULL;
    char cap_s[32];
    snprintf(cap_s, sizeof cap_s, "%d", cap_experts);

    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], 0); dup2(out_pipe[1], 1); dup2(err_pipe[1], 2);
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        char env_ctx[32], env_new[32];
        snprintf(env_ctx, sizeof env_ctx, "%d", ctx);
        snprintf(env_new, sizeof env_new, "%d", max_new);
        if (local_dir) {
            setenv("SNAP", local_dir, 1);
        } else {
            setenv("LD_PRELOAD", shim, 1);
            setenv("LUMABRI_VROOT", vroot, 1);
            setenv("LUMABRI_CACHE", cache, 1);
            setenv("LUMABRI_TRACKER", tracker, 1);
            setenv("LUMABRI_MODEL", model, 1);
            setenv("LUMABRI_STATS", "2", 1);       /* boot progress, not a log */
            setenv("SNAP", vroot, 1);
        }
        setenv("CHAT", "1", 1);                    /* olmoe's dialect */
        setenv("SERVE", "1", 1);                   /* everyone else's */
        setenv("KV_SLOTS", "1", 1);
        setenv("CTX", env_ctx, 1);
        setenv("MAX_NEW", env_new, 1);
        setenv("NGEN", env_new, 1);                /* SERVE mode calls it NGEN */
        char *eargv[] = { (char *)engine, cap_s, line_proto ? "8" : NULL, NULL };
        execv(engine, eargv);
        perror(engine);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    e->pid = pid; e->to = in_pipe[1]; e->from = out_pipe[0];
    e->proto = PROTO_UNKNOWN;
    pthread_t t;
    pthread_create(&t, NULL, stderr_thread, (void *)(intptr_t)err_pipe[0]);
    pthread_detach(t);
    return 0;
}

/* Why the child is gone, in the words of the kernel and of the child. */
static void engine_diag(Engine *e) {
    int st = 0;
    if (e->pid > 0 && waitpid(e->pid, &st, WNOHANG) == e->pid) {
        e->pid = 0;
        if (WIFSIGNALED(st)) {
            int s = WTERMSIG(st);
            printf("  %sil motore è stato ucciso dal kernel (segnale %d: %s)%s\n",
                   C_RED, s, strsignal(s), C_R);
            if (s == SIGKILL)
                printf("  %squasi sempre è la RAM: `dmesg | grep -i oom` lo conferma. "
                       "Riduci --ctx e --cap, o prendi una macchina con più memoria.%s\n",
                       C_DIM, C_R);
        } else if (WIFEXITED(st)) {
            int c = WEXITSTATUS(st);
            printf("  %sil motore è uscito con codice %d%s\n", C_RED, c, C_R);
            if (c == 127)
                printf("  %sil binario non è partito affatto: libreria mancante? "
                       "provalo a mano con `ldd`.%s\n", C_DIM, C_R);
        }
    } else
        printf("  %sil motore ha chiuso il suo stdout senza dire di essere pronto%s\n",
               C_RED, C_R);
    printf("  %sultime righe del motore:%s\n", C_DIM, C_R);
    tail_dump(25);
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
    if (engine_path) { snprintf(out, cap, "%s", engine_path); return access(out, X_OK); }
    const char *eng = engine_for(model_type);
    const char *dir = engines_dir ? engines_dir : "../moe-stream/c";
    /* prefer the P2P build when one exists: it is the same engine (identical
     * without expert peers) plus the ability to run the routed experts on
     * the swarm when the tracker offers executors */
    char me[1200];
    exe_dir(me, sizeof me);
    snprintf(out, cap, "%s/%s_p2p", dir, eng);
    if (access(out, X_OK) == 0) return 0;
    snprintf(out, cap, "%s/%s_p2p", me, eng);
    if (access(out, X_OK) == 0) return 0;
    snprintf(out, cap, "%s/%s", dir, eng);
    return access(out, X_OK);
}

/* Is there room for the mirror? The chatter keeps its own copy of every
 * block it touches, so a 300 GB model needs 300 GB here — the single most
 * common way this goes wrong, and it goes wrong hours in, silently. */
static void disk_preflight(const char *model, uint64_t model_bytes) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char cache[1024];
    snprintf(cache, sizeof cache, "%s/.lumabri", home);
    mkdir_p(cache);
    struct statvfs vfs;
    if (statvfs(cache, &vfs)) return;
    double free_gb = (double)vfs.f_bavail * (double)vfs.f_frsize / 1e9;
    double need_gb = (double)model_bytes / 1e9;
    printf("  %smirror in %s: %.0f GB liberi. Tiene solo i blocchi che tocchi "
           "— la parte densa sempre, gli esperti solo se nessun peer li esegue "
           "(al limite %.0f GB)%s\n",
           C_DIM, cache, free_gb, need_gb, C_R);
    /* the dense part is the floor; a tenth of the model is a generous guess
     * at it, and below that even a warm phase-2 chatter cannot boot */
    if (free_gb < need_gb * 0.1)
        printf("  %s⚠ %.0f GB liberi non bastano nemmeno per la parte densa. "
               "Se il modello è già su questo disco usa `--local DIR`: la chat "
               "lo legge dov'è, senza copiarne un byte.%s\n",
               C_RED, free_gb, C_R);
    else if (free_gb < need_gb)
        printf("  %snon c'è spazio per il modello intero: va bene finché gli "
               "esperti girano sui peer, ma se lo sciame si svuota la chat si "
               "ferma per disco pieno.%s\n", C_DIM, C_R);
}

/* boot one model: inspect, resolve, spawn, wait for readiness */
static int model_boot(const char *tracker, const char *model, const char *shim,
                      const char *engines_dir, const char *engine_path,
                      const char *local_dir, int ctx, int max_new, int cap_experts,
                      Engine *e, Swarm *sw) {
    char mtype[64] = "";
    memset(sw, 0, sizeof *sw);
    if (local_dir) {
        local_model_type(local_dir, mtype, sizeof mtype);
        printf("  %smodello locale %s%s%s%s · niente rete, niente mirror%s\n",
               C_DIM, C_R, C_BOLD, local_dir, C_DIM, C_R);
    } else {
        printf("  %schiedo allo sciame chi ha %s…%s\n", C_DIM, model, C_R);
        if (swarm_inspect(tracker, model, sw)) {
            printf("  %smodel %s: nobody on the swarm has it%s\n", C_RED, model, C_R);
            return -1;
        }
        snprintf(mtype, sizeof mtype, "%s", sw->model_type);
        printf("  %s%d file · %.0f GB · %d peer · tipo %s%s\n", C_DIM,
               sw->nfiles, (double)sw->total_bytes / 1e9, sw->npeers,
               mtype[0] ? mtype : "?", C_R);
        disk_preflight(model, sw->total_bytes);
    }

    char engine[1200];
    if (resolve_engine(engines_dir, engine_path, mtype, engine, sizeof engine)) {
        printf("  %sengine not found: %s%s\n"
               "  point me at a colibri build with --engine or --engines-dir\n",
               C_RED, engine, C_R);
        return -1;
    }
    printf("  %smotore %s%s\n", C_DIM, engine, C_R);
    if (!local_dir)
        printf("  %sora scarico la parte densa una volta sola — gli esperti "
               "restano sullo sciame%s\n", C_DIM, C_R);

    if (engine_spawn(engine, shim, tracker, model, local_dir,
                     ctx, max_new, cap_experts, e)) return -1;

    g_eng.booting = 1;
    g_eng.last_out = nowd();
    g_eng.spinning = 1;
    pthread_t tspin;
    if (g_tty) pthread_create(&tspin, NULL, spinner_thread, (void *)"lo sciame si scalda");
    double t0 = nowd();
    int ready = engine_wait_ready(e);
    g_eng.spinning = 0;
    if (g_tty) pthread_join(tspin, NULL);
    g_eng.booting = 0;
    if (ready) {
        printf("  %s✗ il motore non è arrivato a essere pronto%s\n", C_RED, C_R);
        engine_diag(e);
        engine_stop(e);
        return -1;
    }
    printf("  %s\xe2\x9c\x93 %s pronto in %.1fs%s%s · net %.0f MB · "
           "/swarm /model /reset /quit%s\n",
           C_GRN, model, nowd() - t0, C_R, C_DIM, g_eng.net_mb, C_R);
    return 0;
}

static int cmd_chat(int argc, char **argv) {
    const char *tracker = "127.0.0.1:7300";
    const char *engine_path = NULL, *engines_dir = getenv("LUMABRI_ENGINES");
    const char *want_model = NULL, *local_dir = NULL;
    int max_new = 256, ctx = 2048, cap_experts = 64;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--tracker") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--engines-dir") && i + 1 < argc) engines_dir = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) want_model = argv[++i];
        else if (!strcmp(argv[i], "--local") && i + 1 < argc) local_dir = argv[++i];
        else if (!strcmp(argv[i], "--max-new") && i + 1 < argc) max_new = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctx") && i + 1 < argc) ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cap") && i + 1 < argc) cap_experts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--plain")) g_tty = 0;
        else { fprintf(stderr, "usage: lumabri chat [--tracker H:P] [--model NAME] "
                               "[--local DIR] [--engine BIN] [--engines-dir DIR]\n"
                               "                    [--max-new N] [--ctx N] [--cap N]\n");
               return 2; }
    }

    char models[16][64];
    int nmodels = 0;
    char model[64];
    if (local_dir) {
        const char *base = strrchr(local_dir, '/');
        snprintf(model, sizeof model, "%s", base && base[1] ? base + 1 : local_dir);
    } else {
        nmodels = swarm_models(tracker, models, 16);
        if (nmodels <= 0) {
            fprintf(stderr, "%sno swarm at %s%s\n"
                            "start one with:  lumabri serve --model <dir>\n"
                            "or chat with a model already on this disk:  "
                            "lumabri chat --local <dir>\n", C_RED, tracker, C_R);
            return 1;
        }
        snprintf(model, sizeof model, "%s", want_model ? want_model : models[0]);
    }

    char dir[1024], shim[1200];
    exe_dir(dir, sizeof dir);
    snprintf(shim, sizeof shim, "%s/liblumabri.so", dir);
    if (access(shim, R_OK))       /* installed layout: bin/../lib/lumabri/ */
        snprintf(shim, sizeof shim, "%s/../lib/lumabri/liblumabri.so", dir);
    if (!local_dir && access(shim, R_OK)) {
        fprintf(stderr, "liblumabri.so missing; run make (or make install)\n");
        return 1;
    }

    Swarm sw;
    Engine eng = {0};

    /* welcome panel: the wordmark, then the spark line */
    int W = term_w() - 2;
    if (W > 66) W = 66;
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

    if (model_boot(tracker, model, shim, engines_dir, engine_path, local_dir,
                   ctx, max_new, cap_experts, &eng, &sw))
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
            if (local_dir) { printf("  %s--local: un modello solo%s\n", C_DIM, C_R); continue; }
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
            if (model_boot(tracker, model, shim, engines_dir, engine_path, local_dir,
                           ctx, max_new, cap_experts, &eng, &sw))
                return 1;
            continue;
        }

        int is_reset = !strcmp(line, "/reset");
        if (eng.proto == PROTO_FRAMED) {
            /* framed dialect: reset is a control byte, everything else is the
             * prompt line as-is */
            const char *send = is_reset ? "\x02RESET" : line;
            if (write(eng.to, send, strlen(send)) < 0) break;
            if (write(eng.to, "\n", 1) < 0) break;
        } else {
            line[L] = '\n';
            if (write(eng.to, line, L + 1) < 0) break;
            line[L] = 0;
            if (is_reset) {
                printf("  %s\xe2\x9c\xa6 nuova conversazione%s\n", C_DIM, C_R);
                continue;
            }
        }

        double m0 = g_eng.net_mb, r0 = nowd();
        char stat[128] = "";

        if (eng.proto == PROTO_FRAMED) {
            if (!is_reset) printf("%s%s\xe2\x97\x86 %s%s\n  ", C_BOLD, C_CORAL, model, C_R);
            if (stream_until_end(&eng, stat, sizeof stat)) {
                fprintf(stderr, "\n%sengine exited%s\n", C_RED, C_R);
                engine_diag(&eng);
                break;
            }
            printf("\n");
            if (is_reset) { printf("  %s\xe2\x9c\xa6 nuova conversazione%s\n", C_DIM, C_R); continue; }
        } else {
            g_eng.spinning = 1;
            pthread_t tspin;
            if (g_tty) pthread_create(&tspin, NULL, spinner_thread, NULL);
            char *reply = read_until_prompt(eng.from);
            g_eng.spinning = 0;
            if (g_tty) pthread_join(tspin, NULL);
            if (!reply) {
                fprintf(stderr, "%sengine exited%s\n", C_RED, C_R);
                engine_diag(&eng);
                break;
            }
            char *text = reply;
            while (*text == '\n') text++;
            printf("%s%s\xe2\x97\x86 %s%s\n", C_BOLD, C_CORAL, model, C_R);
            printf("  %s\n", text);
            free(reply);
        }

        /* STAT <tokens> <tok/s> <cache hit%> <rss GB> */
        double tps = 0, hit = 0, rss = 0;
        int ntok = 0;
        int nstat = sscanf(stat, "STAT %d %lf %lf %lf", &ntok, &tps, &hit, &rss);
        double dmb = g_eng.net_mb - m0;
        printf("%s  %.1fs", C_DIM, nowd() - r0);
        if (nstat >= 2 && tps > 0) printf(" · %.1f tok/s", tps);
        if (nstat >= 4 && rss > 0) printf(" · %.1f GB residenti", rss);
        if (local_dir)    printf(" · disco locale");
        else if (dmb > 0.5) printf(" · %.0f MB dallo sciame · mirror %.0f MB", dmb, g_eng.net_mb);
        else                printf(" · mirror caldo, zero rete");
        printf("%s\n", C_R);
    }

    engine_stop(&eng);
    printf("\n");
    return 0;
}

/* ---- key: the operator's identity ---------------------------------------
 * Ed25519 keypair. The secret signs the swarm's ground truth and belongs
 * only on the machine that owns the model — ideally offline, since the
 * signatures are computed once. The public half is what everyone else
 * needs, and it is the ONLY thing a chatter must get out of band: with it,
 * neither the tracker nor any peer has to be trusted. */
static int cmd_key(int argc, char **argv) {
    const char *out = "lumabri";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else { fprintf(stderr, "usage: lumabri key [--out NAME]\n"); return 2; }
    }
    uint8_t seed[32], pk[32], sk[64];
    FILE *ur = fopen("/dev/urandom", "rb");
    if (!ur || fread(seed, 1, 32, ur) != 32) {
        fprintf(stderr, "cannot read 32 random bytes from /dev/urandom\n");
        if (ur) fclose(ur);
        return 1;
    }
    fclose(ur);
    lmb_sign_keypair(pk, sk, seed);

    char skpath[1100], pkpath[1100], hex[200];
    snprintf(skpath, sizeof skpath, "%s.key", out);
    snprintf(pkpath, sizeof pkpath, "%s.pub", out);
    int fd = open(skpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* secret: 0600 */
    if (fd < 0) { perror(skpath); return 1; }
    lmb_hex(hex, sk, 64);
    if (write(fd, hex, strlen(hex)) < 0 || write(fd, "\n", 1) < 0) { perror(skpath); close(fd); return 1; }
    close(fd);
    FILE *pf = fopen(pkpath, "w");
    if (!pf) { perror(pkpath); return 1; }
    lmb_hex(hex, pk, 32);
    fprintf(pf, "%s\n", hex);
    fclose(pf);

    printf("\n  %ssecret%s %s  %s(0600 — keep it off the swarm)%s\n",
           C_BOLD, C_R, skpath, C_DIM, C_R);
    printf("  %spublic%s %s  %s%s%s\n\n", C_BOLD, C_R, pkpath, C_DIM, hex, C_R);
    printf("  serve the model as its origin:\n");
    printf("    %slumabri serve --model DIR --key %s%s\n", C_DIM, skpath, C_R);
    printf("  let everyone verify (give them the public value, not the file):\n");
    printf("    %sLUMABRI_PUBKEY=%s lumabri chat --tracker HOST:7300%s\n\n",
           C_DIM, hex, C_R);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
    g_tty = isatty(1);
    if (argc >= 2 && !strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "chat"))  return cmd_chat(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "key"))   return cmd_key(argc - 2, argv + 2);
    fprintf(stderr,
        "lumabri: run huge models from a swarm of peers\n\n"
        "  lumabri serve --model DIR [--port 7300] [--join TRACKER]   share a model\n"
        "  lumabri chat  [--tracker HOST:7300] [--model NAME]         chat with it\n"
        "  lumabri key   [--out NAME]                                 operator keypair\n");
    return 2;
}
