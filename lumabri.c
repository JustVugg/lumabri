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
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"

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

/* snprintf truncation is especially dangerous for executable, tracker and
 * model names: a valid-looking prefix can select the wrong resource. Keep
 * the convenience of formatting, but make every such truncation an error. */
static int checked_printf(char *dst, size_t cap, const char *fmt, ...) {
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

static int tracker_addr_set(char *dst, size_t cap, const char *input) {
    static const char port[] = ":7300";
    size_t n = strlen(input);
    size_t extra = strchr(input, ':') ? 0 : sizeof port - 1;
    if (n >= cap || extra > cap - n - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dst, input, n);
    if (extra) memcpy(dst + n, port, sizeof port);
    else dst[n] = 0;
    return 0;
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

/* Donor processes used to inherit the chat's terminal, so their stats and
 * retries printed straight into the streamed reply ("Page[donor-exec] 3114
 * exec calls…"). Give each donor its own log file instead; /debug tails it.
 * If the log cannot be opened the donor still runs on the terminal — a
 * noisy donation beats a missed one. `envv` carries KEY=VAL pairs for the
 * child's environment only: the swarm-fed expert node needs the shim wired
 * up (LD_PRELOAD and the mirror's coordinates), and none of that may leak
 * into our own process — a chat client running behind its own shim would
 * mirror every file it touches. */
static char g_donor_logs[8][1200];
static int g_ndonor_logs = 0;

static const char *donor_log_path(const char *name, char *buf, size_t cap) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/.lumabri/logs", home);
    mkdir_p(dir);
    snprintf(buf, cap, "%s/%s.log", dir, name);
    if (g_ndonor_logs < 8)
        snprintf(g_donor_logs[g_ndonor_logs++], sizeof g_donor_logs[0], "%s", buf);
    return buf;
}

static pid_t spawn_argv_logged(char *const argv[], char *const envv[],
                               const char *logpath) {
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; envv && envv[i]; i++) putenv(envv[i]);
        int lfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd >= 0) { dup2(lfd, 1); dup2(lfd, 2); close(lfd); }
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    return pid;
}

static pid_t g_children[8];
static int g_nchildren = 0;
/* The async handler never reads the mutable table/count.  Each fixed slot is
 * one sig_atomic_t publication, so delivery on any worker thread cannot see a
 * compacted/torn child table. */
static volatile sig_atomic_t g_signal_children[8];
/* what each child was, and how to start it again: a supervisor that cannot
 * name or restart what died is just a process that happens to be the parent */
static char **g_cargv[8];
static const char *g_cwhat[8];

static void child_publish(int idx, pid_t pid) {
    g_children[idx] = pid;
    g_signal_children[idx] = (sig_atomic_t)pid;
}
static void child_unpublish(int idx) {
    g_signal_children[idx] = 0;
    g_children[idx] = 0;
}

static void spawn_tracked(char *const argv[], const char *what) {
    int n = 0;
    while (argv[n]) n++;
    char **copy = (char **)calloc((size_t)n + 1, sizeof *copy);
    for (int i = 0; i < n; i++) copy[i] = strdup(argv[i]);
    pid_t pid = spawn_argv(argv);          /* the slow part, outside the mask */
    int idx = g_nchildren;
    g_cargv[idx] = copy;
    g_cwhat[idx] = what;
    child_publish(idx, pid);
    g_nchildren++;
}

static volatile sig_atomic_t g_stopping = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_stopping = 1;
    for (int i = 0; i < 8; i++) {
        sig_atomic_t p = g_signal_children[i];
        if (p > 0) kill((pid_t)p, SIGTERM);
    }
}

/* Which expert-node binary can execute this model's experts, or NULL when
 * that engine has no phase-2 build yet. One per engine family: the engines
 * do not share an expert shape, so neither can the peers. */
static const char *expert_node_for(const char *model_type) {
    if (strstr(model_type, "olmoe"))    return "expert_node";
    if (strstr(model_type, "glm"))      return "expert_node_glm";
    if (strstr(model_type, "inkling"))  return "expert_node_inkling";
    if (strstr(model_type, "kimi"))     return "expert_node_kimi";
    if (strstr(model_type, "deepseek")) return "expert_node_deepseek";
    if (strstr(model_type, "qwen"))     return "expert_node_qwen36";
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

static int parse_serve_port(const char *s, int *port) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (s[0] < '0' || s[0] > '9' || errno == ERANGE || !end || *end ||
        v < 1 || v > 65533) return -1;
    *port = (int)v;
    return 0;
}

static int cmd_serve(int argc, char **argv) {
    const char *model = NULL, *join = NULL, *mname = NULL, *donate = NULL;
    const char *key = NULL, *pubkey = NULL, *advertise = NULL;
    int port = 7300, no_exec = 0, cache_slots = 128;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            if (parse_serve_port(argv[++i], &port)) {
                fprintf(stderr, "--port must be an integer from 1 to 65533\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--model-name") && i + 1 < argc) mname = argv[++i];
        else if (!strcmp(argv[i], "--donate") && i + 1 < argc) donate = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc) pubkey = argv[++i];
        else if (!strcmp(argv[i], "--advertise") && i + 1 < argc) advertise = argv[++i];
        else if (!strcmp(argv[i], "--no-exec")) no_exec = 1;
        else if (!strcmp(argv[i], "--exec-cache") && i + 1 < argc) cache_slots = atoi(argv[++i]);
        else { fprintf(stderr, "usage: lumabri serve --model DIR [--port N] "
                               "[--join TRACKER] [--model-name S] [--donate GB] "
                               "[--key FILE] [--pubkey FILE] [--advertise HOST] "
                               "[--no-exec] [--exec-cache N]\n"); return 2; }
    }
    if (!model) { fprintf(stderr, "usage: lumabri serve --model DIR [--port N]\n"); return 2; }
    if (donate && (!join || !join[0] || !mname || !mname[0])) {
        fprintf(stderr, "--donate needs --join TRACKER and --model-name NAME "
                        "(whose model to help hold)\n");
        return 2;
    }
    if (donate) {
        char *end = NULL;
        double gb = strtod(donate, &end);
        if (end == donate || *end || !(gb > 0) || !isfinite(gb) ||
            gb * 1e9 < 1 || gb > (double)UINT64_MAX / 1e9) {
            fprintf(stderr, "--donate needs a positive number of GB\n");
            return 2;
        }
    }
    int disk_donor = donate != NULL;
    struct stat st;
    if (stat(model, &st) && disk_donor) mkdir_p(model);  /* a donor starts empty */
    if (stat(model, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: not a directory\n", model); return 1;
    }
    /* A directory without config.json is not a model, and letting it through
     * produces three disconnected symptoms for one cause: the maintainer
     * serves bytes happily, no expert node starts (the engine family is
     * unknown), and every chatter is told "nobody on the swarm has it"
     * because the config it needs is not at the root. A disk donor is the
     * exception: its directory starts empty and the tracker fills the slice. */
    {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/config.json", model);
        if (!disk_donor && access(probe, R_OK)) {
            fprintf(stderr, "%s%s non contiene config.json — non e' una "
                            "directory di modello%s\n"
                            "  (se il modello e' in una sottocartella, punta "
                            "li: --model %s/<sottocartella>)\n",
                    C_RED, model, C_R, model);
            return 2;
        }
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
        spawn_tracked(targv, "il tracker");
        usleep(300 * 1000);
    }
    char *margv[24];
    int a = 0;
    margv[a++] = maint_bin;
    margv[a++] = "--root"; margv[a++] = (char *)model;
    margv[a++] = "--port"; margv[a++] = mport;
    margv[a++] = "--tracker"; margv[a++] = taddr;
    if (mname) { margv[a++] = "--model-name"; margv[a++] = (char *)mname; }
    if (donate) { margv[a++] = "--donate"; margv[a++] = (char *)donate; }
    if (key) { margv[a++] = "--key"; margv[a++] = (char *)key; }
    /* a donor pulls other people's bytes: give it the operator key so it can
     * refuse anything the operator did not sign, instead of holding it */
    if (pubkey) { margv[a++] = "--pubkey"; margv[a++] = (char *)pubkey; }
    static char madv[80];
    if (advertise) {
        snprintf(madv, sizeof madv, "%s:%d", advertise, port + 1);
        margv[a++] = "--advertise"; margv[a++] = madv;
    }
    margv[a] = NULL;
    spawn_tracked(margv, "il maintainer");

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
    if (node) snprintf(exec_bin, sizeof exec_bin, "%s/%s", dir, node);
    int with_exec = 0;
    if (!no_exec && !disk_donor && !node && mtype[0])
        printf("  %sfase 2 non disponibile per il motore %s: questo server "
               "serve i byte, gli esperti li esegue il chatter%s\n",
               C_DIM, mtype, C_R);
    if (!no_exec && !disk_donor && node && access(exec_bin, X_OK))
        printf("  %s%s non è compilato: nessun esperto eseguito qui "
               "(make %s ENGINE=/path/to/colibri/c)%s\n", C_DIM, node, node, C_R);
    if (!no_exec && !disk_donor && node && access(exec_bin, X_OK) == 0) {
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
        static char eadv[80];
        if (advertise) {
            snprintf(eadv, sizeof eadv, "%s:%d", advertise, port + 2);
            eargv[a++] = "--advertise"; eargv[a++] = eadv;
        }
        eargv[a] = NULL;
        spawn_tracked(eargv, "l'esecutore di esperti");
        with_exec = 1;
    }
    /* The node opens its port only after loading the dense side, which on a
     * big model is minutes. Until then a chatter that connects sees no
     * executor and concludes phase 2 is impossible — so say it, rather than
     * let someone race their own server and blame the swarm. */
    if (with_exec)
        printf("  %sl'esecutore sta caricando il modello: la porta %d si apre "
               "quando ha finito. Un chatter che si collega prima non lo "
               "trovera' e scarichera' gli esperti.%s\n",
               C_DIM, port + 2, C_R);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("\n%sserving%s %s %s(tracker %s%s)%s\n", C_GRN, C_R, model, C_DIM, taddr,
           with_exec ? " · executing experts for the swarm" : "", C_R);
    /* Without --advertise every local peer registers as 127.0.0.1, and the
     * tracker's correction cannot help because these registrations arrive
     * over loopback. A remote chatter then uses the READ/EXEC tracker relay.
     * That works through NAT but adds an extra hop and centralises traffic,
     * so say plainly that --advertise is required for the direct P2P path. */
    if (!advertise && !join)
        printf("%s⚠ nessun --advertise: i chatter remoti useranno il relay del "
               "tracker per READ ed EXEC.%s\n"
               "%s  Funziona anche dietro NAT, ma aggiunge un hop e carica il "
               "tracker; --advertise abilita il P2P diretto.%s\n"
               "%s  Per il percorso diretto: lumabri serve --model %s --advertise <ip-pubblico>%s\n",
               C_RED, C_R, C_DIM, C_R, C_DIM, model, C_R);
    printf("%schat from this machine:   lumabri chat%s\n", C_DIM, C_R);
    printf("%schat from another one:    lumabri chat --tracker <this-ip>:%d%s\n\n",
           C_DIM, port, C_R);
    while (g_nchildren) {
        int status;
        pid_t p = wait(&status);
        if (p < 0 && errno == EINTR) continue;
        if (p < 0) break;
        int idx = -1;
        for (int i = 0; i < g_nchildren; i++) if (g_children[i] == p) idx = i;
        if (idx >= 0 && g_cargv[idx] && !g_stopping) {
            /* Losing a child silently is the expensive failure: with the
             * executor gone every chatter falls back to downloading experts
             * and nothing anywhere says why. Name it, and bring it back. */
            if (WIFSIGNALED(status))
                printf("\n%s⚠ %s e' stato ucciso (segnale %d)%s\n",
                       C_RED, g_cwhat[idx], WTERMSIG(status), C_R);
            else
                printf("\n%s⚠ %s e' uscito (codice %d)%s\n",
                       C_RED, g_cwhat[idx], WEXITSTATUS(status), C_R);
            printf("  %slo riavvio fra 5 s — finche' manca, i chatter si "
                   "scaricano gli esperti invece di farli eseguire%s\n", C_DIM, C_R);
            fflush(stdout);
            sleep(5);
            pid_t np = spawn_argv(g_cargv[idx]);
            child_publish(idx, np);
            printf("  %s%s riavviato%s\n", C_DIM, g_cwhat[idx], C_R);
            fflush(stdout);
            continue;
        }
        if (idx >= 0) {
            int last = --g_nchildren;
            pid_t moved = g_children[last];
            if (idx != last) child_publish(idx, moved); /* duplicate is harmless */
            child_unpublish(last);
        }
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
        if (!seen && out < cap) {
            memcpy(names[out], rows[i].model, sizeof rows[i].model);
            names[out][sizeof rows[i].model - 1] = 0;
            out++;
        }
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
    volatile int    streaming;       /* a reply is being echoed token by token */
    volatile int    deferred;        /* net lines swallowed while streaming */
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
        /* While booting the spinner shows the latest line and overwrites it,
         * which is fine for progress and wrong for conclusions: "phase 2
         * active", the peers found and the signature verdict are exactly what
         * someone wants to re-read afterwards, and they used to scroll past
         * inside a spinner that erases itself. Those stay; the rest keeps
         * flowing through the spinner. */
        if (g_eng.booting) {
            snprintf(g_eng.phase, sizeof g_eng.phase, "%s", line);
            int keep = strstr(line, "phase 2 active") || strstr(line, "expert peers") ||
                       strstr(line, "running experts locally") ||
                       strstr(line, "no peer") || strstr(line, "unreachable") ||
                       (strstr(line, "peer ") && strstr(line, "rtt"));
            if (!g_tty) fprintf(stderr, "  %s%s%s\n", C_DIM, line, C_R);
            else if (keep) fprintf(stderr, "\r\x1b[2K  %s%s%s\n", C_DIM, line, C_R);
            continue;
        }
        if (strstr(line, "[lumabri]") || strstr(line, "resident weights") ||
            strstr(line, "[chat]") || strstr(line, "[USAGE]")) {
            /* Mid-reply these lines splice themselves into the streamed text
             * ("Page[lumabri] peer …") and a redraw can even eat the last
             * token off the line. They are already in the tail: keep the
             * reply clean, count them, and let /debug show them. */
            if (g_eng.streaming) { g_eng.deferred++; continue; }
            fprintf(stderr, "%s  %s%s\n", C_DIM, line, C_R);
        }
    }
    fclose(f);
    return NULL;
}

/* /debug: the last engine lines and each donor's log tail — everything that
 * used to shout over the streamed reply, on demand instead. */
static void tail_file(const char *path, int max) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("    %s(niente ancora)%s\n", C_DIM, C_R); return; }
    char ring[8][256];
    int n = 0;
    if (max > 8) max = 8;
    char l[256];
    while (fgets(l, sizeof l, f)) {
        size_t k = strlen(l);
        while (k && (l[k - 1] == '\n' || l[k - 1] == '\r')) l[--k] = 0;
        if (!k) continue;
        snprintf(ring[n % 8], sizeof ring[0], "%s", l);
        n++;
    }
    fclose(f);
    int show = n < max ? n : max;
    for (int i = n - show; i < n; i++)
        printf("    %s\xe2\x94\x82%s %s\n", C_GRAY, C_R, ring[i % 8]);
    if (!n) printf("    %s(niente ancora)%s\n", C_DIM, C_R);
}

static void render_debug(void) {
    printf("  %sultime righe del motore:%s\n", C_DIM, C_R);
    tail_dump(15);
    for (int i = 0; i < g_ndonor_logs; i++) {
        printf("  %s%s%s\n", C_DIM, g_donor_logs[i], C_R);
        tail_file(g_donor_logs[i], 8);
    }
    if (!g_ndonor_logs)
        printf("  %snessun donatore in questa sessione%s\n", C_DIM, C_R);
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

/* PROTO_SERVE2 is colibri's newer serve codec (every engine but GLM): the client
 * sends `SUBMIT <id> <slot> <bytes> <max> <temp> <top_p>\n<prompt>\n` and reads
 * back ACCEPT / DATA <id> <n> frames / DONE — not the raw-prompt-then-FRAME_END
 * dialect GLM speaks. Both announce readiness with FRAME_READY and a STAT line,
 * so they can't be told apart from the handshake; which one an engine speaks is
 * known by its kind (engine_kind_of), the way the olmoe line probe once was. */
typedef enum { PROTO_UNKNOWN = 0, PROTO_LINE, PROTO_FRAMED, PROTO_SERVE2 } Proto;
/* Which colibri engine we launched. The serve-codec engines (everyone but GLM)
 * hand their SUBMIT payload straight to the tokenizer — coli_v4_prompt_build()
 * and its siblings run only on the CLI path — so lumabri, standing in for the
 * gateway, must apply each engine's own chat template. GLM (EK_GLM) templates
 * inside its serve and speaks the older framed dialect, so it needs neither. */
typedef enum {
    EK_GLM = 0,        /* colibri monolith: framed, templates internally */
    EK_DEEPSEEK,       /* deepseek_v4:  <｜User｜>…<｜Assistant｜></think>       */
    EK_OLMOE,          /* olmoe:        |||IP_ADDRESS|||<|user|>…<|assistant|> */
    EK_QWEN36,         /* qwen36:       ChatML <|im_start|>…                   */
    EK_INKLING,        /* inkling:      <|message_user|><|content_text|>…      */
    EK_KIMI            /* kimi_k3:      K3CHAT1 byte-counted wire              */
} EngKind;
typedef struct { pid_t pid; int to, from; Proto proto; EngKind kind; } Engine;

/* Map the resolved engine binary name to its kind. Unknown ⇒ EK_GLM, the safe
 * default: the framed dialect with no client-side templating, which is exactly
 * how lumabri behaved before per-engine templates existed. */
static EngKind engine_kind_of(const char *engine) {
    if (strstr(engine, "deepseek")) return EK_DEEPSEEK;
    if (strstr(engine, "olmoe"))    return EK_OLMOE;
    if (strstr(engine, "qwen"))     return EK_QWEN36;
    if (strstr(engine, "inkling"))  return EK_INKLING;
    if (strstr(engine, "kimi"))     return EK_KIMI;
    return EK_GLM;
}
/* The serve-codec engines: framed READY at boot, but SUBMIT/DATA/DONE turns and
 * a raw (un-templated) payload. Everything but GLM. */
static int kind_is_serve2(EngKind k) { return k != EK_GLM; }

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

/* Some engines (DeepSeek V4) print a dashboard on stdout between the reply text:
 * whole lines like `EMAP 43 256 <hex>`, `TIERS 0 …`, `HITS …`, `HWINFO …`,
 * `PROF …`. They are diagnostics, not generated tokens, and they used to land
 * raw in the chat. Drop any line that starts with one of those keywords
 * followed by a space and a digit (the real dashboard shape — so a sentence
 * that merely begins "PROF ..." is left alone), and stream everything else live.
 * State persists across reads; call le_flush_tel() at the end to emit a trailing
 * partial line that turned out to be ordinary text. */
typedef struct { int mode; char pfx[8]; int pfxn; } TelFilter;
enum { TF_LINESTART = 0, TF_MID, TF_DROP };

static void emit_no_tel(const char *p, size_t n, TelFilter *t) {
    static const char *const kw[] = { "TIERS ", "EMAP ", "HITS ", "HWINFO ", "PROF " };
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (t->mode == TF_DROP) { if (c == '\n') t->mode = TF_LINESTART; continue; }
        if (t->mode == TF_MID) { putchar(c); if (c == '\n') t->mode = TF_LINESTART; continue; }
        /* TF_LINESTART: buffer until we can classify the line */
        if (c == '\n') {                       /* short line, can't be a dashboard row */
            fwrite(t->pfx, 1, (size_t)t->pfxn, stdout); putchar('\n');
            t->pfxn = 0; continue;
        }
        if (t->pfxn < (int)sizeof t->pfx) t->pfx[t->pfxn++] = c;
        int is_tel = 0, maybe = 0;
        for (size_t k = 0; k < sizeof kw / sizeof *kw; k++) {
            size_t kl = strlen(kw[k]);
            if ((size_t)t->pfxn >= kl + 1) {
                if (!memcmp(t->pfx, kw[k], kl) && t->pfx[kl] >= '0' && t->pfx[kl] <= '9')
                    { is_tel = 1; break; }
            } else if (!memcmp(t->pfx, kw[k], (size_t)t->pfxn)) {
                maybe = 1;                     /* still a possible prefix */
            }
        }
        if (is_tel) { t->pfxn = 0; t->mode = TF_DROP; }
        else if (!maybe || t->pfxn == (int)sizeof t->pfx) {   /* ruled out: it is text */
            fwrite(t->pfx, 1, (size_t)t->pfxn, stdout);
            t->pfxn = 0; t->mode = TF_MID;
        }
        /* else: keep buffering (a keyword prefix so far) */
    }
    fflush(stdout);
}

static void le_flush_tel(TelFilter *t) {       /* trailing buffered text at stream end */
    if (t->mode == TF_LINESTART && t->pfxn > 0) {
        fwrite(t->pfx, 1, (size_t)t->pfxn, stdout);
        t->pfxn = 0;
    }
    fflush(stdout);
}

static int stream_until_end(Engine *e, char *statline, size_t scap) {
    const char *S = FRAME_END;
    size_t SL = strlen(S), cap = 8192, len = 0, shown = 0;
    char *buf = malloc(cap), *hit = NULL;
    TelFilter tf = {0};
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
            emit_no_tel(buf + shown, safe - shown, &tf);
            shown = safe;
        }
        if (hit) break;
    }
    le_flush_tel(&tf);
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

/* ---- serve codec (PROTO_SERVE2) client — DeepSeek V4 --------------------- */
typedef struct { int fd; unsigned char b[16384]; size_t off, len; } SReader;

static ssize_t sr_fill(SReader *s) {
    if (s->off) { memmove(s->b, s->b + s->off, s->len - s->off); s->len -= s->off; s->off = 0; }
    if (s->len >= sizeof s->b) return -1;              /* a header longer than the buffer */
    ssize_t r = read(s->fd, s->b + s->len, sizeof s->b - s->len);
    if (r > 0) s->len += (size_t)r;
    return r;
}
/* One '\n'-terminated line. Only the first cap-1 bytes are kept in `out` (enough
 * to read the DATA/DONE/… keyword), but the WHOLE line is consumed — DeepSeek's
 * EMAP row is tens of KB of hex, far past any header, and must be swallowed, not
 * overflow the reader. -1 on EOF with nothing buffered. */
static int sr_line(SReader *s, char *out, size_t cap) {
    size_t got = 0;
    for (;;) {
        unsigned char *start = s->b + s->off, *nl = memchr(start, '\n', s->len - s->off);
        size_t avail = nl ? (size_t)(nl - start) : s->len - s->off;
        if (got < cap - 1) {
            size_t room = cap - 1 - got, take = avail < room ? avail : room;
            memcpy(out + got, start, take); got += take;
        }
        s->off += avail + (nl ? 1 : 0);
        if (nl) { out[got] = 0; return (int)got; }
        if (sr_fill(s) <= 0) { out[got] = 0; return got ? (int)got : -1; }
    }
}
/* A growable capture of the assistant's reply, to feed back as history. */
typedef struct { char *p; size_t len, cap; } Cap;
static int cap_add(Cap *c, const char *d, size_t n) {
    if (c->len + n + 1 > c->cap) {
        size_t nc = c->cap ? c->cap : 4096;
        while (nc < c->len + n + 1) nc *= 2;
        char *np = realloc(c->p, nc);
        if (!np) return -1;
        c->p = np; c->cap = nc;
    }
    memcpy(c->p + c->len, d, n); c->len += n; c->p[c->len] = 0;
    return 0;
}
static int cap_str(Cap *c, const char *s) { return cap_add(c, s, strlen(s)); }
/* Append a printf-formatted fragment (used for kimi's `M <role> <len>\n` heads). */
static int cap_addf(Cap *c, const char *fmt, ...) {
    char tmp[64];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    return cap_add(c, tmp, (size_t)n);
}

static int sr_take(SReader *s, size_t n, int emit, Cap *cap) {   /* copy/discard n bytes */
    while (n) {
        if (s->off >= s->len && sr_fill(s) <= 0) return -1;
        size_t avail = s->len - s->off, take = avail < n ? avail : n;
        if (emit) fwrite(s->b + s->off, 1, take, stdout);
        if (cap && cap_add(cap, (char *)s->b + s->off, take)) return -1;
        s->off += take; n -= take;
    }
    if (emit) fflush(stdout);
    return 0;
}

/* Read a serve-codec reply: DATA frames (the generated text) until DONE. Bare
 * dashboard lines (EMAP/TIERS/…) and ACCEPT are skipped. On success returns 0
 * with the STAT tail in statline and, if captured != NULL, the assistant text
 * malloc'd into *captured (for the conversation history). -1 if the engine died. */
static int stream_serve2(Engine *e, char *statline, size_t scap, char **captured) {
    SReader s = { e->from, {0}, 0, 0 };
    char line[600];
    Cap cap = {0};
    if (statline && scap) statline[0] = 0;
    if (captured) *captured = NULL;
    for (;;) {
        if (sr_line(&s, line, sizeof line) < 0) { free(cap.p); return -1; }
        if (!strncmp(line, "DATA ", 5)) {
            char *sp = strchr(line + 5, ' ');            /* DATA <id> <bytes> */
            size_t n = sp ? strtoull(sp + 1, NULL, 10) : 0;
            if (sr_take(&s, n, 1, captured ? &cap : NULL) < 0) { free(cap.p); return -1; }
            if (sr_take(&s, 1, 0, NULL) < 0) { free(cap.p); return -1; } /* frame '\n' */
        } else if (!strncmp(line, "DONE ", 5)) {
            char *st = strstr(line, "STAT ");
            if (st && statline && scap) snprintf(statline, scap, "%s", st);
            if (captured) *captured = cap.p; else free(cap.p);
            return 0;
        } else if (!strncmp(line, "ERROR ", 6)) {
            char *msg = strchr(line + 6, ' ');            /* skip the id */
            printf("%s%s%s", C_RED, msg ? msg + 1 : line + 6, C_R);
            free(cap.p);
            return 0;
        }
        /* ACCEPT / EMAP / TIERS / HITS / HWINFO / PROF / other: skip */
    }
}

/* The serve-codec engines tokenize the SUBMIT payload as-is: coli_v4_prompt_build
 * and its per-engine siblings run only on the CLI path, so lumabri — standing in
 * for openai_server.py — applies each engine's own chat template. The whole
 * conversation is resent every turn (prefix + prior user/assistant turns + this
 * user turn ending at the assistant-generation marker); the serve reuses the KV
 * prefix, so it stays a real multi-turn chat without reprocessing the history.
 *
 * Each marker set mirrors its engine's serve/CLI source. Only DeepSeek and olmoe
 * are exercised against a real model here; qwen36/inkling/kimi are transcribed
 * from colibri and unverified — see the PR notes. Keep them in step with colibri;
 * the ideal home is each engine's serve, as GLM already does.
 *
 * DeepSeek: ｜ is U+FF5C, ▁ is U+2581 — each its own literal so the next letter
 * is not eaten by the \x escape. </think> is the non-thinking "answer now" form. */
#define DS_PIPE "\xef\xbd\x9c"
#define DS_USCR "\xe2\x96\x81"
#define DS_BOS  "<" DS_PIPE "begin" DS_USCR "of" DS_USCR "sentence" DS_PIPE ">"
#define DS_USER "<" DS_PIPE "User" DS_PIPE ">"
#define DS_ASST "<" DS_PIPE "Assistant" DS_PIPE "></think>"
/* olmoe (olmoe.c fmt_user_turn): bos/eos is |||IP_ADDRESS|||; the first turn glues
 * <|user|> to it, later turns put a newline between. The reply is stored raw — the
 * next turn's leading |||IP_ADDRESS||| is the eos that closes it. */
#define OLMO_U0   "|||IP_ADDRESS|||<|user|>\n"
#define OLMO_UL   "|||IP_ADDRESS|||\n<|user|>\n"
#define OLMO_ASST "\n<|assistant|>\n"
/* qwen36 (qwen36.c: ChatML, no BOS). */
#define QW_U    "<|im_start|>user\n"
#define QW_ASST "<|im_end|>\n<|im_start|>assistant\n"
#define QW_AEND "<|im_end|>\n"
/* inkling (inkling.c chat template, thinking-effort system line dropped). */
#define INK_U    "<|message_user|><|content_text|>"
#define INK_ASST "<|end_message|><|message_model|>"
#define INK_AEND "<|end_message|>"

/* Append one turn for engine kind k. `first` = the conversation is empty so far
 * (matters only where the leading marker differs by position). reply==NULL builds
 * the *pending* turn — user text ending at the assistant-generation marker, for
 * the SUBMIT payload; reply!=NULL builds a *completed* turn — user text plus the
 * assistant's reply, for the running history. 0, or -1 on OOM. */
static int serve2_turn(Cap *c, EngKind k, int first, const char *u, const char *reply) {
    switch (k) {
    case EK_DEEPSEEK:
        if (cap_str(c, DS_USER) || cap_str(c, u) || cap_str(c, DS_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_OLMOE:
        if (cap_str(c, first ? OLMO_U0 : OLMO_UL) || cap_str(c, u) ||
            cap_str(c, OLMO_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_QWEN36:
        if (cap_str(c, QW_U) || cap_str(c, u) || cap_str(c, QW_ASST)) return -1;
        return reply ? (cap_str(c, reply) || cap_str(c, QW_AEND)) : 0;
    case EK_INKLING:
        if (cap_str(c, INK_U) || cap_str(c, u) || cap_str(c, INK_ASST)) return -1;
        return reply ? (cap_str(c, reply) || cap_str(c, INK_AEND)) : 0;
    case EK_KIMI:
        /* K3CHAT1 wire (kimi_k3.c chat_build_wire): `M <role> <bytes>\n<text>`,
         * byte-counted so the text may hold newlines. The engine appends the
         * assistant-generation open after the wire, so a pending user turn needs
         * no trailing marker. */
        if (cap_addf(c, "M user %zu\n", strlen(u)) || cap_str(c, u)) return -1;
        return reply ? (cap_addf(c, "M assistant %zu\n", strlen(reply)) ||
                        cap_str(c, reply)) : 0;
    default:                                    /* EK_GLM never speaks serve-codec */
        return -1;
    }
}

/* The once-at-front SUBMIT prefix (kept out of the stored history). */
static int serve2_prefix(Cap *c, EngKind k) {
    if (k == EK_DEEPSEEK) return cap_str(c, DS_BOS);
    if (k == EK_KIMI)     return cap_str(c, "K3CHAT1\n");
    return 0;
}

/* SUBMIT: prefix + history + this pending user turn. 0, or -1 on error. */
static int submit_serve2(Engine *e, const char *history, const char *prompt, int max_new) {
    static unsigned id = 0;
    Cap c = {0};
    if (serve2_prefix(&c, e->kind) || cap_str(&c, history) ||
        serve2_turn(&c, e->kind, history[0] == 0, prompt, NULL)) { free(c.p); return -1; }
    char hdr[128];
    int hn = snprintf(hdr, sizeof hdr, "SUBMIT %u 0 %zu %d 0.7 0.95\n",
                      ++id, c.len, max_new < 1 ? 1 : max_new);
    int ok = hn >= 0 && write(e->to, hdr, (size_t)hn) >= 0 &&
             write(e->to, c.p, c.len) >= 0 &&
             write(e->to, "\n", 1) >= 0;                  /* payload terminator */
    free(c.p);
    return ok ? 0 : -1;
}

/* Append this finished turn to the running conversation. Returns the new
 * history (frees the old); NULL on OOM, leaving the old freed. */
static char *serve2_history_append(Engine *e, char *history, const char *user,
                                    const char *reply) {
    Cap c = {0};
    if (cap_str(&c, history) ||
        serve2_turn(&c, e->kind, history[0] == 0, user, reply ? reply : "")) {
        free(c.p); free(history); return NULL;
    }
    free(history);
    return c.p ? c.p : calloc(1, 1);
}

static const char *engine_for(const char *model_type) {
    if (strstr(model_type, "olmoe")) return "olmoe";
    if (strstr(model_type, "deepseek")) return "deepseek";
    if (strstr(model_type, "kimi")) return "kimi_k3";
    if (strstr(model_type, "inkling")) return "inkling";
    if (strstr(model_type, "qwen")) return "qwen36";
    return "colibri";
}

/* `local_dir` non-NULL: the model is already on this disk, so no shim, no
 * mirror, no second copy. That is the right mode on the machine that serves
 * the model — otherwise chatting there downloads it from itself. */
static int engine_spawn(const char *engine, const char *shim, const char *tracker,
                        const char *model, const char *local_dir,
                        int ctx, int max_new, int cap_experts, Engine *e) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char vroot[1024], cache[1024], cas[1024];
    const char *vroot_env = getenv("LUMABRI_VROOT");
    const char *cache_env = getenv("LUMABRI_CACHE");
    if (vroot_env && vroot_env[0]) snprintf(vroot, sizeof vroot, "%s", vroot_env);
    else snprintf(vroot, sizeof vroot, "%s/.lumabri/%s/vroot", home, model);
    if (cache_env && cache_env[0]) snprintf(cache, sizeof cache, "%s", cache_env);
    else snprintf(cache, sizeof cache, "%s/.lumabri/%s/cache", home, model);
    snprintf(cas, sizeof cas, "%s/.lumabri/cas", home);
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
            setenv("LUMABRI_CAS", cas, 0);       /* shared across model mirrors */
            setenv("LUMABRI_TRACKER", tracker, 1);
            setenv("LUMABRI_MODEL", model, 1);
            setenv("LUMABRI_STATS", "2", 1);       /* boot progress, not a log */
            setenv("SNAP", vroot, 1);
        }
        /* Pinning is for an engine that owns its experts. A SWARM chatter never
         * does: either they run on peers (pinning is pointless) or they come
         * over the network (pinning is catastrophic — colibri's AUTOPIN reads
         * a shipped .coli_usage and preloads GBs of them before the first
         * token). But a --local run owns its experts on disk, and there pinning
         * the hot ones in RAM is the whole difference between decode from RAM
         * and streaming every expert off disk each token (measured: GLM at
         * TIERS 0 resident → ~0.1 tok/s). So only force it off for the swarm;
         * a local run keeps colibri's own AUTOPIN. overwrite=0 either way, so
         * an explicit PIN still wins. */
        if (!local_dir) setenv("PIN", "0", 0);
        /* Same split for the expert cache. A swarm chatter caches almost nothing
         * (experts run on peers), so its cap stays at the small default. But a
         * --local run holds its own experts, and there the cap IS the resident
         * set: cap_experts is a swarm-shaped default (64), and colibri only
         * auto-grows the cache to fit RAM when CAP_RAISE is on — which it turns
         * OFF by default on a fast SSD. Left alone, a --local GLM cached 64 of
         * 19456 experts and streamed the rest (TIERS 0…, ~0.1 tok/s) with the
         * box's RAM sitting idle. Turn the RAM auto-raise on for local runs so
         * the cache grows to whatever RAM_GB (or the auto 88%) allows; overwrite=0
         * keeps an explicit CAP_RAISE=… authoritative. */
        if (local_dir) setenv("CAP_RAISE", "1", 0);
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
    e->kind = engine_kind_of(engine);
    pthread_t t;
    pthread_create(&t, NULL, stderr_thread, (void *)(intptr_t)err_pipe[0]);
    pthread_detach(t);
    return 0;
}

/* Why the child is gone, in the words of the kernel and of the child. */
static void engine_diag(Engine *e, int booting) {
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
    } else if (booting)
        printf("  %sil motore ha chiuso il suo stdout senza dire di essere pronto%s\n",
               C_RED, C_R);
    else
        printf("  %sil motore si e' fermato durante la risposta%s\n"
               "  %sla causa piu' probabile e' un peer sparito: con un solo "
               "detentore per esperto non c'e' dove ripiegare%s\n",
               C_RED, C_R, C_DIM, C_R);
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
    if (engine_path) {
        if (checked_printf(out, cap, "%s", engine_path)) return -1;
        return access(out, X_OK);
    }
    const char *eng = engine_for(model_type);
    const char *dir = engines_dir ? engines_dir : "../moe-stream/c";
    /* prefer the P2P build when one exists: it is the same engine (identical
     * without expert peers) plus the ability to run the routed experts on
     * the swarm when the tracker offers executors */
    char me[1200];
    exe_dir(me, sizeof me);
    if (checked_printf(out, cap, "%s/%s_p2p", dir, eng)) return -1;
    if (access(out, X_OK) == 0) return 0;
    if (checked_printf(out, cap, "%s/%s_p2p", me, eng)) return -1;
    if (access(out, X_OK) == 0) return 0;
    if (checked_printf(out, cap, "%s/%s", dir, eng)) return -1;
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
    printf("  %smirror di %s in %s: %.0f GB liberi. Tiene solo i blocchi che tocchi "
           "— la parte densa sempre, gli esperti solo se nessun peer li esegue "
           "(al limite %.0f GB)%s\n",
           C_DIM, model, cache, free_gb, need_gb, C_R);
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

/* ---- the settings a TUI user must never be asked twice -------------------
 *
 * Everything the chat needs used to live on the command line: the tracker,
 * the engines directory, the operator key. Someone who only ever opens the
 * TUI would have to be told all three by whoever runs the swarm, and would
 * have to retype them every time — and the failure when they get one wrong
 * is not "invalid argument", it is a 299 GB download or a silently
 * unverified model. So they are asked once, in the panel, and remembered.
 *
 * ~/.lumabri/config, one key=value per line. Flags still win when given:
 * a script is not a person and should not inherit somebody's saved answers. */
typedef struct { char tracker[80], pubkey[LMB_PATH_MAX], engines[1024]; } Cfg;

static void cfg_path(char *dst, size_t cap) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    snprintf(dst, cap, "%s/.lumabri/config", home);
}

static void cfg_load(Cfg *c) {
    memset(c, 0, sizeof *c);
    char p[1100];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1, *nl = strchr(v, '\n');
        if (nl) *nl = 0;
        if (!strcmp(line, "tracker"))
            (void)checked_printf(c->tracker, sizeof c->tracker, "%s", v);
        else if (!strcmp(line, "pubkey"))
            (void)checked_printf(c->pubkey, sizeof c->pubkey, "%s", v);
        else if (!strcmp(line, "engines"))
            (void)checked_printf(c->engines, sizeof c->engines, "%s", v);
    }
    fclose(f);
}

static void cfg_save(const Cfg *c) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char d[1100], p[1200];
    snprintf(d, sizeof d, "%s/.lumabri", home);
    mkdir_p(d);
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    if (c->tracker[0]) fprintf(f, "tracker=%s\n", c->tracker);
    if (c->pubkey[0])  fprintf(f, "pubkey=%s\n", c->pubkey);
    if (c->engines[0]) fprintf(f, "engines=%s\n", c->engines);
    fclose(f);
}

/* Where the engines live. Nobody should have to know: look where `make` and
 * `make install` put them, and where a colibri checkout usually sits. */
static int find_engines(char *dst, size_t cap) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char me[1100];
    exe_dir(me, sizeof me);
    const char *names[] = { "colibri_p2p", "olmoe_p2p", "colibri", "olmoe" };
    char cand[8][1100];
    int n = 0;
    if (!checked_printf(cand[n], sizeof cand[n], "%s", me)) n++;
    if (!checked_printf(cand[n], sizeof cand[n], ".")) n++;
    if (!checked_printf(cand[n], sizeof cand[n], "%s/colibri/c", home)) n++;
    if (!checked_printf(cand[n], sizeof cand[n], "../moe-stream/c")) n++;
    if (!checked_printf(cand[n], sizeof cand[n], "../colibri/c")) n++;
    if (!checked_printf(cand[n], sizeof cand[n], "/usr/local/bin")) n++;
    for (int i = 0; i < n; i++)
        for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
            char probe[1300];
            if (checked_printf(probe, sizeof probe, "%s/%s", cand[i], names[k]))
                continue;
            if (access(probe, X_OK) == 0)
                return checked_printf(dst, cap, "%s", cand[i]);
        }
    return -1;
}

/* ---- joining: what do you bring? ----------------------------------------
 *
 * A chatter is a taker. Most people would give something back if it took one
 * keypress, and almost nobody will read a manual to find the flag for it —
 * so the choice is made here, on the way in, with Enter meaning "just chat"
 * so the impatient path stays one key.
 *
 * Whatever is chosen starts as a child of the chat process and dies with it,
 * which is the honest shape for a donation made from a terminal someone has
 * open: it lasts as long as they are around. A donor that should outlive the
 * session is `lumabri serve --join`, and the picker says so.
 *
 * The server side needs no changes at all. A disk donor is a maintainer with
 * a byte budget, and the tracker already assigns it the rarest files first;
 * a compute donor is an expert node, and chatters already discover it by
 * heartbeat. The role is entirely a client-side decision.
 */
typedef struct {
    int disk, compute;
    double gb;
    char model_dir[1024];
    char donor_name[48];        /* --donor-name; "" = auto from the hostname */
} Role;

/* Donor names used to be "donor-exec-<port>" — the same string on every
 * machine that donates on the default port. The tracker binds a name to the
 * first peer key that registers it (anti-takeover), so the second machine on
 * Earth to donate was silently rejected forever. Put the hostname in the
 * automatic name so machines stop colliding; --donor-name overrides it. */
static void donor_base_name(const Role *r, char *out, size_t cap) {
    if (r->donor_name[0]) { snprintf(out, cap, "%s", r->donor_name); return; }
    char host[64] = "";
    if (gethostname(host, sizeof host - 1)) host[0] = 0;
    char clean[24];
    size_t n = 0;
    for (const char *p = host; *p && n < sizeof clean - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
            clean[n++] = c;
    }
    clean[n] = 0;
    if (n) snprintf(out, cap, "donor-%s", clean);
    else   snprintf(out, cap, "donor");
}

/* free space where the donated slice would live, in GB */
static double free_gb_at(const char *path) {
    struct statvfs v;
    if (statvfs(path, &v)) return 0;
    return (double)v.f_bavail * (double)v.f_frsize / 1e9;
}

/* --- a small line editor -------------------------------------------------
 * fgets leaves the terminal in canonical mode, where the arrow keys are not
 * handled and arrive as raw escape bytes (^[[D) that land in the text. This
 * gives the prompt the editing people expect — left/right, Home/End,
 * backspace/Delete, word/line kill, and up/down history — by reading in raw
 * mode and repainting only the input, so the caller's prompt (a drawn box) is
 * left untouched. Non-interactive input (a pipe, a test) still uses fgets.
 * Cursor moves are relative and per-character (UTF-8 aware), so it assumes the
 * input does not wrap past the terminal width — fine for a chat line. */
#define LE_HIST 64
static char *le_hist[LE_HIST];
static int le_hist_n = 0;

static void le_hist_push(const char *s) {
    if (!s || !*s) return;
    char *last = le_hist_n ? le_hist[(le_hist_n - 1) % LE_HIST] : NULL;
    if (last && !strcmp(last, s)) return;      /* no consecutive duplicate */
    char *d = strdup(s);
    if (!d) return;
    free(le_hist[le_hist_n % LE_HIST]);
    le_hist[le_hist_n % LE_HIST] = d;
    le_hist_n++;
}

static int le_lead(unsigned char c) { return (c & 0xC0) != 0x80; }
static int le_cols(const char *s, int a, int b) {   /* characters in [a,b) */
    int n = 0;
    for (int i = a; i < b; i++)
        if (le_lead((unsigned char)s[i])) n++;
    return n;
}
static int le_prev(const char *s, int pos) {        /* start of char before pos */
    int i = pos - 1;
    while (i > 0 && !le_lead((unsigned char)s[i])) i--;
    return i < 0 ? 0 : i;
}
static int le_next(const char *s, int len, int pos) {
    int i = pos + 1;
    while (i < len && !le_lead((unsigned char)s[i])) i++;
    return i > len ? len : i;
}
static void le_left(int n) { if (n > 0) printf("\x1b[%dD", n); }

/* Replace the visible input with `text` (history recall / line kill). */
static void le_set(char *buf, size_t cap, int *len, int *pos, const char *text) {
    le_left(le_cols(buf, 0, *pos));            /* to input start */
    printf("\x1b[K");                          /* clear to end of line */
    snprintf(buf, cap, "%s", text ? text : "");
    *len = (int)strlen(buf);
    *pos = *len;
    fwrite(buf, 1, (size_t)*len, stdout);
}

static int line_edit(char *buf, size_t cap) {
    struct termios old, raw;
    if (tcgetattr(0, &old)) return -2;         /* not a real tty -> caller fgets */
    raw = old;
    /* Clear ISIG/IEXTEN too, and IXON, so Ctrl-C / Ctrl-Z / Ctrl-S reach read()
     * as bytes instead of the tty acting on them behind our back — otherwise the
     * Ctrl-C branch below is dead and Ctrl-Z suspends with the terminal still in
     * raw mode, leaving a garbled shell. */
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw)) return -2;

    int len = 0, pos = 0, rc = 0;
    int hidx = le_hist_n;                       /* == "the line being typed" */
    char *save = (char *)malloc(cap);           /* in-progress line, for down */
    if (!save) { tcsetattr(0, TCSANOW, &old); return -2; }   /* -> caller fgets */
    save[0] = 0;
    buf[0] = 0;

    for (;;) {
        unsigned char c;
        ssize_t rn = read(0, &c, 1);
        if (rn < 0 && errno == EINTR) continue;  /* a signal, not end of input */
        if (rn <= 0) { rc = -1; break; }

        if (c == '\r' || c == '\n') { printf("\r\n"); break; }
        if (c == 3) {                            /* Ctrl-C: cancel, keep old semantics */
            tcsetattr(0, TCSANOW, &old);
            printf("\r\n");
            raise(SIGINT);
            rc = -1; len = 0; break;
        }
        if (c == 26) {                           /* Ctrl-Z: suspend, terminal restored */
            tcsetattr(0, TCSANOW, &old);
            raise(SIGTSTP);
            tcsetattr(0, TCSANOW, &raw);         /* resumed: back to raw */
            continue;
        }
        if (c == 4) {                            /* Ctrl-D: EOF on empty, else Delete */
            if (len == 0) { rc = -1; break; }
            if (pos < len) {
                int nx = le_next(buf, len, pos);
                memmove(buf + pos, buf + nx, (size_t)(len - nx));
                len -= nx - pos;
                fwrite(buf + pos, 1, (size_t)(len - pos), stdout);
                printf(" ");
                le_left(le_cols(buf, pos, len) + 1);
            }
        } else if (c == 127 || c == 8) {         /* Backspace */
            if (pos > 0) {
                int p = le_prev(buf, pos);
                memmove(buf + p, buf + pos, (size_t)(len - pos));
                len -= pos - p;
                pos = p;
                le_left(1);
                fwrite(buf + pos, 1, (size_t)(len - pos), stdout);
                printf(" ");
                le_left(le_cols(buf, pos, len) + 1);
            }
        } else if (c == 1) {                     /* Ctrl-A: Home */
            le_left(le_cols(buf, 0, pos)); pos = 0;
        } else if (c == 5) {                     /* Ctrl-E: End */
            fwrite(buf + pos, 1, (size_t)(len - pos), stdout); pos = len;
        } else if (c == 21) {                    /* Ctrl-U: clear line */
            le_set(buf, cap, &len, &pos, "");
        } else if (c == 11) {                    /* Ctrl-K: kill to end */
            printf("\x1b[K"); len = pos; buf[len] = 0;
        } else if (c == 23) {                    /* Ctrl-W: delete previous word */
            int p = pos;
            while (p > 0 && buf[p-1] == ' ') p--;
            while (p > 0 && buf[p-1] != ' ') p--;
            if (p < pos) {
                int killed = le_cols(buf, p, pos);   /* columns removed — count BEFORE the shift */
                le_left(killed);
                memmove(buf + p, buf + pos, (size_t)(len - pos));
                len -= pos - p; pos = p;
                fwrite(buf + pos, 1, (size_t)(len - pos), stdout);
                for (int k = 0; k < killed; k++) printf(" ");
                le_left(le_cols(buf, pos, len) + killed);
            }
        } else if (c == 27) {                    /* an escape sequence */
            unsigned char a, b;
            if (read(0, &a, 1) <= 0) continue;
            if (a != '[' && a != 'O') continue;
            if (read(0, &b, 1) <= 0) continue;
            if (b == 'D') {                      /* Left */
                if (pos > 0) { pos = le_prev(buf, pos); le_left(1); }
            } else if (b == 'C') {               /* Right */
                if (pos < len) { int nx = le_next(buf, len, pos);
                    fwrite(buf + pos, 1, (size_t)(nx - pos), stdout); pos = nx; }
            } else if (b == 'H') {               /* Home */
                le_left(le_cols(buf, 0, pos)); pos = 0;
            } else if (b == 'F') {               /* End */
                fwrite(buf + pos, 1, (size_t)(len - pos), stdout); pos = len;
            } else if (b == 'A' || b == 'B') {   /* Up / Down: history */
                int avail = le_hist_n < LE_HIST ? le_hist_n : LE_HIST;
                int oldest = le_hist_n - avail;
                if (b == 'A' && hidx > oldest) {
                    if (hidx == le_hist_n) snprintf(save, cap, "%s", buf);
                    hidx--;
                    le_set(buf, cap, &len, &pos, le_hist[hidx % LE_HIST]);
                } else if (b == 'B' && hidx < le_hist_n) {
                    hidx++;
                    le_set(buf, cap, &len, &pos,
                           hidx == le_hist_n ? save : le_hist[hidx % LE_HIST]);
                }
            } else if (b >= '0' && b <= '9') {   /* extended: read to the final '~' */
                unsigned char t = b, last = b;
                while (read(0, &t, 1) == 1 && t != '~') last = t;
                (void)last;
                if (b == '3' && pos < len) {     /* Delete */
                    int nx = le_next(buf, len, pos);
                    memmove(buf + pos, buf + nx, (size_t)(len - nx));
                    len -= nx - pos;
                    fwrite(buf + pos, 1, (size_t)(len - pos), stdout);
                    printf(" ");
                    le_left(le_cols(buf, pos, len) + 1);
                } else if (b == '1' || b == '7') {         /* Home */
                    le_left(le_cols(buf, 0, pos)); pos = 0;
                } else if (b == '4' || b == '8') {         /* End */
                    fwrite(buf + pos, 1, (size_t)(len - pos), stdout); pos = len;
                }
            }
        } else if (c >= 0x20) {                  /* a printable char (maybe UTF-8) */
            unsigned char cb[4]; int nb = 1;
            cb[0] = c;
            if (c >= 0xC0) {
                nb = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                for (int k = 1; k < nb; k++)
                    if (read(0, &cb[k], 1) <= 0) { nb = k; break; }
            }
            if (len + nb < (int)cap - 1) {
                memmove(buf + pos + nb, buf + pos, (size_t)(len - pos));
                memcpy(buf + pos, cb, (size_t)nb);
                len += nb;
                fwrite(buf + pos, 1, (size_t)(len - pos), stdout);
                le_left(le_cols(buf, pos + nb, len));
                pos += nb;
            }
        }
        buf[len] = 0;
        fflush(stdout);
    }

    buf[len] = 0;
    tcsetattr(0, TCSANOW, &old);
    if (rc == 0) le_hist_push(buf);
    free(save);
    return rc;
}

static int prompt_line(char *buf, size_t cap) {
    if (g_tty && isatty(0)) {
        int r = line_edit(buf, cap);
        if (r != -2) return r;                   /* -2 = no tty, fall through */
    }
    if (!fgets(buf, (int)cap, stdin)) return -1;
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    return 0;
}

/* Ask for what is missing, once, and remember it. Enter keeps the saved
 * value, so the second time this is three keypresses of nothing. */
static void setup_panel(Cfg *c) {
    char line[1200];
    printf("  %sa quale sciame ti colleghi?%s\n", C_BOLD, C_R);
    if (c->tracker[0])
        printf("  %sinvio = %s%s\n", C_DIM, c->tracker, C_R);
    else
        printf("  %sindirizzo del server, es. 148.251.4.122 (invio = questo "
               "computer)%s\n", C_DIM, C_R);
    printf("\n%s\xe2\x94\x82%s %s%s\xe2\x80\xba%s ", C_GRAY, C_R, C_CORAL, C_BOLD, C_R);
    fflush(stdout);
    if (!prompt_line(line, sizeof line) && line[0]) {
        /* a bare host means the default port: nobody should have to know it */
        if (tracker_addr_set(c->tracker, sizeof c->tracker, line))
            printf("  %sindirizzo troppo lungo, uso quello salvato%s\n", C_RED, C_R);
    } else if (!c->tracker[0])
        snprintf(c->tracker, sizeof c->tracker, "127.0.0.1:7300");

    if (!c->pubkey[0]) {
        printf("\n  %schiave pubblica dello sciame%s %s(64 caratteri, te la da "
               "chi lo gestisce)%s\n", C_BOLD, C_R, C_DIM, C_R);
        printf("  %scon la chiave ogni byte del modello viene verificato; "
               "invio per saltare e fidarti del server%s\n", C_DIM, C_R);
        printf("\n%s\xe2\x94\x82%s %s%s\xe2\x80\xba%s ", C_GRAY, C_R, C_CORAL, C_BOLD, C_R);
        fflush(stdout);
        if (!prompt_line(line, sizeof line) && strlen(line) == 64)
            snprintf(c->pubkey, sizeof c->pubkey, "%s", line);
        else if (line[0])
            printf("  %snon sono 64 caratteri esadecimali — proseguo senza "
                   "verifica%s\n", C_DIM, C_R);
    }
    cfg_save(c);
    printf("\n  %sricordato in ~/.lumabri/config%s\n\n", C_DIM, C_R);
}

/* Returns 0 when the user chose, -1 when they quit. `have_model_dir` is
 * whether a full local copy of the model exists — without one, executing
 * experts is not on offer, because an expert node reads the weights from
 * disk and there would be none to read. */
/* --role takes whole words: chat, disk, compute, all, or a combination like
 * "disk,compute". Matching by letter was shorter and wrong — strchr("chat",
 * 'c') is true, so the one role that donates nothing was the one that
 * switched compute donation on. */
static int role_has(const char *arg, const char *want) {
    size_t wl = strlen(want);
    for (const char *p = arg; *p; ) {
        size_t n = strcspn(p, ",+ ");
        if (n == wl && !strncasecmp(p, want, wl)) return 1;
        p += n; if (*p) p++;
    }
    return 0;
}

/* Every token has to be a role, not just one of them: accepting
 * "chat,bogus" because "chat" is in there drops the half the user probably
 * cared about and says nothing. Returns the first word it does not know. */
static int role_unknown(const char *arg, char *out, size_t cap) {
    static const char *ok[] = { "chat", "disk", "compute", "all" };
    int any = 0;
    for (const char *p = arg; *p; ) {
        size_t n = strcspn(p, ",+ ");
        if (n) {
            any = 1;
            int good = 0;
            for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
                if (n == strlen(ok[i]) && !strncasecmp(p, ok[i], n)) good = 1;
            if (!good) { snprintf(out, cap, "%.*s", (int)n, p); return 1; }
        }
        p += n; if (*p) p++;
    }
    if (!any) { snprintf(out, cap, "%s", "(vuoto)"); return 1; }
    return 0;
}

static int role_pick(Role *r, const char *model, int have_model_dir) {
    printf("  %scome entri nello sciame?%s\n\n", C_BOLD, C_R);
    printf("    %s1%s  solo chattare        %snon condividi niente%s\n",
           C_CORAL, C_R, C_DIM, C_R);
    printf("    %s2%s  chatti e doni disco  %stieni un pezzo di %s per lo sciame%s\n",
           C_CORAL, C_R, C_DIM, model, C_R);
    if (have_model_dir)
        printf("    %s3%s  chatti e doni calcolo %sesegui esperti per gli altri%s\n",
               C_CORAL, C_R, C_DIM, C_R);
    else
        printf("    %s3%s  chatti e doni calcolo %sla tua fetta di esperti "
               "arriva dallo sciame%s\n", C_CORAL, C_R, C_DIM, C_R);
    printf("    %s4%s  tutti e due%s\n", C_CORAL, C_R, C_R);
    printf("\n  %sinvio = solo chattare%s\n", C_DIM, C_R);
    printf("\n%s\xe2\x94\x82%s %s%s\xe2\x80\xba%s ", C_GRAY, C_R, C_CORAL, C_BOLD, C_R);
    fflush(stdout);

    char line[256];
    if (prompt_line(line, sizeof line)) return -1;
    int c = line[0] ? line[0] : '1';
    if (c == 'q') return -1;
    if (c == '2' || c == '4') r->disk = 1;
    if (c == '3' || c == '4') r->compute = 1;
    if (!r->disk && !r->compute) return 0;

    if (r->disk) {
        const char *home = getenv("HOME") ? getenv("HOME") : ".";
        snprintf(r->model_dir, sizeof r->model_dir, "%s/.lumabri/%s/donated",
                 home, model);
        mkdir_p(r->model_dir);
        double freeg = free_gb_at(r->model_dir);
        double suggest = freeg * 0.25;
        if (suggest > 100) suggest = 100;
        if (suggest < 1) suggest = 1;
        printf("\n  %squanti GB doni? %.0f liberi in %s%s\n",
               C_DIM, freeg, r->model_dir, C_R);
        printf("  %sinvio = %.0f GB%s\n", C_DIM, suggest, C_R);
        printf("\n%s\xe2\x94\x82%s %s%s\xe2\x80\xba%s ", C_GRAY, C_R, C_CORAL, C_BOLD, C_R);
        fflush(stdout);
        if (prompt_line(line, sizeof line)) return -1;
        r->gb = line[0] ? atof(line) : suggest;
        if (r->gb <= 0) r->gb = suggest;
        if (r->gb > freeg) {
            printf("  %s%.0f GB non ci stanno: dono %.0f%s\n",
                   C_DIM, r->gb, freeg > 1 ? freeg - 1 : 0.0, C_R);
            r->gb = freeg > 1 ? freeg - 1 : 0;
            if (r->gb <= 0) r->disk = 0;
        }
    }
    return 0;
}

/* A port nobody is on, starting from `from` — a donor picked from a TUI
 * cannot ask the user for one, and two chatters on the same box must not
 * collide. */
static int free_port(int from) {
    for (int p = from; p < from + 200; p++) {
        int fd = lmb_listen(p);
        if (fd >= 0) { close(fd); return p; }
    }
    return 0;
}

/* Start whatever was chosen. Never fatal: a donation that cannot start is a
 * missed contribution, not a reason to refuse someone a conversation. */
static void role_start(const Role *r, const char *tracker, const char *model,
                       const char *model_type) {
    char dir[1024];
    exe_dir(dir, sizeof dir);

    if (r->disk) {
        char bin[1200], portstr[16], gbstr[32], name[64];
        snprintf(bin, sizeof bin, "%s/maintainer", dir);
        int port = free_port(7601);   /* clear of the test ranges */
        if (!port || access(bin, X_OK)) {
            printf("  %snon riesco ad avviare il maintainer: dono disco saltato%s\n",
                   C_DIM, C_R);
        } else {
            char base[48];
            donor_base_name(r, base, sizeof base);
            snprintf(portstr, sizeof portstr, "%d", port);
            snprintf(gbstr, sizeof gbstr, "%.2f", r->gb);
            snprintf(name, sizeof name, "%s-disk-%d", base, port);
            char *argv[20];
            int a = 0;
            argv[a++] = bin;
            argv[a++] = "--root";       argv[a++] = (char *)r->model_dir;
            argv[a++] = "--port";       argv[a++] = portstr;
            argv[a++] = "--tracker";    argv[a++] = (char *)tracker;
            argv[a++] = "--name";       argv[a++] = name;
            argv[a++] = "--model-name"; argv[a++] = (char *)model;
            argv[a++] = "--donate";     argv[a++] = gbstr;
            /* the TUI asked for this key once and remembered it: the donor
             * should refuse unsigned bytes for exactly the same reason the
             * chatter does */
            const char *pub = getenv("LUMABRI_PUBKEY");
            if (pub && pub[0]) { argv[a++] = "--pubkey"; argv[a++] = (char *)pub; }
            argv[a] = NULL;
            { char lp[1200];
              pid_t np = spawn_argv_logged(argv, NULL,
                                           donor_log_path(name, lp, sizeof lp));
              child_publish(g_nchildren++, np); }
            printf("  %s\xe2\x9c\xa6 dono %.0f GB di %s%s%s%s: il tracker mi assegna "
                   "i file piu\xcc\x80 rari%s\n",
                   C_GRN, r->gb, C_R, C_BOLD, model, C_DIM, C_R);
        }
    }

    if (r->compute) {
        const char *node = expert_node_for(model_type ? model_type : "");
        char bin[1200];
        snprintf(bin, sizeof bin, "%s/%s", dir, node ? node : "expert_node");
        int port = free_port(7701);
        /* Which container does the node read? The user's local copy when
         * there is one. Otherwise the model's vroot behind the shim: every
         * loader read becomes a verified block fetch from the swarm, so the
         * node pulls exactly the experts the tracker assigned it — donating
         * compute no longer requires owning the model. Weights still never
         * cross the EXEC channel; this is the disk-donor delivery path,
         * hash-verified, feeding an executor. */
        char probe[1100], shim[1200];
        snprintf(probe, sizeof probe, "%s/config.json", r->model_dir);
        int local = r->model_dir[0] && access(probe, R_OK) == 0;
        snprintf(shim, sizeof shim, "%s/liblumabri.so", dir);
        if (access(shim, R_OK))
            snprintf(shim, sizeof shim, "%s/../lib/lumabri/liblumabri.so", dir);
        if (!node || !port || access(bin, X_OK)) {
            printf("  %snessun expert node per %s (make engines): dono calcolo "
                   "saltato%s\n", C_DIM, model_type ? model_type : "?", C_R);
        } else if (!local && access(shim, R_OK)) {
            printf("  %sliblumabri.so mancante (make): dono calcolo saltato%s\n",
                   C_DIM, C_R);
        } else {
            /* same mirror the chatter uses: dense blocks are shared, and the
             * shim's cache lock is shared for a process lifetime */
            const char *home = getenv("HOME") ? getenv("HOME") : ".";
            char vroot[1024], cachedir[1024], casdir[1040];
            char e_pre[1216], e_vr[1040], e_ca[1040], e_cs[1056],
                 e_tr[160], e_mo[96];
            char *envv[8];
            int ne = 0;
            if (!local) {
                const char *ve = getenv("LUMABRI_VROOT");
                const char *ce = getenv("LUMABRI_CACHE");
                if (ve && ve[0]) snprintf(vroot, sizeof vroot, "%s", ve);
                else snprintf(vroot, sizeof vroot, "%s/.lumabri/%s/vroot",
                              home, model);
                if (ce && ce[0]) snprintf(cachedir, sizeof cachedir, "%s", ce);
                else snprintf(cachedir, sizeof cachedir, "%s/.lumabri/%s/cache",
                              home, model);
                snprintf(casdir, sizeof casdir, "%s/.lumabri/cas", home);
                mkdir_p(cachedir);              /* the vroot stays virtual */
                snprintf(e_pre, sizeof e_pre, "LD_PRELOAD=%s", shim);
                snprintf(e_vr, sizeof e_vr, "LUMABRI_VROOT=%s", vroot);
                snprintf(e_ca, sizeof e_ca, "LUMABRI_CACHE=%s", cachedir);
                snprintf(e_cs, sizeof e_cs, "LUMABRI_CAS=%s", casdir);
                snprintf(e_tr, sizeof e_tr, "LUMABRI_TRACKER=%s", tracker);
                snprintf(e_mo, sizeof e_mo, "LUMABRI_MODEL=%s", model);
                envv[ne++] = e_pre;
                envv[ne++] = e_vr;
                envv[ne++] = e_ca;
                if (!getenv("LUMABRI_CAS")) envv[ne++] = e_cs;
                envv[ne++] = e_tr;
                envv[ne++] = e_mo;
                /* AUTOPIN behind the shim would mirror GBs of experts the
                 * tracker never assigned; an explicit PIN still wins */
                if (!getenv("PIN")) envv[ne++] = (char *)"PIN=0";
                envv[ne] = NULL;
            }
            char portstr[16], name[64], base[48];
            donor_base_name(r, base, sizeof base);
            snprintf(portstr, sizeof portstr, "%d", port);
            snprintf(name, sizeof name, "%s-exec-%d", base, port);
            char *argv[20];
            int a = 0;
            argv[a++] = bin;
            argv[a++] = "--model";      argv[a++] = local ? (char *)r->model_dir
                                                          : vroot;
            argv[a++] = "--port";       argv[a++] = portstr;
            argv[a++] = "--tracker";    argv[a++] = (char *)tracker;
            argv[a++] = "--name";       argv[a++] = name;
            argv[a++] = "--model-name"; argv[a++] = (char *)model;
            argv[a++] = "--hold";       argv[a++] = "auto";   /* the tracker hands us a disjoint slice; the node sizes it to free RAM */
            argv[a] = NULL;
            { char lp[1200];
              pid_t np = spawn_argv_logged(argv, local ? NULL : envv,
                                           donor_log_path(name, lp, sizeof lp));
              child_publish(g_nchildren++, np); }
            if (local)
                printf("  %s\xe2\x9c\xa6 eseguo esperti per lo sciame%s %s(%s · "
                       "log in ~/.lumabri/logs, /debug per vederli)%s\n",
                       C_GRN, C_R, C_DIM, node, C_R);
            else
                printf("  %s\xe2\x9c\xa6 eseguo esperti per lo sciame%s %s(%s, "
                       "la fetta assegnata arriva dallo sciame · /debug per i log)%s\n",
                       C_GRN, C_R, C_DIM, node, C_R);
        }
    }
    if (r->disk || r->compute)
        printf("  %sfinche\xcc\x81 questa chat resta aperta. Per un donatore che "
               "sopravvive alla sessione: lumabri serve --join%s\n", C_DIM, C_R);
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
    /* The stock engine works and downloads every expert it routes to. On a
     * 299 GB model that is the difference between 12 GB and all of it, and
     * the only sign used to be the absence of a line. If the swarm has
     * executors, say it here, before anything is fetched. */
    if (!local_dir && !strstr(engine, "_p2p")) {
        LmbMsg em = {0};
        LmbBuf eb = {0};
        lmb_buf_str(&eb, model);
        int nexec = 0;
        if (!lmb_request(tracker, LMB_EPEERS, eb.p, (uint32_t)eb.len, &em) &&
            em.op == LMB_EPEERS_R) {
            LmbCur ec = { em.body, em.body_len, 0 };
            uint32_t n = 0;
            if (!lmb_cur_u32(&ec, &n)) nexec = (int)n;
        }
        free(eb.p);
        lmb_msg_free(&em);
        if (nexec > 0)
            printf("\n  %s⚠ questo motore non e' la build P2P: gli esperti li "
                   "scarichera' invece di farli eseguire%s\n"
                   "  %sci sono %d peer pronti a eseguirli. Serve %s_p2p, che "
                   "si costruisce con:  make chatters ENGINE=/path/to/colibri/c%s\n\n",
                   C_RED, C_R, C_DIM, nexec, engine_for(mtype), C_R);
    }
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
    /* Every engine but GLM hands out FRAME_READY like the older framed engines
     * but then speaks the SUBMIT/DATA/DONE serve codec, not raw-prompt/FRAME_END
     * — and the two are identical at the handshake (both announce with
     * FRAME_READY + a STAT line; qwen36's boot STAT even has GLM's 4-field shape,
     * so the field count can't tell them apart). So switch on the engine kind,
     * the way the olmoe line dialect used to be picked. */
    if (e->proto == PROTO_FRAMED && kind_is_serve2(e->kind))
        e->proto = PROTO_SERVE2;
    if (ready) {
        printf("  %s✗ il motore non è arrivato a essere pronto%s\n", C_RED, C_R);
        engine_diag(e, 1);
        engine_stop(e);
        return -1;
    }
    printf("  %s\xe2\x9c\x93 %s pronto in %.1fs%s%s · net %.0f MB · "
           "/swarm /model /debug /reset /quit%s\n",
           C_GRN, model, nowd() - t0, C_R, C_DIM, g_eng.net_mb, C_R);
    return 0;
}

static int cmd_chat(int argc, char **argv) {
    const char *tracker = NULL;
    const char *engine_path = NULL, *engines_dir = getenv("LUMABRI_ENGINES");
    const char *want_model = NULL, *local_dir = NULL;
    const char *role_arg = NULL, *model_dir_arg = NULL;
    const char *donor_name_arg = NULL;
    double donate_gb = 0;
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
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role_arg = argv[++i];
        else if (!strcmp(argv[i], "--model-dir") && i + 1 < argc) model_dir_arg = argv[++i];
        else if (!strcmp(argv[i], "--donate") && i + 1 < argc) donate_gb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--donor-name") && i + 1 < argc) donor_name_arg = argv[++i];
        else if (!strcmp(argv[i], "--plain")) g_tty = 0;
        else { fprintf(stderr, "usage: lumabri chat [--tracker H:P] [--model NAME] "
                               "[--local DIR] [--engine BIN] [--engines-dir DIR]\n"
                               "                    [--max-new N] [--ctx N] [--cap N]\n"
                               "                    [--role chat|disk|compute|all] "
                               "[--donate GB] [--model-dir DIR] [--donor-name S]\n");
               return 2; }
    }

    /* The panel comes BEFORE anything is contacted: the wordmark, then what
     * is missing, then the role. A TUI user never sees a flag. */
    Cfg cfg;
    cfg_load(&cfg);
    int interactive = !local_dir && g_tty;
    if (interactive) {
        int W0 = term_w() - 2; if (W0 > 66) W0 = 66;
        printf("\n");
        hline("\xe2\x95\xad", "\xe2\x95\xae", W0);
        panel_row(W0, "", "");
        for (int r = 0; r < 6; r++) {
            char row[512];
            snprintf(row, sizeof row, "\x1b[38;5;%dm%s\x1b[0m", WORD_TINT[r], WORDMARK[r]);
            panel_row(W0, row, "");
        }
        char tag0[256];
        snprintf(tag0, sizeof tag0, "%s\xe2\x9c\xbb%s %stiny engine, immense swarm%s",
                 C_CORAL, C_R, C_DIM, C_R);
        panel_row(W0, "", ""); panel_row(W0, tag0, "");
        hline("\xe2\x95\xb0", "\xe2\x95\xaf", W0);
        printf("\n");
        if (!tracker && !cfg.tracker[0]) setup_panel(&cfg);
        else if (!tracker) {
            printf("  %ssciame%s %s%s%s%s   invio per confermare, o un altro "
                   "indirizzo%s\n", C_DIM, C_R, C_BOLD, cfg.tracker, C_R, C_DIM, C_R);
            printf("\n%s\xe2\x94\x82%s %s%s\xe2\x80\xba%s ", C_GRAY, C_R, C_CORAL, C_BOLD, C_R);
            fflush(stdout);
            char l[1200];
            if (!prompt_line(l, sizeof l) && l[0]) {
                if (tracker_addr_set(cfg.tracker, sizeof cfg.tracker, l))
                    printf("  %sindirizzo troppo lungo, uso quello salvato%s\n", C_RED, C_R);
                else
                    cfg_save(&cfg);
            }
            printf("\n");
        }
    }
    if (!tracker && cfg.tracker[0]) tracker = cfg.tracker;
    if (!tracker) tracker = "127.0.0.1:7300";
    /* the key is remembered, never retyped, and never overrides an explicit
     * environment: a script that sets LUMABRI_PUBKEY means it */
    if (cfg.pubkey[0] && !getenv("LUMABRI_PUBKEY")) setenv("LUMABRI_PUBKEY", cfg.pubkey, 1);
    if (!engines_dir && !engine_path) {
        if (cfg.engines[0] && access(cfg.engines, X_OK) == 0) engines_dir = cfg.engines;
        else {
            static char found[1024];
            if (find_engines(found, sizeof found) == 0) {
                engines_dir = found;
                snprintf(cfg.engines, sizeof cfg.engines, "%s", found);
                cfg_save(&cfg);
            }
        }
    }

    char models[16][64];
    int nmodels = 0;
    char model[64];
    if (local_dir) {
        const char *base = strrchr(local_dir, '/');
        if (checked_printf(model, sizeof model, "%s",
                           base && base[1] ? base + 1 : local_dir)) {
            fprintf(stderr, "model name is longer than %zu bytes\n",
                    sizeof model - 1);
            return 2;
        }
    } else {
        nmodels = swarm_models(tracker, models, 16);
        if (nmodels <= 0) {
            fprintf(stderr, "%sno swarm at %s%s\n"
                            "start one with:  lumabri serve --model <dir>\n"
                            "or chat with a model already on this disk:  "
                            "lumabri chat --local <dir>\n", C_RED, tracker, C_R);
            return 1;
        }
        if (checked_printf(model, sizeof model, "%s",
                           want_model ? want_model : models[0])) {
            fprintf(stderr, "model name is longer than %zu bytes\n",
                    sizeof model - 1);
            return 2;
        }
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

    if (!interactive) {                 /* the panel was already drawn above */
        int W = term_w() - 2;
        if (W > 66) W = 66;
        printf("\n");
        hline("\xe2\x95\xad", "\xe2\x95\xae", W);
        panel_row(W, "", "");
        for (int r = 0; r < 6; r++) {
            char row[512];
            snprintf(row, sizeof row, "%s", WORDMARK[r]);
            panel_row(W, row, "");
        }
        panel_row(W, "", "");
        panel_row(W, "* tiny engine, immense swarm", "");
        hline("\xe2\x95\xb0", "\xe2\x95\xaf", W);
    }
    if (nmodels > 1) {
        printf("  %s%d modelli sullo sciame:%s", C_DIM, nmodels, C_R);
        for (int i = 0; i < nmodels; i++) printf(" %s%s%s", C_BOLD, models[i], C_R);
        printf("  %s(/model per cambiare)%s\n", C_DIM, C_R);
    }
    printf("\n");

    /* the role, before the engine boots: a donor started now warms up while
     * the dense weights cross the wire, instead of after */
    Role role = {0};
    if (donor_name_arg)
        snprintf(role.donor_name, sizeof role.donor_name, "%s", donor_name_arg);
    if (!local_dir && role_arg) {
        char bad[40];
        if (role_unknown(role_arg, bad, sizeof bad)) {
            fprintf(stderr, "--role: non conosco \"%s\". Vuole chat, disk, "
                            "compute o all (anche combinati: "
                            "--role disk,compute)\n", bad);
            return 2;
        }
        int all = role_has(role_arg, "all");
        if (all || role_has(role_arg, "disk"))    role.disk = 1;
        if (all || role_has(role_arg, "compute")) role.compute = 1;
        if (role.disk || role.compute) {
            const char *home = getenv("HOME") ? getenv("HOME") : ".";
            if (model_dir_arg) snprintf(role.model_dir, sizeof role.model_dir, "%s", model_dir_arg);
            else snprintf(role.model_dir, sizeof role.model_dir,
                          "%s/.lumabri/%s/donated", home, model);
            mkdir_p(role.model_dir);
            role.gb = donate_gb > 0 ? donate_gb : 10;
        }
    } else if (!local_dir && g_tty) {
        char probe[1100];
        int have_dir = 0;
        if (model_dir_arg) {
            snprintf(role.model_dir, sizeof role.model_dir, "%s", model_dir_arg);
            snprintf(probe, sizeof probe, "%s/config.json", role.model_dir);
            have_dir = access(probe, R_OK) == 0;
        }
        if (role_pick(&role, model, have_dir)) return 0;
        if (have_dir && role.compute)
            snprintf(role.model_dir, sizeof role.model_dir, "%s", model_dir_arg);
    }

    if (model_boot(tracker, model, shim, engines_dir, engine_path, local_dir,
                   ctx, max_new, cap_experts, &eng, &sw))
        return 1;
    if (role.disk || role.compute) {
        signal(SIGINT, on_sigint);            /* the donors die with the chat */
        signal(SIGTERM, on_sigint);
        role_start(&role, tracker, model, sw.model_type);
    }

    char *conv = calloc(1, 1);   /* serve-codec conversation history (after bos) */
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
        int got = prompt_line(line, sizeof line) == 0;   /* line editor when a tty */
        if (g_tty) hline("\xe2\x95\xb0", "\xe2\x95\xaf", w);
        if (!got) break;
        size_t L = strlen(line);   /* prompt_line already stripped the newline */
        if (!L) continue;
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
        if (!strcmp(line, "/swarm")) { render_swarm(tracker); continue; }
        if (!strcmp(line, "/debug")) { render_debug(); continue; }
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
            if (checked_printf(model, sizeof model, "%s", arg)) {
                printf("  %snome modello troppo lungo%s\n", C_RED, C_R);
                continue;
            }
            engine_stop(&eng);
            if (model_boot(tracker, model, shim, engines_dir, engine_path, local_dir,
                           ctx, max_new, cap_experts, &eng, &sw))
                return 1;
            continue;
        }

        int is_reset = !strcmp(line, "/reset");
        if (eng.proto == PROTO_SERVE2) {
            /* the serve codec has no reset command; dropping the local history
             * (and starting a fresh bos) is the conversation reset. */
            if (is_reset) {
                free(conv); conv = calloc(1, 1);
                printf("  %s\xe2\x9c\xa6 nuova conversazione%s\n", C_DIM, C_R);
                continue;
            }
            if (!conv || submit_serve2(&eng, conv, line, max_new)) break;
        } else if (eng.proto == PROTO_FRAMED) {
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

        if (eng.proto == PROTO_SERVE2) {
            char *reply = NULL;
            printf("%s%s\xe2\x97\x86 %s%s\n  ", C_BOLD, C_CORAL, model, C_R);
            g_eng.streaming = 1;
            int dead = stream_serve2(&eng, stat, sizeof stat, &reply);
            g_eng.streaming = 0;
            if (dead) {
                fprintf(stderr, "\n%sengine exited%s\n", C_RED, C_R);
                engine_diag(&eng, 0);
                free(reply);
                break;
            }
            printf("\n");
            conv = serve2_history_append(&eng, conv, line, reply ? reply : "");
            free(reply);
        } else if (eng.proto == PROTO_FRAMED) {
            if (!is_reset) printf("%s%s\xe2\x97\x86 %s%s\n  ", C_BOLD, C_CORAL, model, C_R);
            g_eng.streaming = 1;
            int dead = stream_until_end(&eng, stat, sizeof stat);
            g_eng.streaming = 0;
            if (dead) {
                fprintf(stderr, "\n%sengine exited%s\n", C_RED, C_R);
                engine_diag(&eng, 0);
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
                engine_diag(&eng, 0);
                break;
            }
            char *text = reply;
            while (*text == '\n') text++;
            printf("%s%s\xe2\x97\x86 %s%s\n", C_BOLD, C_CORAL, model, C_R);
            printf("  %s\n", text);
            free(reply);
        }

        if (g_eng.deferred) {
            printf("  %s\xe2\x9c\xa6 %d righe di rete durante la risposta — "
                   "/debug per vederle%s\n", C_DIM, g_eng.deferred, C_R);
            g_eng.deferred = 0;
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

    free(conv);
    engine_stop(&eng);
    /* The picker promises the donation lasts as long as the chat. That was
     * only true for Ctrl-C: a normal /quit returned and left the maintainer
     * running as an orphan, still serving, with nobody left who knew it
     * existed. */
    for (int i = 0; i < g_nchildren; i++) {
        pid_t p = g_children[i];
        child_unpublish(i);
        kill(p, SIGTERM);
        waitpid(p, NULL, 0);
    }
    if (g_nchildren) printf("  %sdonazione chiusa%s\n", C_DIM, C_R);
    printf("\n");
    return 0;
}

/* ---- key: the operator's identity ---------------------------------------
 * Ed25519 keypair. The secret signs the swarm's ground truth and belongs
 * only on the machine that owns the model — ideally offline, since the
 * signatures are computed once. The public half is what everyone else
 * needs, and it is the ONLY thing a chatter must get out of band: with it,
 * neither the tracker nor any peer has to be trusted. */
static int key_write_full(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = EIO; return -1; }
        data += n; len -= (size_t)n;
    }
    return 0;
}

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

    char skpath[1100], pkpath[1100], skhex[130], pkhex[66];
    int sn = snprintf(skpath, sizeof skpath, "%s.key", out);
    int pn = snprintf(pkpath, sizeof pkpath, "%s.pub", out);
    if (sn < 0 || (size_t)sn >= sizeof skpath ||
        pn < 0 || (size_t)pn >= sizeof pkpath) {
        fprintf(stderr, "key output path is too long\n");
        return 1;
    }

    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int skfd = open(skpath, flags, 0600);
    if (skfd < 0) { perror(skpath); return 1; }
    int pkfd = open(pkpath, flags, 0644);
    if (pkfd < 0) {
        int saved = errno;
        close(skfd); unlink(skpath); errno = saved;
        perror(pkpath);
        return 1;
    }

    lmb_hex(skhex, sk, 64); skhex[128] = '\n'; skhex[129] = 0;
    lmb_hex(pkhex, pk, 32); pkhex[64] = '\n'; pkhex[65] = 0;
    int ok = fchmod(skfd, 0600) == 0 &&
             key_write_full(skfd, skhex, 129) == 0 && fsync(skfd) == 0 &&
             key_write_full(pkfd, pkhex, 65) == 0 && fsync(pkfd) == 0;
    int saved = errno;
    if (close(skfd) && ok) { ok = 0; saved = errno; }
    if (close(pkfd) && ok) { ok = 0; saved = errno; }
    if (!ok) {
        unlink(skpath); unlink(pkpath); errno = saved;
        perror("cannot write operator keypair");
        return 1;
    }
    pkhex[64] = 0;

    printf("\n  %ssecret%s %s  %s(0600 — keep it off the swarm)%s\n",
           C_BOLD, C_R, skpath, C_DIM, C_R);
    printf("  %spublic%s %s  %s%s%s\n\n", C_BOLD, C_R, pkpath, C_DIM, pkhex, C_R);
    printf("  serve the model as its origin:\n");
    printf("    %slumabri serve --model DIR --key %s%s\n", C_DIM, skpath, C_R);
    printf("  let everyone verify (give them the public value, not the file):\n");
    printf("    %sLUMABRI_PUBKEY=%s lumabri chat --tracker HOST:7300%s\n\n",
           C_DIM, pkhex, C_R);
    return 0;
}

/* Print this machine's transport/registration identity.  Operators use this
 * public value to build LUMABRI_PEER_PINS without exposing peer.key. */
static int cmd_peer_key(int argc, char **argv) {
    (void)argv;
    if (argc) { fprintf(stderr, "usage: lumabri peer-key\n"); return 2; }
    char path[512], hex[65]; uint8_t sk[64], pk[32];
    const char *kp = lmb_peer_key_path(path, sizeof path);
    if (lmb_peer_identity(kp, sk, pk)) { perror(kp); return 1; }
    lmb_hex(hex, pk, sizeof pk);
    printf("%s\n", hex);
    memset(sk, 0, sizeof sk);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
    g_tty = isatty(1);
    if (argc >= 2 && !strcmp(argv[1], "key")) return cmd_key(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "peer-key"))
        return cmd_peer_key(argc - 2, argv + 2);
    if (lmb_secure_init()) return 1; /* children inherit the same strict mode */
    const char *tok = getenv("LUMABRI_TOKEN");
    if (tok && strlen(tok) > LMB_TOKEN_MAX) {
        fprintf(stderr, "LUMABRI_TOKEN must be at most %u bytes\n",
                (unsigned)LMB_TOKEN_MAX);
        return 2;
    }
    if (argc >= 2 && !strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "chat"))  return cmd_chat(argc - 2, argv + 2);
    /* No arguments and a terminal: this is a person, not a script. Chat is
     * the only thing a person wants by default, and everything it needs is
     * either remembered or asked for in the panel. */
    if (argc == 1 && g_tty) return cmd_chat(0, NULL);
    fprintf(stderr,
        "lumabri: run huge models from a swarm of peers\n\n"
        "  lumabri                                                    chat (asks what it needs)\n"
        "  lumabri peer-key                                           print this machine's endpoint identity\n"
        "  lumabri serve --model DIR [--port 7300] [--join TRACKER]   share a model\n"
        "  lumabri chat  [--tracker HOST:7300] [--model NAME]         chat with it\n"
        "  lumabri key   [--out NAME]                                 operator keypair\n");
    return 2;
}
