/* lumabri.c — household workspace and explicit advanced CLI tools.
 *
 *   lumabri serve --model DIR      share a model with the swarm
 *   lumabri                       open the household workspace
 *   lumabri chat --tracker H:P    use the advanced swarm chat path
 *
 * `serve` runs the tracker and a maintainer for the given directory.
 * `chat` asks the tracker what is available, mounts the chosen model through
 * the LD_PRELOAD shim (nothing is downloaded up front; blocks arrive on
 * first touch and stay in the local mirror), spawns the UNMODIFIED colibri
 * engine in its interactive CHAT mode, and wraps it in a terminal UI.
 *
 * In-chat commands: /swarm and /hosts (named live topology), /experts
 * (executor use), /model, /debug, /storage, /reset, /help and /quit.
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
#include "lumabri_ready.h"
#include "lumabri_platform.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "lumabri_home_net.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <locale.h>
#include <limits.h>
#include <wchar.h>
#include <poll.h>
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
#include "lumabri_families.h"
#include "lumabri_cluster.h"
#include "src/ui/lumabri_tui.h"
#include "src/ui/lumabri_visual.h"
#include "src/ui/lumabri_execution_view.h"
#include "lumabri_machine.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "lumabri_segment_discovery.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "src/runtime/lumabri_preload.h"
#include "lumabri_inventory.h"
#include "lumabri_home.h"
#include "lumabri_runtime_identity.h"
#include "lumabri_runtime_probe.h"
#include "lumabri_checkpoint_identity.h"
#include "lumabri_calibration_store.h"
#include "src/runtime/lumabri_probe_deadline.h"

/* Borrowed by the synchronous household chat only. Ordinary/legacy hosts
 * cannot create an entry without an approved plan and actual numeric metadata. */
static LmbCalibration *g_recording_calibration;
static const char *g_calibration_directory;

/* ---- terminal ----------------------------------------------------------- */

static int g_tty = 0;
/* Borrowed only during the synchronous household cmd_chat call. The plan is
 * immutable while the chat's input/status workers can read it. */
static const LmbExecutionView *g_execution_view;
/* Snapshot the shell's terminal state once, before either line editor can
 * touch it.  Taking live_begin's "old" value from the current tty created a
 * narrow hand-off race where it could inherit the previous editor's cbreak
 * mode and then faithfully restore a non-canonical terminal on Ctrl-Z. */
static struct termios g_chat_term;
static int g_chat_term_valid;
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

/* Engine descriptors are pipes for a child and sockets for a hosted engine.
 * lmb_write_full is socket-only (send), so it cannot write to a child pipe. */
static int engine_write_full(int fd, const void *data, size_t bytes) {
    const unsigned char *p = data;
    while (bytes) {
        ssize_t n = write(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; bytes -= (size_t)n;
    }
    return 0;
}

static int term_h(void) {
    struct winsize ws;
    if (g_tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 7)
        return ws.ws_row;
    return 24;
}

static const char *const CHAT_COMMANDS[] = {
    "/swarm", "/experts", "/hosts", "/model", "/debug", "/storage",
    "/reset", "/help", "/quit", "/plan",
};
static int g_slash_completion;
static const char *const CHAT_COMMAND_HELP[] = {
    "Show cluster activity", "Show expert execution", "Show computers",
    "Show the current model", "Open diagnostics", "Show cache storage",
    "Start a fresh conversation", "List chat commands", "Close this chat",
    "Show the approved layer allocation",
};
_Static_assert(sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS ==
               sizeof CHAT_COMMAND_HELP / sizeof *CHAT_COMMAND_HELP, "command help parity");

static int chat_command_matches(const char *prefix, int *indices) {
    if (!prefix || prefix[0] != '/' || strchr(prefix, ' ')) return 0;
    int n = 0;
    for (size_t i = 0; i < sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS; i++)
        if (!strncmp(CHAT_COMMANDS[i], prefix, strlen(prefix))) indices[n++] = (int)i;
    return n;
}

static void exe_dir(char *dst, size_t cap) {
#ifdef __APPLE__
    char executable[PATH_MAX];
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size)) {
        snprintf(dst, cap, "."); return;
    }
    char *resolved = realpath(executable, NULL);
    if (!resolved) { snprintf(dst, cap, "."); return; }
    snprintf(dst, cap, "%s", resolved);
    free(resolved);
#else
    ssize_t n = readlink("/proc/self/exe", dst, cap - 1);
    if (n <= 0) { snprintf(dst, cap, "."); return; }
    dst[n] = 0;
#endif
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

static int vis_len(const char *s);
static void hline(const char *l, const char *r, int w) {
    printf("%s%s", C_GRAY, l);
    for (int i = 0; i < w - 2; i++) printf("\xe2\x94\x80");
    printf("%s%s\n", r, C_R);
}

/* the same frame edge carrying a short text: "╭─ text ──────╮" */
static void hline_text(const char *l, const char *r, int w, const char *text) {
    int tw = text && text[0] ? vis_len(text) + 2 : 0;
    printf("%s%s", C_GRAY, l);
    if (tw) { printf("\xe2\x94\x80 %s%s%s ", C_DIM, text, C_GRAY); tw += 1; }
    for (int i = 0; i < w - 2 - tw; i++) printf("\xe2\x94\x80");
    printf("%s%s", r, C_R);
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


/* ---- serve -------------------------------------------------------------- */

static int child_follow_parent(pid_t parent) {
#ifdef __linux__
    /* Close fork-to-prctl: the parent may have died before the child armed
     * its signal. This also keeps a 30 GB executor from surviving its TUI. */
    return prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent;
#else
    (void)parent;
    return 0;
#endif
}

static pid_t spawn_argv(char *const argv[]) {
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        if (child_follow_parent(parent)) _exit(125);
        execv(argv[0], argv); perror(argv[0]); _exit(127);
    }
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
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        if (child_follow_parent(parent)) _exit(125);
        for (int i = 0; envv && envv[i]; i++) putenv(envv[i]);
        int lfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd >= 0) { dup2(lfd, 1); dup2(lfd, 2); close(lfd); }
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    return pid;
}

#define MAX_CHILDREN 16
static pid_t g_children[MAX_CHILDREN];
static int g_nchildren = 0;
/* Held by the parent and inherited by its compute child. It serializes
 * automatic RAM-sized donation across chat/serve processes on one machine. */
static int g_compute_lease_fd = -1;
/* The async handler never reads the mutable table/count.  Each fixed slot is
 * one sig_atomic_t publication, so delivery on any worker thread cannot see a
 * compacted/torn child table. */
static volatile sig_atomic_t g_signal_children[MAX_CHILDREN];
/* what each child was, and how to start it again: a supervisor that cannot
 * name or restart what died is just a process that happens to be the parent */
static char **g_cargv[MAX_CHILDREN];
static const char *g_cwhat[MAX_CHILDREN];
/* Children that must not share the operator's terminal keep their own log,
 * and keep it across a supervised restart. */
static char *g_clog[MAX_CHILDREN];
/* restart history per slot, for the supervisor's backoff */
static int g_crestarts[MAX_CHILDREN];
static double g_cstarted[MAX_CHILDREN];

static void child_publish(int idx, pid_t pid) {
    g_children[idx] = pid;
    g_signal_children[idx] = (sig_atomic_t)pid;
}
static void child_unpublish(int idx) {
    g_signal_children[idx] = 0;
    g_children[idx] = 0;
}

/* logpath == NULL keeps the child on the operator's terminal, which is what
 * the tracker and the maintainer want. Several Segment slices share one
 * origin, and unbuffered stderr from N processes interleaves mid-line: an
 * operator was shown "run: hybrid b" and "run: q<garbage>v" instead of two
 * whole diagnoses. Those get a file each. */
static void spawn_tracked_logged(char *const argv[], const char *what,
                                 const char *logpath) {
    if (g_nchildren >= MAX_CHILDREN) {
        fprintf(stderr, "cannot start %s: child supervisor is full\n", what);
        return;
    }
    int n = 0;
    while (argv[n]) n++;
    char **copy = (char **)calloc((size_t)n + 1, sizeof *copy);
    for (int i = 0; i < n; i++) copy[i] = strdup(argv[i]);
    char *log = logpath ? strdup(logpath) : NULL;
    /* the slow part, outside the mask */
    pid_t pid = log ? spawn_argv_logged(argv, NULL, log) : spawn_argv(argv);
    int idx = g_nchildren;
    g_cargv[idx] = copy;
    g_cwhat[idx] = what;
    g_clog[idx] = log;
    child_publish(idx, pid);
    g_nchildren++;
}

static void spawn_tracked(char *const argv[], const char *what) {
    spawn_tracked_logged(argv, what, NULL);
}

static volatile sig_atomic_t g_stopping = 0;
/* The chat engine is not one of the optional donor children above.  Publish
 * it separately so a shutdown signal can break a blocked read immediately
 * instead of waiting for a long inference to finish. */
static volatile sig_atomic_t g_signal_engine_pid = 0;
/* The same, for an engine that is a socket rather than a child. -1 = none. */
static volatile sig_atomic_t g_signal_engine_fd = -1;

static void on_sigint(int sig) {
    if (g_stopping) {
        /* A second shutdown signal is the escape hatch for an engine stuck in
         * an uninterruptible path.  The live input thread restored termios
         * before the first Ctrl-C; _exit is async-signal-safe and cannot
         * deadlock on a stdio/pthread lock held by another thread. */
        sig_atomic_t engine = g_signal_engine_pid;
        if (engine > 0) kill((pid_t)engine, SIGKILL);
        /* A remote engine has no pid to kill, so the escape hatch is the
         * socket: shutdown() from a signal handler is async-signal-safe and
         * makes the blocked read in stream_serve2 return at once. Without
         * it, Ctrl-C on a hosted chat waits for a reply that may never
         * come. */
        sig_atomic_t sock = g_signal_engine_fd;
        if (sock >= 0) shutdown((int)sock, SHUT_RDWR);
        _exit(128 + (sig > 0 && sig < 128 ? sig : SIGTERM));
    }
    g_stopping = 1;
    sig_atomic_t engine = g_signal_engine_pid;
    if (engine > 0) kill((pid_t)engine, SIGTERM);
    /* A remote engine gets the same courtesy on the FIRST Ctrl-C: shutting
     * the socket down for reading wakes a blocked stream without discarding
     * anything already queued, which is what SIGTERM does for a child. */
    sig_atomic_t sock = g_signal_engine_fd;
    if (sock >= 0) shutdown((int)sock, SHUT_RD);
    for (int i = 0; i < MAX_CHILDREN; i++) {
        sig_atomic_t p = g_signal_children[i];
        if (p > 0) kill((pid_t)p, SIGTERM);
    }
}

static void install_chat_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_sigint;
    sigemptyset(&action.sa_mask);
    /* Do not use SA_RESTART: a signal from another terminal must also wake a
     * line-editor read.  During inference the engine termination closes the
     * pipe and wakes every streaming dialect independently of EINTR. */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
}

/* One table decides the family; the three lookups below only read it.
 *
 * LUMABRI_ENGINE_ID names an adapter directly. A name Colibri does not
 * register is refused instead of being ignored and falling through to an
 * automatic choice. */
static const LmbModelFamily *lumi_family(const char *model_type) {
    const char *forced = getenv("LUMABRI_ENGINE_ID");
    if (forced && forced[0]) {
        const LmbModelFamily *f = lmb_family_override(forced);
        if (!f) {
            fprintf(stderr, "[lumabri] LUMABRI_ENGINE_ID=%s is not an adapter "
                            "Colibri registers; refusing to choose an engine\n",
                    forced);
            return NULL;
        } else
            return f;
    }
    const LmbModelFamily *f = lmb_family_for(model_type ? model_type : "");
    /* A model_type nobody claims used to be impossible: every ladder ended in
     * a substring broad enough to catch anything, so an unknown checkpoint
     * was executed by whichever engine shared three letters with it. Now it
     * is a real state, and a real state has to be said out loud — once, with
     * the string and the way out, rather than a Segment path that silently
     * never starts. */
    static int told;
    if (!f && !told && model_type && model_type[0]) {
        told = 1;
        fprintf(stderr,
                "[lumabri] model_type \"%s\" is not mapped to any Colibri "
                "adapter, so no engine is chosen for it. Set "
                "LUMABRI_ENGINE_ID=<adapter> to name one:", model_type);
        for (size_t i = 0; i < LMB_FAMILY_COUNT; i++)
            fprintf(stderr, " %s", LMB_FAMILIES[i].segment_id);
        fprintf(stderr, "\n");
    }
    return f;
}

/* Which expert-node binary can execute this model's experts, or NULL when
 * that engine has no phase-2 build yet. One per engine family: the engines
 * do not share an expert shape, so neither can the peers. */
static const char *expert_node_for(const char *model_type) {
    const LmbModelFamily *f = lumi_family(model_type);
    return f ? f->expert_node : NULL;
}

/* Public Colibri Segment adapter ID for the same model family.  Unlike the
 * expert executors, these names are the stable ABI IDs rather than binary
 * basenames. NULL when no family claims this model_type — the caller says so
 * out loud rather than picking whichever row shares three letters with it. */
static const char *segment_engine_for(const char *model_type) {
    const LmbModelFamily *f = lumi_family(model_type);
    return f ? f->segment_id : NULL;
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
    (void)lmb_json_string(buf, "model_type", out, cap);
}

/* All current Colibri families publish num_hidden_layers in config.json.
 * Keep this deliberately narrow: guessing a different key could advertise a
 * plausible but incorrect range and execute the wrong graph. */
static int local_model_u32(const char *model_dir, const char *name,
                           uint32_t *result) {
    char path[1200];
    if (!model_dir || !name || !result ||
        checked_printf(path, sizeof path, "%s/config.json", model_dir))
        return -1;
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return -1; }
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return -1; }
    char *json = malloc((size_t)length + 1);
    if (!json) { fclose(file); return -1; }
    int bad = fread(json, 1, (size_t)length, file) != (size_t)length;
    fclose(file);
    json[length] = 0;
    char quoted[128];
    if (checked_printf(quoted, sizeof quoted, "\"%s\"", name)) {
        free(json); return -1;
    }
    char *key = bad ? NULL : strstr(json, quoted);
    char *colon = key ? strchr(key, ':') : NULL;
    char *end = NULL;
    errno = 0;
    unsigned long value = colon ? strtoul(colon + 1, &end, 10) : 0;
    bad = !colon || errno || end == colon + 1 || !value || value > UINT32_MAX;
    free(json);
    if (bad) return -1;
    *result = (uint32_t)value;
    return 0;
}

static int local_model_layers(const char *model_dir, uint32_t *layers) {
    return local_model_u32(model_dir, "num_hidden_layers", layers);
}

/* RAM the classic expert executor will grow into. It is spawned before the
 * Segment slices and sized by --exec-cache alone, so unless its residency is
 * subtracted here the Segment budget is computed against memory the executor
 * is about to fill: two caches of the same experts, both sized to "all the
 * free RAM", on one box. DeepSeek V4 is the family whose executor holds no
 * dense weights and whose expert size is fixed by config.json, so it can be
 * estimated exactly; the other glues run a full engine and are left at 0
 * (unknown), which keeps the old behaviour rather than inventing a number. */
static uint64_t expert_executor_ram_estimate(const char *model_dir,
                                             const char *model_type,
                                             int cache_slots) {
    uint32_t inter = 0, hidden = 0, layers = 0, topk = 0;
    const LmbModelFamily *family = lmb_family_for(model_type);
    if (!family || strcmp(family->segment_id, "deepseek_v4")) return 0;
    if (local_model_u32(model_dir, "moe_intermediate_size", &inter) ||
        local_model_u32(model_dir, "hidden_size", &hidden) ||
        local_model_layers(model_dir, &layers))
        return 0;
    if (local_model_u32(model_dir, "num_experts_per_tok", &topk)) topk = 6;
    uint64_t per_expert = (uint64_t)inter * hidden * 3 / 2;
    per_expert += per_expert / 5;                 /* the glue's own headroom */
    uint64_t slots = cache_slots > 0 ? (uint64_t)cache_slots : 8;
    uint64_t floor = (uint64_t)topk * layers;     /* the store's per-layer minimum */
    if (slots < floor) slots = floor;
    return per_expert * slots + ((uint64_t)1 << 30);   /* + 1 GiB of workspace */
}

/* A server with a real public IPv4 should need no networking flag. Private,
 * loopback, link-local and CGNAT addresses are deliberately not guessed:
 * publishing one to an Internet swarm would create a "complete" Segment
 * route that remote chatters cannot reach. NAT installations keep the
 * proven READ/EXEC relay path unless the operator supplies --advertise. */
static int machine_public_ipv4(char *out, size_t cap) {
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return -1;
    int found = -1;
    for (struct ifaddrs *it = all; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        uint32_t ip = ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr);
        if ((ip >> 24) == 0 || (ip >> 24) == 10 || (ip >> 24) == 127 ||
            (ip >> 16) == 0xa9fe || (ip >> 20) == 0xac1 ||
            (ip >> 16) == 0xc0a8 || (ip >> 22) == 0x0191) continue;
        char text[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)it->ifa_addr)->sin_addr,
                       text, sizeof text) || strlen(text) >= cap) continue;
        snprintf(out, cap, "%s", text);
        found = 0;
        break;
    }
    freeifaddrs(all);
    return found;
}

/* A readable, collision-resistant default for every role started by one
 * `serve`.  The port distinguishes two swarms on the same machine; role
 * suffixes distinguish storage, classic experts and Segment slices. */
static int machine_host_base(const char *chosen, int port,
                             char *out, size_t cap) {
    char raw[64] = "";
    if (chosen && chosen[0]) {
        if (strlen(chosen) >= sizeof raw) return -1;
        snprintf(raw, sizeof raw, "%s", chosen);
    }
    else if (gethostname(raw, sizeof raw - 1)) snprintf(raw, sizeof raw, "machine");
    char clean[24];
    size_t used = 0;
    for (const char *p = raw; *p && used < sizeof clean - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
            clean[used++] = (char)c;
        else if ((c == ' ' || c == '_' || c == '.') && used &&
                 clean[used - 1] != '-')
            clean[used++] = '-';
    }
    while (used && clean[used - 1] == '-') used--;
    clean[used] = 0;
    if (!used) snprintf(clean, sizeof clean, "machine");
    if (chosen && chosen[0]) return checked_printf(out, cap, "%s", clean);
    return checked_printf(out, cap, "host-%s-%d", clean, port);
}

static const char *model_name_for(const char *model_dir, const char *explicit,
                                  char *storage, size_t cap) {
    if (explicit && explicit[0]) return explicit;
    size_t length = strlen(model_dir);
    while (length > 1 && model_dir[length - 1] == '/') length--;
    const char *base = model_dir + length;
    while (base > model_dir && base[-1] != '/') base--;
    if (!length || (size_t)(model_dir + length - base) >= cap) return NULL;
    snprintf(storage, cap, "%.*s", (int)(model_dir + length - base), base);
    return storage;
}

static int parse_serve_port(const char *s, int *port) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (s[0] < '0' || s[0] > '9' || errno == ERANGE || !end || *end ||
        v < 1 || v > 65525) return -1;
    *port = (int)v;
    return 0;
}

static int serve_port_available(int port) {
    int fd = lmb_listen(port);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void serve_watch_start(const char *tracker, const char *model);

static uint64_t du_bytes(const char *path, int depth);

static int cmd_serve(int argc, char **argv) {
    const char *model = NULL, *join = NULL, *mname = NULL, *donate = NULL;
    const char *key = NULL, *pubkey = NULL, *advertise = NULL;
    const char *host_name = NULL;
    int port = 7300, no_exec = 0, cache_slots = 128;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            if (parse_serve_port(argv[++i], &port)) {
                fprintf(stderr, "--port must be an integer from 1 to 65525\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--model-name") && i + 1 < argc) mname = argv[++i];
        else if (!strcmp(argv[i], "--donate") && i + 1 < argc) donate = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc) pubkey = argv[++i];
        else if (!strcmp(argv[i], "--advertise") && i + 1 < argc) advertise = argv[++i];
        else if (!strcmp(argv[i], "--host-name") && i + 1 < argc) host_name = argv[++i];
        else if (!strcmp(argv[i], "--no-exec")) no_exec = 1;
        else if (!strcmp(argv[i], "--exec-cache") && i + 1 < argc) cache_slots = atoi(argv[++i]);
        else { fprintf(stderr, "usage: lumabri serve --model DIR [--port N] "
                               "[--join TRACKER] [--model-name S] [--donate GB] "
                               "[--key FILE] [--pubkey FILE] [--advertise HOST] "
                               "[--host-name NAME] "
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
    char host_base[40];
    if (machine_host_base(host_name, port, host_base, sizeof host_base)) {
        fprintf(stderr, "--host-name is empty or too long\n"); return 2;
    }
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
            fprintf(stderr, "%s%s does not contain config.json — not a "
                            "model directory%s\n"
                            "  (if the model is in a subdirectory, use "
                            "--model %s/<subdirectory>)\n",
                    C_RED, model, C_R, model);
            return 2;
        }
    }

    char auto_advertise[INET_ADDRSTRLEN] = "";
    if (!advertise && machine_public_ipv4(auto_advertise,
                                           sizeof auto_advertise) == 0) {
        advertise = auto_advertise;
        printf("  %snetwork: automatically advertising %s%s\n",
               C_DIM, advertise, C_R);
    }

    char dir[1024], tracker_bin[1200], maint_bin[1200], portstr[16], mport[16], taddr[64];
    exe_dir(dir, sizeof dir);
    snprintf(tracker_bin, sizeof tracker_bin, "%s/tracker", dir);
    snprintf(maint_bin, sizeof maint_bin, "%s/maintainer", dir);
    snprintf(portstr, sizeof portstr, "%d", port);
    snprintf(mport, sizeof mport, "%d", port + 1);
    if (join) snprintf(taddr, sizeof taddr, "%s", join);
    else      snprintf(taddr, sizeof taddr, "127.0.0.1:%d", port);

    /* Resolve the complete local topology before forking anything. Starting
     * half a server and then discovering EADDRINUSE made the supervisor retry
     * forever and produced a misleading apparently-live `serve`. */
    char exec_bin[1200] = "", mtype[64] = "";
    local_model_type(model, mtype, sizeof mtype);
    const char *node = expert_node_for(mtype);
    if (node) snprintf(exec_bin, sizeof exec_bin, "%s/%s", dir, node);
    char segment_bin[1200], segment_model_storage[64];
    snprintf(segment_bin, sizeof segment_bin, "%s/segment_node", dir);
    const char *segment_engine = segment_engine_for(mtype);
    const char *segment_model = model_name_for(
        model, mname, segment_model_storage, sizeof segment_model_storage);
    uint32_t segment_layers = 0;
    LmbMachineProfile serve_profile;
    (void)lmb_machine_probe(&serve_profile, model, NULL);
    uint64_t segment_available = serve_profile.ram_available_bytes;
    uint64_t segment_min_free = (uint64_t)lmb_env_int(
        "LUMABRI_SEGMENT_MIN_FREE_MB", 8192, 1024, 262144) << 20;
    uint64_t exec_estimate = 0;
    if (!disk_donor && !no_exec && node && access(exec_bin, X_OK) == 0)
        exec_estimate = expert_executor_ram_estimate(model, mtype, cache_slots);
    if (exec_estimate && segment_available) {
        printf("  %sthe expert executor may grow to ~%.1f GB "
               "(--exec-cache %d): reserving this outside the Segment budget%s\n", C_DIM,
               (double)exec_estimate / 1e9, cache_slots, C_R);
        segment_available = segment_available > exec_estimate
                          ? segment_available - exec_estimate : 1;
    }
    int segment_candidate = !disk_donor && !no_exec && segment_engine &&
        segment_model && access(segment_bin, X_OK) == 0 &&
        local_model_layers(model, &segment_layers) == 0 &&
        (!segment_available || segment_available >= segment_min_free);
    if (!segment_candidate && !disk_donor && !no_exec && segment_engine &&
        segment_model && access(segment_bin, X_OK) == 0 && segment_available &&
        segment_available < segment_min_free)
        printf("  %sAutomatic Segment startup skipped: %.1f GB RAM available, "
               "the governor requires %.1f. Storage and experts remain "
               "available.%s\n", C_DIM,
               (double)segment_available / 1e9,
               (double)segment_min_free / 1e9, C_R);

    /* A joined host donates one RAM-sized compute role, regardless of how
     * many `chat` or `serve --join` commands its user happens to open. The
     * origin is intentionally exempt: it is the swarm's fallback server. */
    int has_expert_runtime = node && access(exec_bin, X_OK) == 0;
    if (join && !disk_donor && !no_exec &&
        (segment_candidate || has_expert_runtime)) {
        char lease_owner[256] = "";
        g_compute_lease_fd = lmb_machine_compute_lease_acquire(
            segment_model ? segment_model : model, taddr,
            lease_owner, sizeof lease_owner);
        if (g_compute_lease_fd < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                printf("  %scompute sharing is already active on this machine%s%s%s; "
                       "this process offers storage only%s\n", C_DIM,
                       lease_owner[0] ? " (" : "",
                       lease_owner[0] ? lease_owner : "",
                       lease_owner[0] ? ")" : "", C_R);
            else
                printf("  %sdonor RAM lease unavailable: %s; "
                       "this process offers storage only%s\n",
                       C_DIM, strerror(errno), C_R);
            no_exec = 1;
            segment_candidate = 0;
        }
    }
    int planned_chunks = segment_candidate
        ? lmb_env_int("LUMABRI_SEGMENT_CHUNKS", 4, 1, 7) : 0;
    if ((uint32_t)planned_chunks > segment_layers)
        planned_chunks = (int)segment_layers;
    int first_port = join ? port + 1 : port;
    int last_port = port + 1;
    if (!disk_donor && !no_exec && node && access(exec_bin, X_OK) == 0)
        last_port = port + 2;
    if (planned_chunks) last_port = port + 2 + planned_chunks;
    for (int candidate_port = first_port; candidate_port <= last_port;
         candidate_port++) {
        if (candidate_port == port + 2 &&
            (disk_donor || no_exec || !node || access(exec_bin, X_OK)))
            continue;
        if (!serve_port_available(candidate_port)) {
            fprintf(stderr, "lumabri serve: port %d is already in use; "
                    "nothing was started\n", candidate_port);
            return 1;
        }
    }

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
        spawn_tracked(targv, "the tracker");
        usleep(300 * 1000);
    }
    char storage_name[64];
    if (checked_printf(storage_name, sizeof storage_name, "%s-storage", host_base))
        return 2;
    char *margv[32];
    int a = 0;
    margv[a++] = maint_bin;
    margv[a++] = "--root"; margv[a++] = (char *)model;
    margv[a++] = "--port"; margv[a++] = mport;
    margv[a++] = "--tracker"; margv[a++] = taddr;
    margv[a++] = "--name"; margv[a++] = storage_name;
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
    spawn_tracked(margv, "the maintainer");

    /* The bootstrap executor: when the model family has an expert node
     * build, serve also runs one on the whole model with an SSD-streaming
     * cache — so a brand-new swarm can chat phase-2 from minute zero with
     * this server executing every expert. Donors that join later are
     * discovered by the chatters and win the calls they are nearest for;
     * this node stays the replica of last resort. */
    /* one node binary per engine family — they do not share an expert shape */
    int with_exec = 0;
    if (!no_exec && !disk_donor && !node && mtype[0])
        printf("  %sphase 2 unavailable for engine %s: this server "
               "serves weights; the chatter executes experts%s\n",
               C_DIM, mtype, C_R);
    if (!no_exec && !disk_donor && node && access(exec_bin, X_OK))
        printf("  %s%s is not built: no experts execute here "
               "(make %s ENGINE=/path/to/colibri/c)%s\n", C_DIM, node, node, C_R);
    if (!no_exec && !disk_donor && node && access(exec_bin, X_OK) == 0) {
        char eport[16], cachestr[16], ename[64];
        snprintf(eport, sizeof eport, "%d", port + 2);
        snprintf(cachestr, sizeof cachestr, "%d", cache_slots);
        snprintf(ename, sizeof ename, "%s-experts", host_base);
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
        spawn_tracked(eargv, "the expert executor");
        with_exec = 1;
    }
    /* The node opens its port only after loading the dense side, which on a
     * big model is minutes. Until then a chatter that connects sees no
     * executor and concludes phase 2 is impossible — so say it, rather than
     * let someone race their own server and blame the swarm. */
    if (with_exec)
        printf("  %sthe executor is loading the model: port %d opens "
               "when loading finishes. A chatter connecting earlier will not "
               "find it and will download expert weights.%s\n",
               C_DIM, port + 2, C_R);

    /* Segment bootstrap: the origin publishes complete stateful coverage in
     * addition to the expert executor.  This binary exists only in builds
     * made against Colibri's additive Edge/Segment archive; release builds
     * without it preserve the exact old serve topology. */
    int with_segment = 0;
    if (segment_candidate) {
        long cores = serve_profile.physical_cores;
        if (cores < 1) cores = 1;
        /* Keep stable layer boundaries from minute zero. Donors take these
         * exact ranges one by one, so each new resident machine replaces a
         * corresponding origin slice without migrating an active chat. The
         * chain is sequential for decode: every local slice therefore gets
         * the complete CPU team, rather than the old cores/chunks team that
         * made four slices four times slower on one host. */
        int chunks = lmb_env_int("LUMABRI_SEGMENT_CHUNKS", 4, 1, 7);
        if ((uint32_t)chunks > segment_layers) chunks = (int)segment_layers;
        int slice_threads = lmb_env_int("LUMABRI_SEGMENT_THREADS",
                                        (int)cores, 1, 256);
        if (slice_threads < 1) slice_threads = 1;
        int sessions = lmb_env_int("LUMABRI_SEGMENT_SESSIONS",
                                   advertise ? 4 : 2, 1, 64);
        int run_queue = lmb_env_int("LUMABRI_SEGMENT_RUN_QUEUE", 32, 0, 256);
        int run_wait_ms = lmb_env_int("LUMABRI_SEGMENT_RUN_WAIT_MS",
                                      30000, 50, 60000);
        uint32_t segment_context = 4096, model_context = 0;
        if (!local_model_u32(model, "max_position_embeddings", &model_context) &&
            model_context < segment_context)
            segment_context = model_context;
        char context[16], session_text[16], thread_text[16], memory_text[32];
        char run_queue_text[16], run_wait_text[16];
        snprintf(context, sizeof context, "%u", segment_context);
        snprintf(session_text, sizeof session_text, "%d", sessions);
        snprintf(thread_text, sizeof thread_text, "%d", slice_threads);
        snprintf(run_queue_text, sizeof run_queue_text, "%d", run_queue);
        snprintf(run_wait_text, sizeof run_wait_text, "%d", run_wait_ms);
        uint64_t total_budget = segment_available > segment_min_free
                              ? segment_available - segment_min_free : 0;
        uint64_t origin_model_bytes = du_bytes(model, 0);
        char origin_model_text[32], origin_layers_text[16];
        snprintf(origin_model_text, sizeof origin_model_text, "%llu",
                 (unsigned long long)origin_model_bytes);
        snprintf(origin_layers_text, sizeof origin_layers_text, "%u",
                 segment_layers);
        uint64_t slice_budget_mb = (total_budget / (uint64_t)chunks) >> 20;
        if (!slice_budget_mb) slice_budget_mb = 1;
        snprintf(memory_text, sizeof memory_text, "%llu",
                 (unsigned long long)slice_budget_mb);

        /* A few disjoint origin slices retain the single-copy weight total,
         * but give placement exact boundaries it can replace independently:
         * as ordinary server-class peers join, each non-fallback slice wins
         * over its matching origin slice and this machine loses that portion
         * of the pipeline. Four is the latency/granularity default; the
         * operator may tune 1..7 without exposing layer ranges to users. */
        for (int chunk = 0; chunk < chunks; chunk++) {
            uint32_t begin = (uint32_t)((uint64_t)segment_layers * chunk /
                                         (uint32_t)chunks);
            uint32_t end = (uint32_t)((uint64_t)segment_layers * (chunk + 1) /
                                       (uint32_t)chunks);
            char sport[16], range[40], sname[64], sadv[80];
            snprintf(sport, sizeof sport, "%d", port + 3 + chunk);
            snprintf(range, sizeof range, "%u:%u", begin, end);
            snprintf(sname, sizeof sname, "%s-segment-%d", host_base,
                     chunk + 1);
            snprintf(sadv, sizeof sadv, "%s:%d",
                     advertise ? advertise : "127.0.0.1", port + 3 + chunk);
            char *sargv[40];
            a = 0;
            sargv[a++] = segment_bin;
            sargv[a++] = "--engine";       sargv[a++] = (char *)segment_engine;
            sargv[a++] = "--model-dir";    sargv[a++] = (char *)model;
            sargv[a++] = "--model";        sargv[a++] = (char *)segment_model;
            sargv[a++] = "--range";        sargv[a++] = range;
            sargv[a++] = "--port";         sargv[a++] = sport;
            sargv[a++] = "--tracker";      sargv[a++] = taddr;
            sargv[a++] = "--name";         sargv[a++] = sname;
            sargv[a++] = "--auto-identity";
            if (!join) sargv[a++] = "--fallback";
            if (!advertise) sargv[a++] = "--relay-only";
            sargv[a++] = "--context";      sargv[a++] = context;
            sargv[a++] = "--max-rows";     sargv[a++] = "16";
            sargv[a++] = "--sessions";     sargv[a++] = session_text;
            sargv[a++] = "--threads";      sargv[a++] = thread_text;
            sargv[a++] = "--run-queue";    sargv[a++] = run_queue_text;
            sargv[a++] = "--run-wait-ms";  sargv[a++] = run_wait_text;
            sargv[a++] = "--memory-limit-mb"; sargv[a++] = memory_text;
            /* A donor is refused a range it cannot hold resident; the origin
             * used to skip that check and open the engine anyway, which on a
             * model larger than the box meant every slice streamed experts
             * from disk into a cache that outgrew its budget, went DRAINING
             * and stayed there. Same check, same exit code 3, no restart. */
            if (origin_model_bytes) {
                sargv[a++] = "--model-bytes";  sargv[a++] = origin_model_text;
                sargv[a++] = "--model-layers"; sargv[a++] = origin_layers_text;
            }
            sargv[a++] = "--advertise";    sargv[a++] = sadv;
            sargv[a] = NULL;
            char slog[1200];
            spawn_tracked_logged(sargv, join ? "the Segment peer executor"
                                             : "the Segment origin executor",
                                 donor_log_path(sname, slog, sizeof slog));
            with_segment++;
        }
        double ram_gb = (double)segment_available / 1e9;
        const char *home = getenv("HOME") ? getenv("HOME") : ".";
        printf("  %smachine %s · %ld physical cores · %s · %.1f/%.1f GB RAM"
               " · %u GPU%s%s\n", C_DIM, serve_profile.hostname, cores,
               serve_profile.isa, serve_profile.ram_available_bytes / 1e9,
               serve_profile.ram_total_bytes / 1e9, serve_profile.gpu_count,
               serve_profile.gpu_count ? " detected" : "", C_R);
        printf("  %sSegment is preparing %d layer-aligned slices on ports %d-%d "
               "(%ld CPUs, %.1f GB RAM available, %d threads and %d sessions/slice). "
               "%s%s%s\n",
               C_DIM, chunks, port + 3, port + 2 + chunks, cores, ram_gb,
               slice_threads, sessions, join ? "Ordinary peers; " :
               "Replaceable fallback peers; ",
               advertise ? "direct data plane with relay fallback. " :
                           "relay data plane (no public port required). ",
               C_R);
        /* Their diagnostics belong in one file per slice: N unbuffered
         * writers on one terminal shred each other's lines exactly when
         * something has gone wrong and the line matters most. */
        printf("  %sslice logs: %s/.lumabri/logs/%s-segment-*.log · "
               "%s%s\n",
               C_DIM, home, host_base,
               advertise ? "allow Segment ports through the firewall for direct P2P"
                         : "NAT relay active: no need to open Segment ports",
               C_R);
    }
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("\n%sserving%s %s %s(host %s · tracker %s%s)%s\n", C_GRN, C_R,
           model, C_DIM, host_base, taddr,
           with_segment ? " · Segment origin active" :
           (with_exec ? " · executing experts for the swarm" : ""), C_R);
    /* Without --advertise every local peer uses its signed outbound tracker
     * tunnel for READ/EXEC/Segment. It works through NAT with no open data
     * ports, while --advertise retains the lower-latency direct preference. */
    if (!advertise && !join)
        printf("%s⚠ no --advertise: remote chatters will use the "
               "tracker relay for READ, EXEC and Segment.%s\n"
               "%s  This works behind NAT, but adds a hop and loads the "
               "tracker; --advertise enables direct P2P.%s\n"
               "%s  For direct connections: lumabri serve --model %s --advertise <public-ip>%s\n",
               C_RED, C_R, C_DIM, C_R, C_DIM, model, C_R);
    printf("%schat from this machine:   lumabri chat%s\n", C_DIM, C_R);
    printf("%schat from another one:    lumabri chat --tracker <this-ip>:%d%s\n\n",
           C_DIM, port, C_R);
    serve_watch_start(taddr, segment_model ? segment_model :
                      (mname && mname[0] ? mname : model));
    while (g_nchildren) {
        int status;
        pid_t p = wait(&status);
        if (p < 0 && errno == EINTR) continue;
        if (p < 0) break;
        int idx = -1;
        for (int i = 0; i < g_nchildren; i++) if (g_children[i] == p) idx = i;
        if (idx >= 0 && g_cargv[idx] && !g_stopping &&
            WIFEXITED(status) && WEXITSTATUS(status) == 3) {
            /* segment_node exit 3: the assigned range does not fit the RAM
             * budget. Restarting it every 5 s would only repeat the refusal
             * (and, before the check existed, reload a slice into a box that
             * is already full). The expert executor and storage stay up. */
            printf("\n%s%s cannot start: the slice exceeds the RAM budget of "
                   "this machine (see its log). It will not restart; "
                   "storage and experts remain active.%s\n",
                   C_RED, g_cwhat[idx], C_R);
            fflush(stdout);
        } else if (idx >= 0 && g_cargv[idx] && !g_stopping) {
            /* Losing a child silently is the expensive failure: with the
             * executor gone every chatter falls back to downloading experts
             * and nothing anywhere says why. Name it, and bring it back. */
            if (WIFSIGNALED(status))
                printf("\n%s⚠ %s was killed (signal %d)%s\n",
                       C_RED, g_cwhat[idx], WTERMSIG(status), C_R);
            else
                printf("\n%s⚠ %s exited (code %d)%s\n",
                       C_RED, g_cwhat[idx], WEXITSTATUS(status), C_R);
            /* Backoff: a child that dies right after starting (OOM, a port
             * taken, a corrupt model) used to be relaunched every 5 s
             * forever, reloading gigabytes into a box that is already
             * full. The delay doubles up to two minutes and resets once the
             * child has lived ten minutes. */
            double alive = nowd() - g_cstarted[idx];
            if (alive >= 600.0) g_crestarts[idx] = 0;
            int delay = 5 << (g_crestarts[idx] < 5 ? g_crestarts[idx] : 5);
            if (delay > 120) delay = 120;
            g_crestarts[idx]++;
            printf("  %srestarting in %d s — while it is unavailable, chatters "
                   "download expert weights instead of delegating execution%s\n",
                   C_DIM, delay, C_R);
            fflush(stdout);
            sleep((unsigned)delay);
            g_cstarted[idx] = nowd();
            pid_t np = g_clog[idx]
                ? spawn_argv_logged(g_cargv[idx], NULL, g_clog[idx])
                : spawn_argv(g_cargv[idx]);
            child_publish(idx, np);
            printf("  %s%s restarted%s\n", C_DIM, g_cwhat[idx], C_R);
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
        (void)lmb_json_string(cfg, "model_type", s->model_type,
                              sizeof s->model_type);
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

typedef struct {
    char model[64];
    uint64_t calls;
    uint32_t in_flight, age_s;
    int have_stats;
} ExecSwarmRow;

typedef struct {
    char name[64], model[64];
    uint32_t roles, age_s;
    uint64_t held_bytes, served_bytes, served_reads;
    uint32_t nfiles, nexperts, have_exec_stats;
    uint64_t exec_calls;
    uint32_t exec_inflight;
    uint32_t expert_state, expert_resident_flags, resident_experts;
    uint64_t expert_resident_bytes, expert_vram_bytes;
    uint32_t layer_begin, layer_end;
    uint32_t active_sessions, max_sessions, segment_queue, segment_inflight;
    uint32_t segment_flags;
} SwarmDetailRow;

static int swarm_detail(const char *tracker, SwarmDetailRow *rows, int cap) {
    LmbMsg message = {0};
    if (lmb_request(tracker, LMB_SWARM_DETAIL, NULL, 0, &message)) return -1;
    if (message.op != LMB_SWARM_DETAIL_R || message.pay_len) {
        lmb_msg_free(&message); return -2;
    }
    LmbCur cursor = { message.body, message.body_len, 0 };
    uint32_t version = 0, count = 0;
    if (lmb_cur_u32(&cursor, &version) ||
        version != LMB_SWARM_DETAIL_VERSION ||
        lmb_cur_u32(&cursor, &count) || count > 4096) {
        lmb_msg_free(&message); return -2;
    }
    int out = 0;
    for (uint32_t index = 0; index < count; index++) {
        SwarmDetailRow row = {0};
        int bad = lmb_cur_str(&cursor, row.name, sizeof row.name) ||
                  lmb_cur_str(&cursor, row.model, sizeof row.model) ||
                  lmb_cur_u32(&cursor, &row.roles) ||
                  lmb_cur_u32(&cursor, &row.age_s) ||
                  lmb_cur_u64(&cursor, &row.held_bytes) ||
                  lmb_cur_u64(&cursor, &row.served_bytes) ||
                  lmb_cur_u64(&cursor, &row.served_reads) ||
                  lmb_cur_u32(&cursor, &row.nfiles) ||
                  lmb_cur_u32(&cursor, &row.nexperts) ||
                  lmb_cur_u32(&cursor, &row.have_exec_stats) ||
                  lmb_cur_u64(&cursor, &row.exec_calls) ||
                  lmb_cur_u32(&cursor, &row.exec_inflight) ||
                  lmb_cur_u32(&cursor, &row.expert_state) ||
                  lmb_cur_u32(&cursor, &row.expert_resident_flags) ||
                  lmb_cur_u32(&cursor, &row.resident_experts) ||
                  lmb_cur_u64(&cursor, &row.expert_resident_bytes) ||
                  lmb_cur_u64(&cursor, &row.expert_vram_bytes) ||
                  lmb_cur_u32(&cursor, &row.layer_begin) ||
                  lmb_cur_u32(&cursor, &row.layer_end) ||
                  lmb_cur_u32(&cursor, &row.active_sessions) ||
                  lmb_cur_u32(&cursor, &row.max_sessions) ||
                  lmb_cur_u32(&cursor, &row.segment_queue) ||
                  lmb_cur_u32(&cursor, &row.segment_inflight) ||
                  lmb_cur_u32(&cursor, &row.segment_flags);
        if (bad || !row.name[0] || !row.model[0] ||
            (row.roles & ~(LMB_SWARM_ROLE_STORAGE | LMB_SWARM_ROLE_EXPERT |
                           LMB_SWARM_ROLE_SEGMENT)) ||
            row.have_exec_stats > 1u) {
            lmb_msg_free(&message); return -2;
        }
        if (out < cap) rows[out++] = row;
    }
    int malformed = cursor.off != cursor.len;
    lmb_msg_free(&message);
    return malformed ? -2 : out;
}

typedef struct { char tracker[80], model[64]; } ServeWatch;

static void *serve_watch_thread(void *opaque) {
    ServeWatch *watch = opaque;
    int last_hosts = -1, last_storage = -1, last_experts = -1, last_segments = -1;
    uint64_t last_calls = UINT64_MAX;
    unsigned tick = 0;
    while (!g_stopping) {
        for (int part = 0; part < 10 && !g_stopping; part++) sleep(1);
        if (g_stopping) break;
        SwarmDetailRow rows[64];
        int n = swarm_detail(watch->tracker, rows, 64);
        if (n < 0) {
            if ((tick++ % 3u) == 0)
                printf("%s[swarm] tracker unreachable; nodes will keep "
                       "retrying%s\n", C_DIM, C_R);
            continue;
        }
        int hosts = 0, storage = 0, experts = 0, segments = 0;
        uint32_t sessions = 0, segment_inflight = 0, expert_inflight = 0;
        uint64_t calls = 0;
        for (int i = 0; i < n; i++) {
            if (watch->model[0] && strcmp(rows[i].model, watch->model)) continue;
            hosts++;
            storage += !!(rows[i].roles & LMB_SWARM_ROLE_STORAGE);
            experts += !!(rows[i].roles & LMB_SWARM_ROLE_EXPERT);
            segments += !!(rows[i].roles & LMB_SWARM_ROLE_SEGMENT);
            calls += rows[i].have_exec_stats ? rows[i].exec_calls : 0;
            expert_inflight += rows[i].exec_inflight;
            sessions += rows[i].active_sessions;
            segment_inflight += rows[i].segment_inflight;
        }
        int changed = hosts != last_hosts || storage != last_storage ||
                      experts != last_experts || segments != last_segments ||
                      calls != last_calls || expert_inflight || segment_inflight;
        if (changed || (++tick % 6u) == 0) {
            printf("%s[swarm]%s %d connected nodes · %d storage · %d expert "
                   "(%llu calls, %u active) · %d Segment "
                   "(%u sessions, %u run active)\n",
                   C_CORAL, C_R, hosts, storage, experts,
                   (unsigned long long)calls, expert_inflight, segments,
                   sessions, segment_inflight);
            fflush(stdout);
        }
        last_hosts = hosts; last_storage = storage; last_experts = experts;
        last_segments = segments; last_calls = calls;
    }
    free(watch);
    return NULL;
}

static void serve_watch_start(const char *tracker, const char *model) {
    ServeWatch *watch = calloc(1, sizeof *watch);
    if (!watch) return;
    if (checked_printf(watch->tracker, sizeof watch->tracker, "%s", tracker) ||
        checked_printf(watch->model, sizeof watch->model, "%s", model)) {
        free(watch); return;
    }
    pthread_t thread;
    if (!pthread_create(&thread, NULL, serve_watch_thread, watch))
        pthread_detach(thread);
    else free(watch);
}

static int swarm_stats(const char *tracker, SwarmRow *rows, int cap,
                       ExecSwarmRow *exec_rows, int exec_cap, int *exec_out) {
    LmbMsg m = {0};
    if (exec_out) *exec_out = 0;
    if (lmb_request(tracker, LMB_SWARM, NULL, 0, &m)) return -1;
    if (m.op != LMB_SWARM_R) { lmb_msg_free(&m); return -2; }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) { lmb_msg_free(&m); return -2; }
    int out = 0;
    /* Consume every legacy row even when the caller's output array is full;
     * the optional executor suffix starts only after the complete prefix. */
    for (uint32_t i = 0; i < n; i++) {
        SwarmRow r = {0};
        if (lmb_cur_str(&c, r.model, sizeof r.model) ||
            lmb_cur_u64(&c, &r.held) || lmb_cur_u64(&c, &r.served_bytes) ||
            lmb_cur_u64(&c, &r.served_reads) || lmb_cur_u32(&c, &r.age_s) ||
            lmb_cur_u32(&c, &r.nfiles)) {
            lmb_msg_free(&m); return -2;
        }
        if (out < cap) rows[out++] = r;
    }
    int eout = 0, malformed = 0;
    if (c.off < c.len) {
        uint32_t magic = 0, version = 0, len = 0;
        LmbCur suffix = c;
        if (lmb_cur_u32(&suffix, &magic) || magic != LMB_SWARM_EXEC_MAGIC ||
            lmb_cur_u32(&suffix, &version) || lmb_cur_u32(&suffix, &len) ||
            len != suffix.len - suffix.off) {
            malformed = 1;
        } else if (version == LMB_SWARM_EXEC_VERSION) {
            LmbCur section = { suffix.p + suffix.off, len, 0 };
            uint32_t en = 0;
            int valid = !lmb_cur_u32(&section, &en) && en <= 4096;
            for (uint32_t i = 0; valid && i < en; i++) {
                ExecSwarmRow r = {0};
                uint32_t have = 0;
                valid = !lmb_cur_str(&section, r.model, sizeof r.model) &&
                        !lmb_cur_u32(&section, &have) && have <= 1 &&
                        !lmb_cur_u64(&section, &r.calls) &&
                        !lmb_cur_u32(&section, &r.in_flight) &&
                        !lmb_cur_u32(&section, &r.age_s);
                r.have_stats = have != 0;
                if (valid && eout < exec_cap) exec_rows[eout++] = r;
            }
            if (!valid || section.off != section.len) malformed = 1;
        }
    }
    if (malformed) eout = 0;
    if (exec_out) *exec_out = eout;
    lmb_msg_free(&m);
    return malformed ? -2 : out;
}

static void render_named_swarm(const SwarmDetailRow *rows, int n) {
    int storage = 0, experts = 0, segments = 0;
    for (int i = 0; i < n; i++) {
        storage += !!(rows[i].roles & LMB_SWARM_ROLE_STORAGE);
        experts += !!(rows[i].roles & LMB_SWARM_ROLE_EXPERT);
        segments += !!(rows[i].roles & LMB_SWARM_ROLE_SEGMENT);
    }
    printf("\n  %s%slive swarm%s  %s%d nodes · %d storage · %d expert · "
           "%d Segment%s\n", C_BOLD, C_CORAL, C_R, C_DIM, n, storage,
           experts, segments, C_R);
    for (int i = 0; i < n; i++) {
        const SwarmDetailRow *row = &rows[i];
        printf("  %s%-28.28s%s  %-16.16s  ", C_BOLD, row->name, C_R,
               row->model);
        int separator = 0;
        if (row->roles & LMB_SWARM_ROLE_STORAGE) {
            printf("storage %.1f GB · %.0f MB/%llu req",
                   (double)row->held_bytes / 1e9,
                   (double)row->served_bytes / 1e6,
                   (unsigned long long)row->served_reads);
            separator = 1;
        }
        if (row->roles & LMB_SWARM_ROLE_EXPERT) {
            if (separator) printf(" | ");
            printf("%u expert", row->nexperts);
            if (row->expert_resident_flags & LMB_EXPERT_RESIDENT_RAM)
                printf(" · %u RAM-ready (%.1f GB)", row->resident_experts,
                       (double)row->expert_resident_bytes / 1e9);
            else if (row->expert_resident_flags & LMB_EXPERT_RESIDENT_VRAM)
                printf(" · %u VRAM-ready (%.1f GB)", row->resident_experts,
                       (double)row->expert_vram_bytes / 1e9);
            else if (row->expert_resident_flags & LMB_EXPERT_DISK_FALLBACK)
                printf(" · disk fallback");
            if (row->have_exec_stats)
                printf(" · %llu call · %u active",
                       (unsigned long long)row->exec_calls,
                       row->exec_inflight);
            separator = 1;
        }
        if (row->roles & LMB_SWARM_ROLE_SEGMENT) {
            if (separator) printf(" | ");
            printf("layer %u:%u · sessions %u/%u · %u active%s",
                   row->layer_begin, row->layer_end,
                   row->active_sessions, row->max_sessions,
                   row->segment_inflight,
                   row->segment_flags & LMB_SEG_ADVERT_RELAY_ONLY
                       ? " · relay" : " · direct");
        }
        printf(" %s· hb %us%s\n", C_DIM, row->age_s, C_R);
    }
}

/* /swarm: prefer operator-chosen host names and real role/load counters.
 * Fall back to the anonymous legacy view when talking to an older tracker. */
static void render_swarm(const char *tracker) {
    SwarmDetailRow detail[64];
    int nd = swarm_detail(tracker, detail, 64);
    if (nd >= 0) { render_named_swarm(detail, nd); return; }
    SwarmRow rows[64];
    ExecSwarmRow exec_rows[64];
    int ne = 0;
    int n = swarm_stats(tracker, rows, 64, exec_rows, 64, &ne);
    if (n == -1) { printf("  %stracker unreachable%s\n", C_RED, C_R); return; }
    if (n < 0) { printf("  %smalformed swarm response%s\n", C_RED, C_R); return; }
    char lines[129][256];
    int nl = 0;
    snprintf(lines[nl++], sizeof lines[0],
             "%s%snetwork now%s  %s%d live peers \xc2\xb7 %d exec%s",
             C_BOLD, C_CORAL, C_R, C_DIM, n, ne, C_R);
    for (int i = 0; i < n; i++)
        snprintf(lines[nl++], sizeof lines[0],
                 "%speer-%d%s  %-12.12s %5.1f GB  %s%.0f MB out \xc2\xb7 %llu req \xc2\xb7 hb %us%s",
                 C_BOLD, i + 1, C_R, rows[i].model, (double)rows[i].held / 1e9,
                 C_DIM, (double)rows[i].served_bytes / 1e6,
                 (unsigned long long)rows[i].served_reads, rows[i].age_s, C_R);
    for (int i = 0; i < ne; i++) {
        if (exec_rows[i].have_stats)
            snprintf(lines[nl++], sizeof lines[0],
                     "%sexec-%d%s  %-12.12s %s%llu calls \xc2\xb7 %u in flight \xc2\xb7 hb %us%s",
                     C_BOLD, i + 1, C_R, exec_rows[i].model, C_DIM,
                     (unsigned long long)exec_rows[i].calls,
                     exec_rows[i].in_flight, exec_rows[i].age_s, C_R);
        else
            snprintf(lines[nl++], sizeof lines[0],
                     "%sexec-%d%s  %-12.12s %sstats unavailable \xc2\xb7 hb %us%s",
                     C_BOLD, i + 1, C_R, exec_rows[i].model, C_DIM,
                     exec_rows[i].age_s, C_R);
    }
    /* the box fits its widest line; the terminal caps it */
    int w = 0;
    for (int i = 0; i < nl; i++)
        if (vis_len(lines[i]) + 6 > w) w = vis_len(lines[i]) + 6;
    if (w > term_w() - 2) w = term_w() - 2;
    printf("\n");
    hline("\xe2\x95\xad", "\xe2\x95\xae", w);
    for (int i = 0; i < nl; i++) {
        int pad = w - 4 - vis_len(lines[i]);
        printf("%s\xe2\x94\x82%s  %s%*s%s\xe2\x94\x82%s\n", C_GRAY, C_R, lines[i],
               pad > 0 ? pad : 0, "", C_GRAY, C_R);
    }
    hline("\xe2\x95\xb0", "\xe2\x95\xaf", w);
}

/* /experts is the compact answer to "are my donated experts actually being
 * used?". Names are stable and human-readable, so a user can recognize this
 * machine without exposing its address. Segment ranges are included because
 * they execute the same MoE blocks without issuing classic EXEC calls. */
static void render_experts(const char *tracker) {
    SwarmDetailRow rows[64];
    int n = swarm_detail(tracker, rows, 64);
    if (n == -1) { printf("  %stracker unreachable%s\n", C_RED, C_R); return; }
    if (n < 0) {
        printf("  %sthe tracker does not expose named counters yet%s\n",
               C_DIM, C_R); return;
    }
    int shown = 0;
    uint64_t calls = 0;
    printf("\n  %s%sexecutor activity%s\n", C_BOLD, C_CORAL, C_R);
    for (int i = 0; i < n; i++) {
        SwarmDetailRow *row = &rows[i];
        if (!(row->roles & (LMB_SWARM_ROLE_EXPERT |
                            LMB_SWARM_ROLE_SEGMENT))) continue;
        shown++;
        printf("  %s%-28.28s%s  ", C_BOLD, row->name, C_R);
        if (row->roles & LMB_SWARM_ROLE_EXPERT) {
            if (row->have_exec_stats) {
                printf("%llu expert calls · %u in flight",
                       (unsigned long long)row->exec_calls,
                       row->exec_inflight);
                calls += row->exec_calls;
            } else printf("%u expert · counter pending", row->nexperts);
            if (row->expert_resident_flags & LMB_EXPERT_RESIDENT_RAM)
                printf(" · %u RAM-resident (%.1f GB)", row->resident_experts,
                       (double)row->expert_resident_bytes / 1e9);
            else if (row->expert_resident_flags & LMB_EXPERT_RESIDENT_VRAM)
                printf(" · %u VRAM-resident (%.1f GB)", row->resident_experts,
                       (double)row->expert_vram_bytes / 1e9);
            else if (row->expert_resident_flags & LMB_EXPERT_DISK_FALLBACK)
                printf(" · disk fallback");
        }
        if (row->roles & LMB_SWARM_ROLE_SEGMENT) {
            if (row->roles & LMB_SWARM_ROLE_EXPERT) printf(" | ");
            printf("Segment %u:%u · sessions %u/%u · %u run in flight",
                   row->layer_begin, row->layer_end,
                   row->active_sessions, row->max_sessions,
                   row->segment_inflight);
        }
        printf("\n");
    }
    if (!shown) printf("  %sno executors connected%s\n", C_DIM, C_R);
    else printf("  %sclassic total: %llu completed expert calls%s\n",
                C_DIM, (unsigned long long)calls, C_R);
}

/* distinct model names on the swarm; returns count */
static int swarm_models(const char *tracker, char names[][64], int cap) {
    SwarmRow rows[64];
    /* One lost packet at startup used to print "no swarm" and exit while
     * the tracker was answering everyone else. Ask a few times first. */
    int n = -1, out = 0;
    for (int attempt = 0; attempt < 4 && n < 0; attempt++) {
        if (attempt) sleep(1);
        n = swarm_stats(tracker, rows, 64, NULL, 0, NULL);
    }
    if (n < 0) return 0;
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
    volatile double dense_mb, dense_total_mb, dense_rate; /* the dense warm-up */
    volatile int    dense_ready;     /* 1 ready · 2 incomplete (engine will retry) */
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
    /* LUMABRI_ENGINE_LOG=path keeps every engine line, not just the tail
     * /debug shows: the only way to read a slow reply after the fact. */
    FILE *keep = getenv("LUMABRI_ENGINE_LOG") ? fopen(getenv("LUMABRI_ENGINE_LOG"), "a") : NULL;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (keep) { fputs(line, keep); fputc('\n', keep); fflush(keep); }
        tail_push(line);
        g_eng.last_out = nowd();

        /* Route detail remains available under /debug, but must never race
         * the readiness handshake and land after the first TUI prompt. */
        if (!strncmp(line, "[segment-route]", 15)) continue;

        double mb, rate, gb, pct;
        double d1, d2, d3;
        if (sscanf(line, "[lumabri] dense %lf/%lf MB (%lf MB/s)", &d1, &d2, &d3) == 3) {
            g_eng.dense_mb = d1; g_eng.dense_total_mb = d2; g_eng.dense_rate = d3;
        } else if (!strncmp(line, "[lumabri] dense ready", 21)) {
            g_eng.dense_ready = 1;
        } else if (!strncmp(line, "[lumabri] dense warm-up incomplete", 34)) {
            g_eng.dense_ready = 2;
        }
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
    if (!f) { printf("    %s(nothing yet)%s\n", C_DIM, C_R); return; }
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
    if (!n) printf("    %s(nothing yet)%s\n", C_DIM, C_R);
}

/* /storage: where the disk went. The mirror is a cache — every byte is
 * re-fetchable from the swarm — but it grew silently for long enough that
 * "perché ho 92 GB in più?" became a reasonable question. Show it, and
 * show the one command that frees it. */
static uint64_t du_bytes(const char *path, int depth) {
    if (depth > 6) return 0;
    struct stat st;
    /* stat, not lstat: a checkpoint fetched with huggingface_hub is a
     * directory of symlinks into its blob cache, and counting the links
     * made a 167 GB model weigh 0 bytes — which silently disabled the
     * "does this Segment range fit" check on the origin. The depth cap
     * above still bounds a symlink loop. */
    if (stat(path, &st)) return 0;
    if (S_ISREG(st.st_mode)) return (uint64_t)st.st_size;
    if (!S_ISDIR(st.st_mode)) return 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    uint64_t tot = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        tot += du_bytes(p, depth + 1);
    }
    closedir(d);
    return tot;
}

static void render_storage(void) {
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char root[1100];
    snprintf(root, sizeof root, "%s/.lumabri", home);
    DIR *d = opendir(root);
    if (!d) { printf("  %snothing in %s%s\n", C_DIM, root, C_R); return; }
    printf("  %sLumabri disk usage in %s:%s\n", C_DIM, root, C_R);
    struct dirent *e;
    uint64_t tot = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%.1099s/%.255s", root, e->d_name);
        uint64_t b = du_bytes(p, 0);
        tot += b;
        if (b < (1u << 20)) continue;      /* chiavi e log sotto il MB: rumore */
        printf("    %s%-24s%s %8.1f GB\n", C_BOLD, e->d_name, C_R, (double)b / 1e9);
    }
    closedir(d);
    printf("  %stotal %.1f GB · the mirror is a cache: weights can be "
           "fetched again while their sources remain available.%s\n"
           "  %sinspect before removing a model cache: %s/<model>/cache%s\n",
           C_DIM, (double)tot / 1e9, C_R, C_DIM, root, C_R);
}

static void render_debug(void) {
    printf("  %srecent engine output:%s\n", C_DIM, C_R);
    tail_dump(15);
    for (int i = 0; i < g_ndonor_logs; i++) {
        printf("  %s%s%s\n", C_DIM, g_donor_logs[i], C_R);
        tail_file(g_donor_logs[i], 8);
    }
    if (!g_ndonor_logs)
        printf("  %sno donors in this session%s\n", C_DIM, C_R);
}

static void render_help(void) {
    printf("\n  %s%scommands%s\n", C_BOLD, C_CORAL, C_R);
    printf("  %-12s named host, storage, expert and Segment status\n", "/swarm");
    printf("  %-12s expert calls and Segment sessions\n", "/experts");
    printf("  %-12s compact alias for /swarm\n", "/hosts");
    printf("  %-12s list or switch models (hosted: current model only)\n", "/model");
    printf("  %-12s engine and donor diagnostics\n", "/debug");
    printf("  %-12s approved household layer allocation\n", "/plan");
    printf("  %-12s mirror and CAS disk usage\n", "/storage");
    printf("  %-12s new conversation\n", "/reset");
    printf("  %-12s close chat\n", "/quit");
    printf("  %sTab completes commands. During inference you can open these "
           "panels or prepare your next prompt.%s\n", C_DIM, C_R);
}

static int le_prev(const char *s, int pos);
static int le_next(const char *s, int len, int pos);

/* During inference the response owns the scrolling part of the terminal and
 * these three bottom rows remain stable: separator, live phase, next input.
 * A tiny raw-input worker lets read-only menus open immediately and queues one
 * next prompt without ever issuing two requests against the same KV session. */
typedef struct {
    pthread_mutex_t lock;
    volatile int active;
    int enabled, rows, cols, raw_set;
    struct termios old_term, raw_term;
    pthread_t thread;
    char tracker[80], model[64], phase[160], notice[160];
    char swarm[200];              /* who is doing the work, refreshed live */
    pthread_t swarm_thread; int swarm_started;
    char input[4096]; int input_len, input_pos;
    char pending[4096]; int pending_ready;
    char completion[64]; int completion_next;
    int output_col;
    mbstate_t output_utf8;
    char output_bytes[MB_LEN_MAX]; size_t output_len;
} LiveUi;

static LiveUi g_live = { .lock = PTHREAD_MUTEX_INITIALIZER };
static int g_live_atexit_registered;

/* Best-effort last line of defence for normal exit() paths.  Fatal signals
 * are routed through on_sigint(), which wakes the main loop and reaches
 * live_end(); SIGKILL is intentionally not claimed because no process can
 * run cleanup after it. */
static void live_atexit_restore(void) {
    if (g_live.raw_set)
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_live.old_term);
    if (g_live.enabled) {
        static const char reset_scroll[] = "\x1b[r";
        ssize_t ignored = write(STDOUT_FILENO, reset_scroll,
                                sizeof reset_scroll - 1);
        (void)ignored;
    }
    g_live.raw_set = 0;
}

static void live_draw_locked(void) {
    if (!g_live.enabled) return;
    int width = g_live.cols > 20 ? g_live.cols : 80;
    printf("\x1b" "7");
    printf("\x1b[%d;1H\x1b[2K%s", g_live.rows - 2, C_GRAY);
    int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
    int matches = chat_command_matches(g_live.input, indices);
    if (matches) {
        int selected = indices[g_live.completion_next % matches];
        printf(" %s %.*s · ↑↓ / Tab", CHAT_COMMANDS[selected], width > 45 ? width - 45 : 1,
               CHAT_COMMAND_HELP[selected]);
    } else for (int i = 0; i < width; i++) fputs("\xe2\x94\x80", stdout);
    printf("%s", C_R);
    printf("\x1b[%d;1H\x1b[2K %s\xe2\x9c\xa6%s %.*s", g_live.rows - 1,
           C_CORAL, C_R, width > 8 ? width - 8 : 12,
           g_live.phase[0] ? g_live.phase : "processing");
    if (g_live.swarm[0])
        printf(" %s\xc2\xb7 %.*s%s", C_DIM, width > 40 ? width / 2 : 12,
               g_live.swarm, C_R);
    if (g_live.notice[0])
        printf(" %s\xc2\xb7 %.*s%s", C_DIM, width > 30 ? width / 2 : 12,
               g_live.notice, C_R);
    printf("\x1b[%d;1H\x1b[2K %s%s\xe2\x80\xba%s ", g_live.rows,
           C_CORAL, C_BOLD, C_R);
    int room = width - 5;
    const char *shown = g_live.input;
    int bytes = g_live.input_len;
    if (bytes > room) { shown += bytes - room; bytes = room; }
    if (bytes > 0) fwrite(shown, 1, (size_t)bytes, stdout);
    printf("\x1b" "8");
    fflush(stdout);
}

static void live_status(const char *fmt, ...) {
    if (!g_live.enabled) return;
    pthread_mutex_lock(&g_live.lock);
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_live.phase, sizeof g_live.phase, fmt, ap);
    va_end(ap);
    live_draw_locked();
    pthread_mutex_unlock(&g_live.lock);
}

static void live_write(const void *data, size_t bytes) {
    if (!g_live.enabled) {
        if (bytes) fwrite(data, 1, bytes, stdout);
        fflush(stdout);
        return;
    }
    pthread_mutex_lock(&g_live.lock);
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; i++) {
        wchar_t glyph;
        char byte = (char)p[i];
        if (g_live.output_len >= sizeof g_live.output_bytes) {
            memset(&g_live.output_utf8, 0, sizeof g_live.output_utf8);
            g_live.output_len = 0;
        }
        g_live.output_bytes[g_live.output_len++] = byte;
        size_t decoded = mbrtowc(&glyph, &byte, 1, &g_live.output_utf8);
        if (decoded == (size_t)-2) continue;
        if (decoded == (size_t)-1) {
            memset(&g_live.output_utf8, 0, sizeof g_live.output_utf8);
            glyph = L'?'; g_live.output_bytes[0] = '?'; g_live.output_len = 1;
        }
        int columns = glyph == L'\t' ? 4 : wcwidth(glyph);
        if (glyph == L'\n' || (columns > 0 && g_live.output_col + columns > g_live.cols - 1)) {
            /* Scroll the WHOLE terminal by writing a newline at its bottom.
             * A partial DECSTBM region discards transcript lines in several
             * terminals instead of preserving them in normal scrollback. */
            printf("\x1b[%d;1H\r\n\x1b[%d;1H\x1b[2K", g_live.rows, g_live.rows - 3);
            g_live.output_col = 0;
            live_draw_locked();
        }
        if (glyph == L'\t') { fputs("    ", stdout); g_live.output_col += 4; }
        else if (columns >= 0 && glyph != L'\n' && glyph != L'\r' && glyph != 0) {
            fwrite(g_live.output_bytes, 1, g_live.output_len, stdout);
            g_live.output_col += columns;
        }
        g_live.output_len = 0;
    }
    fflush(stdout);
    pthread_mutex_unlock(&g_live.lock);
}

static void live_clear_input_locked(void) {
    g_live.input[0] = 0; g_live.input_len = 0; g_live.input_pos = 0;
    g_live.completion[0] = 0; g_live.completion_next = 0;
}

static void live_complete_locked(void) {
    int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
    int matches = chat_command_matches(g_live.input, indices);
    if (!matches) return;
    int wanted = indices[g_live.completion_next % matches];
    snprintf(g_live.input, sizeof g_live.input, "%s", CHAT_COMMANDS[wanted]);
    g_live.input_len = g_live.input_pos = (int)strlen(g_live.input);
    g_live.completion_next = 0;
}

static int live_immediate_command_locked(const char *command) {
    if (strcmp(command, "/swarm") && strcmp(command, "/hosts") &&
        strcmp(command, "/experts") && strcmp(command, "/debug") &&
        strcmp(command, "/storage") && strcmp(command, "/help") &&
        strcmp(command, "/plan")) return 0;
    putchar('\n');
    if (!strcmp(command, "/swarm") || !strcmp(command, "/hosts"))
        render_swarm(g_live.tracker);
    else if (!strcmp(command, "/experts")) render_experts(g_live.tracker);
    else if (!strcmp(command, "/debug")) render_debug();
    else if (!strcmp(command, "/storage")) render_storage();
    else if (!strcmp(command, "/plan")) lmb_execution_print(stdout, g_execution_view);
    else render_help();
    printf("\x1b[%d;1H\r\n\r\n\r\n\r\n\x1b[%d;1H",
           g_live.rows, g_live.rows - 3);
    g_live.output_col = 0;
    live_clear_input_locked();
    snprintf(g_live.notice, sizeof g_live.notice, "inference still running");
    live_draw_locked();
    return 1;
}

static void live_submit_input_locked(void) {
    if (!g_live.input_len) return;
    if (live_immediate_command_locked(g_live.input)) return;
    if (!g_live.pending_ready) {
        snprintf(g_live.pending, sizeof g_live.pending, "%s", g_live.input);
        g_live.pending_ready = 1;
        snprintf(g_live.notice, sizeof g_live.notice,
                 g_live.input[0] == '/' ? "command queued" :
                                          "next prompt ready");
    } else {
        snprintf(g_live.notice, sizeof g_live.notice,
                 "a prompt is already queued");
    }
    live_clear_input_locked();
    live_draw_locked();
}

/* Called from the input worker before requesting shutdown.  The engine may be
 * the very thing that is wedged, so terminal recovery cannot depend on the
 * main thread first escaping its pipe read. */
static void live_release_terminal_locked(void) {
    if (g_live.enabled) {
        printf("\x1b[r\x1b[%d;1H\x1b[2K\x1b[%d;1H\x1b[2K"
               "\x1b[%d;1H\x1b[2K", g_live.rows - 2, g_live.rows - 1,
               g_live.rows);
        fflush(stdout);
    }
    if (g_live.raw_set)
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_live.old_term);
    g_live.raw_set = 0;
    g_live.enabled = 0;
}

/* Ctrl-Z is read as a byte while the dock owns raw input.  Restore canonical
 * mode and the normal scrolling region before stopping, then reconstruct the
 * dock after SIGCONT.  This is ordinary thread context, not a signal handler,
 * so tcsetattr and stdio are safe here. */
static void live_suspend_locked(void) {
    if (g_live.enabled) {
        printf("\x1b[r\x1b[%d;1H\x1b[2K\r\n", g_live.rows);
        fflush(stdout);
    }
    if (g_live.raw_set) {
        struct termios shell_term = g_live.old_term;
        /* An interactive shell may hand a foreground job a cbreak-flavoured
         * snapshot during the editor-to-dock transition.  Job control needs
         * a genuinely usable terminal, not merely that snapshot: guarantee
         * the conventional signal/canonical/echo controls while stopped. */
        shell_term.c_lflag |= (tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
        shell_term.c_iflag |= (tcflag_t)IXON;
        shell_term.c_cc[VMIN] = 1; shell_term.c_cc[VTIME] = 0;
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &shell_term);
    }
    g_live.raw_set = 0;
    raise(SIGTSTP);
    if (!g_live.active || g_stopping) return;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &g_live.raw_term)) return;
    g_live.raw_set = 1;
    g_live.rows = term_h(); g_live.cols = term_w();
    g_live.enabled = g_live.rows >= 8;
    if (g_live.enabled) {
        printf("\x1b[r\x1b[%d;1H\r\n\r\n\r\n\r\n\x1b[%d;1H",
               g_live.rows, g_live.rows - 3);
        g_live.output_col = 0;
    }
    live_draw_locked();
}

static void *live_input_thread(void *unused) {
    (void)unused;
    while (g_live.active && !g_stopping) {
        struct pollfd pollfd = { STDIN_FILENO, POLLIN, 0 };
        int ready = poll(&pollfd, 1, 100);
        if (ready <= 0 || !(pollfd.revents & POLLIN)) continue;
        unsigned char c = 0;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;
        pthread_mutex_lock(&g_live.lock);
        if (c == '\r' || c == '\n') {
            live_submit_input_locked();
        } else if (c == '\t') {
            live_complete_locked(); live_draw_locked();
        } else if (c == 3) {
            snprintf(g_live.notice, sizeof g_live.notice,
                     "stopping inference and exiting");
            live_draw_locked();
            live_release_terminal_locked();
            on_sigint(SIGINT);
        } else if (c == 26) {
            live_suspend_locked();
        } else if (c == 28) {
            snprintf(g_live.notice, sizeof g_live.notice,
                     "SIGQUIT: stopping inference and exiting");
            live_draw_locked();
            live_release_terminal_locked();
            /* ISIG is disabled only while this worker owns the terminal; emit
             * the conventional signal explicitly instead of swallowing ^\ . */
            raise(SIGQUIT);
        } else if (c == 21) {
            live_clear_input_locked(); live_draw_locked();
        } else if (c == 127 || c == 8) {
            if (g_live.input_pos > 0) {
                int previous = le_prev(g_live.input, g_live.input_pos);
                memmove(g_live.input + previous, g_live.input + g_live.input_pos,
                        (size_t)(g_live.input_len - g_live.input_pos + 1));
                g_live.input_len -= g_live.input_pos - previous;
                g_live.input_pos = previous;
            }
            g_live.completion[0] = 0; live_draw_locked();
        } else if (c == 27) {
            unsigned char a = 0, b = 0;
            ssize_t ar = read(STDIN_FILENO, &a, 1);
            ssize_t br = ar == 1 ? read(STDIN_FILENO, &b, 1) : -1;
            if (ar != 1 || br != 1) { pthread_mutex_unlock(&g_live.lock); continue; }
            int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
            int matches = chat_command_matches(g_live.input, indices);
            if ((a == '[' || a == 'O') && (b == 'A' || b == 'B') && matches)
                g_live.completion_next = (g_live.completion_next + (b == 'A' ? matches - 1 : 1)) % matches;
            if ((a == '[' || a == 'O') && b == 'D' && g_live.input_pos > 0)
                g_live.input_pos = le_prev(g_live.input, g_live.input_pos);
            else if ((a == '[' || a == 'O') && b == 'C' &&
                     g_live.input_pos < g_live.input_len)
                g_live.input_pos = le_next(g_live.input, g_live.input_len,
                                           g_live.input_pos);
            live_draw_locked();
        } else if (c >= 0x20 && g_live.input_len + 1 < (int)sizeof g_live.input) {
            unsigned char bytes[4] = {c, 0, 0, 0};
            int count = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
            for (int i = 1; i < count; i++)
                if (read(STDIN_FILENO, &bytes[i], 1) != 1) { count = i; break; }
            if (g_live.input_len + count < (int)sizeof g_live.input) {
                memmove(g_live.input + g_live.input_pos + count,
                        g_live.input + g_live.input_pos,
                        (size_t)(g_live.input_len - g_live.input_pos + 1));
                memcpy(g_live.input + g_live.input_pos, bytes, (size_t)count);
                g_live.input_pos += count; g_live.input_len += count;
                g_live.completion[0] = 0;
            }
            live_draw_locked();
        }
        pthread_mutex_unlock(&g_live.lock);
    }
    return NULL;
}

/* What the swarm is doing for THIS reply: expert calls since it began, the
 * share that went to donors (names start with "donor-"), the busiest
 * executor, and the bytes storage donors served. Read from the tracker's
 * nominative counters every two seconds on its own thread, so the token
 * stream never waits on a network round trip. */
static void *live_swarm_thread(void *arg) {
    (void)arg;
    SwarmDetailRow base[64];
    int nb = swarm_detail(g_live.tracker, base, 64);
    if (nb < 0) nb = 0;
    while (g_live.active) {
        for (int i = 0; i < 20 && g_live.active; i++) usleep(100000);
        if (!g_live.active) break;
        SwarmDetailRow now[64];
        int n = swarm_detail(g_live.tracker, now, 64);
        if (n <= 0) continue;
        unsigned long long calls = 0, donor_calls = 0, bytes = 0, top = 0;
        char top_name[64] = "";
        for (int i = 0; i < n; i++) {
            unsigned long long c0 = 0, b0 = 0;
            for (int j = 0; j < nb; j++)
                if (!strcmp(base[j].name, now[i].name)) {
                    c0 = base[j].exec_calls; b0 = base[j].served_bytes; break;
                }
            if ((now[i].roles & LMB_SWARM_ROLE_EXPERT) && now[i].have_exec_stats &&
                now[i].exec_calls >= c0) {
                unsigned long long d = now[i].exec_calls - c0;
                calls += d;
                if (!strncmp(now[i].name, "donor-", 6)) donor_calls += d;
                if (d > top) { top = d; snprintf(top_name, sizeof top_name, "%.63s", now[i].name); }
            }
            if ((now[i].roles & LMB_SWARM_ROLE_STORAGE) && now[i].served_bytes >= b0)
                bytes += now[i].served_bytes - b0;
        }
        char text[200];
        if (calls)
            snprintf(text, sizeof text, "expert %llu calls · %llu%% to donors · %.24s",
                     calls, 100 * donor_calls / calls, top_name);
        else
            snprintf(text, sizeof text, "expert 0 calls so far");
        if (bytes)
            snprintf(text + strlen(text), sizeof text - strlen(text),
                     " · disk %.0f MB served", (double)bytes / 1e6);
        pthread_mutex_lock(&g_live.lock);
        snprintf(g_live.swarm, sizeof g_live.swarm, "%s", text);
        live_draw_locked();
        pthread_mutex_unlock(&g_live.lock);
    }
    return NULL;
}

static int live_begin(const char *tracker, const char *model,
                      const char *initial_phase) {
    if (!g_tty || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
    struct termios raw;
    if (g_chat_term_valid) g_live.old_term = g_chat_term;
    else if (tcgetattr(STDIN_FILENO, &g_live.old_term)) return 0;
    raw = g_live.old_term;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)IXON;
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) return 0;
    pthread_mutex_lock(&g_live.lock);
    g_live.rows = term_h(); g_live.cols = term_w();
    g_live.enabled = g_live.rows >= 8;
    g_live.raw_term = raw;
    g_live.raw_set = 1; g_live.active = 1;
    snprintf(g_live.tracker, sizeof g_live.tracker, "%s", tracker);
    snprintf(g_live.model, sizeof g_live.model, "%s", model);
    snprintf(g_live.phase, sizeof g_live.phase, "%s", initial_phase);
    g_live.notice[0] = 0;
    g_live.swarm[0] = 0;
    live_clear_input_locked();
    if (g_live.enabled) {
        /* Reserve fresh lines before drawing the dock: never clear a line
         * which still contains the user's just-submitted message. */
        printf("\x1b[r\x1b[%d;1H\r\n\r\n\r\n\r\n\x1b[%d;1H",
               g_live.rows, g_live.rows - 3);
        g_live.output_col = 0; g_live.output_len = 0;
        memset(&g_live.output_utf8, 0, sizeof g_live.output_utf8);
        live_draw_locked();
    }
    pthread_mutex_unlock(&g_live.lock);
    if (!g_live_atexit_registered) {
        if (atexit(live_atexit_restore) == 0) g_live_atexit_registered = 1;
    }
    g_live.swarm_started = g_live.enabled && g_live.tracker[0] &&
        pthread_create(&g_live.swarm_thread, NULL, live_swarm_thread, NULL) == 0;
    if (pthread_create(&g_live.thread, NULL, live_input_thread, NULL)) {
        if (g_live.enabled) {
            static const char reset_scroll[] = "\x1b[r";
            ssize_t ignored = write(STDOUT_FILENO, reset_scroll,
                                    sizeof reset_scroll - 1);
            (void)ignored;
        }
        g_live.active = 0; g_live.enabled = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &g_live.old_term);
        g_live.raw_set = 0;
        return 0;
    }
    return 1;
}

static void live_end(void) {
    if (!g_live.active && !g_live.raw_set) return;
    g_live.active = 0;
    pthread_join(g_live.thread, NULL);
    if (g_live.swarm_started) { pthread_join(g_live.swarm_thread, NULL); g_live.swarm_started = 0; }
    pthread_mutex_lock(&g_live.lock);
    if (g_live.enabled) {
        printf("\x1b" "7\x1b[r\x1b[%d;1H\x1b[2K\x1b[%d;1H\x1b[2K"
               "\x1b[%d;1H\x1b[2K\x1b" "8", g_live.rows - 2, g_live.rows - 1,
               g_live.rows);
        fflush(stdout);
    }
    g_live.enabled = 0;
    pthread_mutex_unlock(&g_live.lock);
    if (g_live.raw_set)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_live.old_term);
    g_live.raw_set = 0;
}

static int live_take_pending(char *out, size_t cap) {
    pthread_mutex_lock(&g_live.lock);
    int have = g_live.pending_ready;
    if (have) {
        snprintf(out, cap, "%s", g_live.pending);
        g_live.pending[0] = 0; g_live.pending_ready = 0;
    }
    pthread_mutex_unlock(&g_live.lock);
    return have;
}

/* one line, rewritten in place: the star, what it is doing, how far along */
/* The engine is up, but on a swarm-fed model its dense weights may still be
 * crossing the network: "ready" only after the warm-up says so, with the
 * bar meanwhile. Bounded: a warm-up that has not moved for two minutes is
 * left to the engine's own on-demand reads (the shim retries them). */
static void *spinner_thread(void *arg);
static void wait_dense_ready(void) {
    if (g_eng.dense_ready || g_eng.dense_total_mb <= 0) {
        /* no progress line yet: give the warm-up a moment to size itself */
        for (int i = 0; i < 30 && !g_eng.dense_ready && g_eng.dense_total_mb <= 0; i++)
            usleep(100 * 1000);
        if (g_eng.dense_ready || g_eng.dense_total_mb <= 0) return;
    }
    g_eng.booting = 1; g_eng.spinning = 1;
    pthread_t sp;
    int spun = g_tty && pthread_create(&sp, NULL, spinner_thread,
                                       (void *)"dense weights in mirror") == 0;
    double last_mb = g_eng.dense_mb, last_move = nowd();
    while (!g_eng.dense_ready) {
        usleep(200 * 1000);
        if (g_eng.dense_mb != last_mb) { last_mb = g_eng.dense_mb; last_move = nowd(); }
        else if (nowd() - last_move > 120) break;
    }
    g_eng.spinning = 0;
    if (spun) pthread_join(sp, NULL);
    g_eng.booting = 0;
    if (g_eng.dense_ready == 1)
        printf("  %s\xe2\x9c\x93 dense weights in mirror: %.2f GB%s\n", C_DIM,
               g_eng.dense_total_mb / 1000.0, C_R);
    else
        printf("  %s\xe2\x9a\xa0 dense weights incomplete (%.2f/%.2f GB): the engine "
               "downloads the rest on first use%s\n", C_DIM,
               g_eng.dense_mb / 1000.0, g_eng.dense_total_mb / 1000.0, C_R);
}

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

        char prog[200] = "";
        double got = g_eng.local_gb + g_eng.net_mb / 1000.0;
        if (g_eng.booting && g_eng.dense_total_mb > 0 && !g_eng.dense_ready) {
            double frac = g_eng.dense_mb / g_eng.dense_total_mb;
            if (frac > 1) frac = 1;
            int cells = 20, full = (int)(frac * cells + 0.5);
            char bar[128] = "";
            for (int k = 0; k < cells; k++)
                strncat(bar, k < full ? "\xe2\x96\x88" : "\xe2\x96\x91", sizeof bar - strlen(bar) - 1);
            double left_mb = g_eng.dense_total_mb - g_eng.dense_mb;
            char eta[40] = "";
            if (g_eng.dense_rate > 0.05 && left_mb > 0) {
                double sec = left_mb / g_eng.dense_rate;
                if (sec >= 3600) snprintf(eta, sizeof eta, " \xc2\xb7 ETA %d h %d min", (int)(sec / 3600), ((int)sec % 3600) / 60);
                else if (sec >= 60) snprintf(eta, sizeof eta, " \xc2\xb7 ETA %.0f min", sec / 60);
                else snprintf(eta, sizeof eta, " \xc2\xb7 ETA %.0f s", sec);
            }
            snprintf(prog, sizeof prog, " %s\xe2\x96\x95%s\xe2\x96\x8f %.2f/%.2f GB \xc2\xb7 %.1f MB/s%s%s",
                     C_DIM, bar, g_eng.dense_mb / 1000.0, g_eng.dense_total_mb / 1000.0,
                     g_eng.dense_rate, eta, C_R);
            snprintf(what, sizeof what, "dense weights in mirror");
        } else if (g_eng.booting && g_eng.total_gb > 0)
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
            fprintf(stderr, "\r\x1b[2K  %sNo bytes or engine output for 90s. "
                            "On first use the server may be hashing the model; "
                            "otherwise check disk space and system logs.%s\n",
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
    EK_INVALID = -1,
    EK_GLM = 0,        /* colibri monolith: framed, templates internally */
    EK_GLM53,          /* glm53:         GLM-5.3 template, serve codec        */
    EK_DEEPSEEK,       /* deepseek_v4:  <｜User｜>…<｜Assistant｜></think>       */
    EK_OLMOE,          /* olmoe:        |||IP_ADDRESS|||<|user|>…<|assistant|> */
    EK_QWEN36,         /* qwen36:       ChatML <|im_start|>…                   */
    EK_QWEN38,         /* qwen38:       system reasoning prompt + ChatML     */
    EK_INKLING,        /* inkling:      <|message_user|><|content_text|>…      */
    EK_KIMI            /* kimi_k3:      K3CHAT1 byte-counted wire              */
} EngKind;
/* Where the engine actually is.
 *
 * Every shutdown path in this file was written for a child process: stop
 * returns immediately without a pid, and the second Ctrl-C kills a pid to
 * break a blocked read. Neither is true of an engine reached over a socket,
 * and the failure is silent — /quit would drop the connection without ever
 * closing the remote session, and a hung read would never wake. So the
 * transport is declared rather than inferred from whether pid happens to be
 * positive. */
typedef enum {
    ENGINE_CHILD = 0,   /* a process we forked; pipes to it */
    ENGINE_SOCKET,      /* a host we dialled; to == from == the socket */
} EngineTransport;

typedef struct {
    pid_t pid;
    int to, from;
    Proto proto;
    EngKind kind;
    int segment;
    int greedy_only; /* actual Edge capability, not a model-name assumption */
    EngineTransport transport;
    uint32_t numeric_abi;
    char numeric_class[97];
} Engine;

static int artifact_is(const char *value, const char *name) {
    if (!value || !name) return 0;
    const char *base = strrchr(value, '/');
    base = base ? base + 1 : value;
    if (!strcmp(base, name)) return 1;
    size_t n = strlen(name);
    return !strncmp(base, name, n) && !strcmp(base + n, "_p2p");
}

/* Protocol and prompt-template dispatch is exact for the same reason model
 * family dispatch is exact. Unknown is not GLM: it is an error. */
static EngKind engine_kind_of(const char *engine) {
    if (artifact_is(engine, "colibri") || !strcmp(engine ? engine : "", "glm") ||
        !strcmp(engine ? engine : "", "glm_moe_dsa") ||
        !strcmp(engine ? engine : "", "glm5_moe")) return EK_GLM;
    if (artifact_is(engine, "glm53") ||
        !strcmp(engine ? engine : "", "glm5_next") ||
        !strcmp(engine ? engine : "", "glm5_next_text")) return EK_GLM53;
    if (artifact_is(engine, "deepseek_v4") || artifact_is(engine, "deepseek"))
        return EK_DEEPSEEK;
    if (artifact_is(engine, "olmoe")) return EK_OLMOE;
    if (artifact_is(engine, "qwen36") ||
        !strcmp(engine ? engine : "", "qwen3_5_moe") ||
        !strcmp(engine ? engine : "", "qwen3_5_moe_text")) return EK_QWEN36;
    if (artifact_is(engine, "qwen38") ||
        !strcmp(engine ? engine : "", "qwen4_exp") ||
        !strcmp(engine ? engine : "", "qwen4_exp_text")) return EK_QWEN38;
    if (artifact_is(engine, "inkling") ||
        !strcmp(engine ? engine : "", "inkling_mm_model")) return EK_INKLING;
    if (artifact_is(engine, "kimi_k3") ||
        !strcmp(engine ? engine : "", "kimi") ||
        !strcmp(engine ? engine : "", "kimi_linear")) return EK_KIMI;
    return EK_INVALID;
}
/* The serve-codec engines: framed READY at boot, but SUBMIT/DATA/DONE turns and
 * a raw (un-templated) payload. Everything but GLM. */
static int kind_is_serve2(EngKind k) { return k != EK_INVALID && k != EK_GLM; }

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
    e->greedy_only = 0;
    e->numeric_abi = 0; e->numeric_class[0] = 0;
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
        if (memmem(buf, len, FRAME_READY, strlen(FRAME_READY))) {
            e->greedy_only = strstr(buf, "\nLUMABRI_SAMPLING GREEDY\n") != NULL;
            const char *numeric = strstr(buf, "\nLUMABRI_NUMERIC ");
            if (numeric) {
                unsigned abi = 0; char value[97] = ""; int consumed = 0;
                if (sscanf(numeric, "\nLUMABRI_NUMERIC %u %96[^\n]%n", &abi, value, &consumed) == 2 && abi &&
                    consumed > 0 && numeric[consumed] == '\n' &&
                    lmb_cal_text(value, sizeof value)) {
                    e->numeric_abi = abi; snprintf(e->numeric_class, sizeof e->numeric_class, "%s", value);
                }
            }
            e->proto = PROTO_FRAMED; free(buf); return 0;
        }
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
        if (t->mode == TF_MID) { live_write(&c, 1); if (c == '\n') t->mode = TF_LINESTART; continue; }
        /* TF_LINESTART: buffer until we can classify the line */
        if (c == '\n') {                       /* short line, can't be a dashboard row */
            live_write(t->pfx, (size_t)t->pfxn); live_write("\n", 1);
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
            live_write(t->pfx, (size_t)t->pfxn);
            t->pfxn = 0; t->mode = TF_MID;
        }
        /* else: keep buffering (a keyword prefix so far) */
    }
}

static void le_flush_tel(TelFilter *t) {       /* trailing buffered text at stream end */
    if (t->mode == TF_LINESTART && t->pfxn > 0) {
        live_write(t->pfx, (size_t)t->pfxn);
        t->pfxn = 0;
    }
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
        if (emit) live_write(s->b + s->off, take);
        if (cap && cap_add(cap, (char *)s->b + s->off, take)) return -1;
        s->off += take; n -= take;
    }
    return 0;
}

/* Read a serve-codec reply: DATA frames (the generated text) until DONE. Bare
 * dashboard lines (EMAP/TIERS/…) and ACCEPT are skipped. On success returns 0
 * with the STAT tail in statline and, if captured != NULL, the assistant text
 * malloc'd into *captured (for the conversation history). -1 if the engine died. */
/* When the first token of the current reply arrived: everything before it
 * is prefill (the prompt crossing the layers), everything after is decode.
 * One number for both hid a 60 s prefill inside "0.2 tok/s". */
#include "lumabri_metrics.h"
static volatile double g_first_token_at;

static int stream_serve2(Engine *e, char *statline, size_t scap, char **captured) {
    SReader s = { e->from, {0}, 0, 0 };
    char line[600];
    Cap cap = {0};
    if (statline && scap) statline[0] = 0;
    if (captured) *captured = NULL;
    g_first_token_at = 0;
    for (;;) {
        if (sr_line(&s, line, sizeof line) < 0) { free(cap.p); return -1; }
        if (!strncmp(line, "DATA ", 5)) {
            live_status("decode · %s", e->segment ? "Segment in the swarm" :
                                                "expert/engine");
            char *sp = strchr(line + 5, ' ');            /* DATA <id> <bytes> */
            size_t n = sp ? strtoull(sp + 1, NULL, 10) : 0;
            if (n && g_first_token_at == 0) g_first_token_at = nowd();
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
        } else if (!strncmp(line, "PROGRESS ", 9)) {
            unsigned id = 0;
            char phase[24] = "";
            size_t current = 0, total = 0, third = 0;
            if (sscanf(line, "PROGRESS %u %23s %zu %zu %zu",
                       &id, phase, &current, &total, &third) >= 2) {
                if (!strcmp(phase, "ROUTE"))
                    live_status("routing · %zu hosts · %zu segments · %s",
                                current, total, third ? "relay" : "direct P2P");
                else if (!strcmp(phase, "PREFILL"))
                    live_status("prefill · %zu/%zu token · %s", current, total,
                                e->segment ? "Segment" : "expert");
                else if (!strcmp(phase, "DECODE"))
                    live_status("decode · %zu token · %s", current,
                                e->segment ? "Segment" : "expert");
                else if (!strcmp(phase, "FAILOVER")) {
                    const char *peer = strstr(line, "FAILOVER ");
                    live_status("Segment recovery · reopening %s",
                                peer ? peer + strlen("FAILOVER ") : "peer");
                } else if (!strcmp(phase, "CHECKPOINT"))
                    live_status("KV checkpoint · %zu tokens saved", current);
            }
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
 * Each marker set mirrors its engine's serve/CLI source. The six-family tiny
 * release gate exercises GLM, Inkling, Kimi, OLMoE, Qwen and DeepSeek through
 * this exact codec and compares their generated token IDs with Colibri oracles.
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
/* GLM-5.2 (colibri.c's official chat_template.jinja path).  The monolithic
 * engine applies this internally; Segment Edge deliberately accepts exact
 * bytes, so only the Segment Serve2 path reaches this case. */
#define GLM_BOS  "[gMASK]<sop>"
#define GLM_U    "<|user|>"
#define GLM_ASST "<|assistant|><think></think>"
#define GLM53_ASST "<|assistant|><think>"
/* qwen36 (qwen36.c: ChatML, no BOS). */
#define QW_U    "<|im_start|>user\n"
#define QW_ASST "<|im_end|>\n<|im_start|>assistant\n<think>\n"
#define QW_AEND "<|im_end|>\n"
#define QW38_SYS "<|im_start|>system\nReasoning effort is set to xhigh. Please " \
    "think carefully through the task, validate key assumptions, consider " \
    "plausible alternatives, and prioritize correctness, consistency, and " \
    "clarity in the final answer.<|im_end|>\n"
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
    (void)first;
    switch (k) {
    case EK_GLM:
        if (cap_str(c, GLM_U) || cap_str(c, u) || cap_str(c, GLM_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_GLM53:
        if (cap_str(c, GLM_U) || cap_str(c, u) || cap_str(c, GLM53_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
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
    case EK_QWEN38:
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
    default:
        return -1;
    }
}

/* The once-at-front SUBMIT prefix (kept out of the stored history). */
static int serve2_prefix(Cap *c, EngKind k) {
    if (k == EK_GLM)      return cap_str(c, GLM_BOS);
    if (k == EK_GLM53)    return cap_str(c, GLM_BOS);
    if (k == EK_DEEPSEEK) return cap_str(c, DS_BOS);
    if (k == EK_QWEN38)   return cap_str(c, QW38_SYS);
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
    int hn = snprintf(hdr, sizeof hdr, "SUBMIT %u 0 %zu %d %.1f 0.95\n",
                      ++id, c.len, max_new < 1 ? 1 : max_new, e->greedy_only ? 0.0 : 0.7);
    int ok = hn >= 0 && (size_t)hn < sizeof hdr &&
             !engine_write_full(e->to, hdr, (size_t)hn) &&
             !engine_write_full(e->to, c.p, c.len) &&
             !engine_write_full(e->to, "\n", 1);             /* payload terminator */
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

/* The monolithic engine binary for the same family. Unknown model types are
 * refused: a generic fallback can tokenize or execute with the wrong ABI. */
static const char *engine_for(const char *model_type) {
    const LmbModelFamily *f = lumi_family(model_type);
    return f ? f->engine : NULL;
}

static const char *p2p_engine_for(const char *model_type) {
    const LmbModelFamily *f = lumi_family(model_type);
    return f ? f->p2p_engine : NULL;
}

/* `local_dir` non-NULL: the model is already on this disk, so no shim, no
 * mirror, no second copy. That is the right mode on the machine that serves
 * the model — otherwise chatting there downloads it from itself. */
static int engine_spawn(const char *engine, const char *shim, const char *tracker,
                        const char *model, const char *local_dir,
                        int ctx, int max_new, int cap_experts, EngKind kind,
                        Engine *e) {
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
    int line_proto = kind == EK_OLMOE;
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
            if (lmb_preload_file(shim)) { perror("Cannot load household weight loader"); _exit(125); }
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
        /* DeepSeek V4 has its own pinning: at open it replays `.coli_usage`
         * and pins the historically hot experts of every layer, which behind
         * the shim is a multi-GB expert download before the first token —
         * the experts this chatter will never execute. PIN=0 is OLMoE's
         * switch, not V4's. */
        if (!local_dir) { setenv("COLI_V4_AUTOPIN", "0", 0);
                          setenv("COLI_V4_PREWARM", "0", 0); }
        /* and pull the dense weights NOW, with a bar, instead of letting the
         * first reply discover them layer by layer over the WAN */
        if (!local_dir) setenv("LUMABRI_PREFETCH_DENSE", "1", 0);
        /* Speculative decoding on a swarm-fed DeepSeek chat: every token is
         * 43 network rounds, and the only way to amortize them without
         * moving state off this machine is to verify several candidate
         * tokens per round. Colibri's V4 n-gram drafter proposes up to
         * V4_DRAFT tokens from the context and the full model verifies them
         * in one batched pass (greedy: the tokens are exactly those of the
         * one-by-one path). The batched MoE rows travel as one multi-row
         * call per expert, which the client already speaks. An explicit
         * V4_DRAFT (0 to turn it off) still wins. */
        if (!local_dir && kind == EK_DEEPSEEK) setenv("V4_DRAFT", "8", 0);
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
    g_signal_engine_pid = (sig_atomic_t)pid;
    e->proto = PROTO_UNKNOWN;
    e->kind = kind;
    if (e->kind == EK_INVALID) {
        fprintf(stderr, "[lumabri] cannot determine protocol for engine %s\n",
                engine);
        close(e->to); close(e->from); kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        e->pid = 0; e->to = e->from = -1;
        return -1;
    }
    e->segment = 0;
    pthread_t t;
    pthread_create(&t, NULL, stderr_thread, (void *)(intptr_t)err_pipe[0]);
    pthread_detach(t);
    return 0;
}

/* Segment speaks the same SUBMIT/DATA/DONE codec already consumed by the TUI,
 * but its Edge loader runs over Lumabri's virtual mirror.  Therefore a client
 * with no model directory fetches only config/tokenizer/embedding/head through
 * the existing signed CAS, while all transformer layers stay on the selected
 * peers. */
static int segment_engine_spawn(const char *engine, const char *shim,
                                const char *tracker, const char *model,
                                const char *model_type, int ctx, Engine *e) {
    const char *segment_id = segment_engine_for(model_type);
    if (!segment_id) return -1;
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char vroot[1024], cache[1024], cas[1024];
    const char *vroot_env = getenv("LUMABRI_VROOT");
    const char *cache_env = getenv("LUMABRI_CACHE");
    if (vroot_env && vroot_env[0]) snprintf(vroot, sizeof vroot, "%s", vroot_env);
    else snprintf(vroot, sizeof vroot, "%s/.lumabri/%s/vroot", home, model);
    if (cache_env && cache_env[0]) snprintf(cache, sizeof cache, "%s", cache_env);
    else snprintf(cache, sizeof cache, "%s/.lumabri/%s/cache", home, model);
    snprintf(cas, sizeof cas, "%s/.lumabri/cas", home);
    mkdir_p(cache);

    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], 0); dup2(out_pipe[1], 1); dup2(err_pipe[1], 2);
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        if (lmb_preload_file(shim)) { perror("Cannot load household weight loader"); _exit(125); }
        setenv("LUMABRI_VROOT", vroot, 1);
        setenv("LUMABRI_CACHE", cache, 1);
        setenv("LUMABRI_CAS", cas, 0);
        setenv("LUMABRI_TRACKER", tracker, 1);
        setenv("LUMABRI_MODEL", model, 1);
        setenv("LUMABRI_STATS", "2", 1);
        char context[32], discovery_ms[32];
        snprintf(context, sizeof context, "%d", ctx);
        snprintf(discovery_ms, sizeof discovery_ms, "%d", getenv("LUMABRI_SEGMENT_REQUIRED") ? 30000 : 2500);
        char *argv[] = {
            (char *)engine,
            "--serve",
            "--engine", (char *)segment_id,
            "--model-dir", vroot,
            "--model", (char *)model,
            "--tracker", (char *)tracker,
            "--context", context,
            "--max-rows", "16",
            "--discovery-timeout-ms", discovery_ms,
            NULL
        };
        execv(engine, argv);
        perror(engine);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    e->pid = pid; e->to = in_pipe[1]; e->from = out_pipe[0];
    g_signal_engine_pid = (sig_atomic_t)pid;
    e->proto = PROTO_UNKNOWN;
    e->kind = engine_kind_of(model_type);
    if (e->kind == EK_INVALID) {
        close(e->to); close(e->from); kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        e->pid = 0; e->to = e->from = -1;
        return -1;
    }
    e->segment = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, stderr_thread,
                   (void *)(intptr_t)err_pipe[0]);
    pthread_detach(thread);
    return 0;
}

/* Why the child is gone, in the words of the kernel and of the child. */
static void engine_diag(Engine *e, int booting) {
    int st = 0;
    pid_t waited = e->pid > 0 ? waitpid(e->pid, &st, WNOHANG) : -1;
    /* EOF on the child's stdout can win the tiny race before exit status is
     * published. Give a boot failure a bounded moment to become waitable so
     * diagnostics do not randomly lose the engine's actual exit code. */
    if (booting && e->pid > 0 && waited == 0) {
        for (int i = 0; i < 20 && waited == 0; i++) {
            struct timespec pause = {0, 10 * 1000 * 1000};
            while (nanosleep(&pause, &pause) && errno == EINTR) { }
            waited = waitpid(e->pid, &st, WNOHANG);
        }
    }
    if (e->pid > 0 && waited == e->pid) {
        if (g_signal_engine_pid == (sig_atomic_t)e->pid)
            g_signal_engine_pid = 0;
        e->pid = 0;
        if (WIFSIGNALED(st)) {
            int s = WTERMSIG(st);
            printf("  %sthe engine was terminated (signal %d: %s)%s\n",
                   C_RED, s, strsignal(s), C_R);
            if (s == SIGKILL)
                printf("  %sCheck system logs for an out-of-memory kill. "
                       "Try reducing --ctx and --cap, or using more RAM.%s\n",
                       C_DIM, C_R);
        } else if (WIFEXITED(st)) {
            int c = WEXITSTATUS(st);
            printf("  %sthe engine exited with code %d%s\n", C_RED, c, C_R);
            if (c == 127)
                printf("  %sthe binary did not start: a library may be missing. "
                       "Check its shared-library dependencies.%s\n", C_DIM, C_R);
        }
    } else if (booting)
        printf("  %sthe engine closed stdout before reporting readiness%s\n",
               C_RED, C_R);
    else
        printf("  %sthe engine stopped during the response%s\n"
               "  %sA peer may have disconnected: with only one "
               "holder per expert there is no fallback%s\n",
               C_RED, C_R, C_DIM, C_R);
    printf("  %srecent engine output:%s\n", C_DIM, C_R);
    tail_dump(25);
    /* A known fatal line deserves its cure, not just its epitaph. The tail
     * told the truth all along — but nobody reads a truth buried under
     * twenty lines of boot noise, so name the fix explicitly. */
    pthread_mutex_lock(&g_eng.lk);
    int n = g_eng.ntail < ETAIL ? g_eng.ntail : ETAIL;
    int tok_missing = 0, xtml_missing = 0;
    for (int i = g_eng.ntail - n; i < g_eng.ntail; i++) {
        const char *l = g_eng.tail[i % ETAIL];
        if (strstr(l, "needs tokenizer.json")) tok_missing = 1;
        if (strstr(l, "XTML")) xtml_missing = 1;
    }
    pthread_mutex_unlock(&g_eng.lk);
    if (tok_missing || xtml_missing)
        printf("  %s→ the checkpoint has no tokenizer.json %s. The Kimi K3 HF repository "
               "does not include it; generate it once with\n"
               "    python3 <colibri>/c/tools/k3_tokenizer.py <model_dir> "
               "-o <model_dir>/tokenizer.json\n"
               "  For a local checkpoint Lumabri retries this on the next "
               "startup; for a swarm, the server operator must do it.%s\n",
               C_RED, xtml_missing ? "with XTML tokens" : "usable", C_R);
}

static void engine_stop(Engine *e) {
    if (e->transport == ENGINE_SOCKET) {
        /* One descriptor, closed once. `to` and `from` are the same socket,
         * so closing both would close a number the process may already have
         * handed to something else. Shutdown first: it wakes a peer blocked
         * on read, which a bare close does not guarantee while another
         * thread still holds the descriptor. */
        if (e->to >= 0) {
            shutdown(e->to, SHUT_RDWR);
            close(e->to);
        }
        e->to = e->from = -1;
        e->pid = 0;
        g_signal_engine_fd = -1;
        return;
    }
    if (e->pid <= 0) return;
    pid_t pid = e->pid;
    if (g_signal_engine_pid == (sig_atomic_t)pid)
        g_signal_engine_pid = 0;
    /* EOF is the Segment gateway's clean shutdown protocol: it closes every
     * remote session before exiting.  Killing it unconditionally made normal
     * /quit leak hour-long session leases until an executor hit its quota.
     * A signal-driven interruption has already asked it to stop and simply
     * falls through to the bounded reap/kill path. */
    close(e->to); e->to = -1;
    if (e->segment && !g_stopping) {
        for (int attempt = 0; attempt < 100; attempt++) {
            pid_t done = waitpid(pid, NULL, WNOHANG);
            if (done == pid || (done < 0 && errno == ECHILD)) {
                close(e->from); e->from = -1; e->pid = 0;
                return;
            }
            struct timespec pause = {0, 20 * 1000 * 1000};
            while (nanosleep(&pause, &pause) && errno == EINTR && !g_stopping) { }
            if (g_stopping) break;
        }
    }
    close(e->from); e->from = -1;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    e->pid = 0;
}

/* model_boot is defined with the chat command below; the host uses the same
 * one, because a host IS a chat whose terminal happens to be a socket. */
static int model_boot(const char *tracker, const char *model, const char *shim,
                      const char *engines_dir, const char *engine_path,
                      const char *local_dir, int ctx, int max_new,
                      int cap_experts, Engine *e, Swarm *sw);

/* ---- the host side ------------------------------------------------------
 *
 * One engine, one session at a time, and a socket in front of it.
 *
 * The bridge is deliberately a byte pipe rather than a parser: the client
 * writes SUBMIT and reads DATA/DONE exactly as it would over a pipe, so the
 * codec, the templating and the arithmetic are all unchanged and untouched
 * by this file. What the host adds is the things a socket needs and a pipe
 * did not: a greeting, admission, limits, and a reset between clients.
 *
 * BUSY is a real answer. A second client is refused immediately rather than
 * queued, because a queue that nobody can see is how "it got slow" replaces
 * "it is full" — and this session owns the KV of the first client, which
 * cannot be shared or interleaved without the work of step 9. */
typedef struct {
    Engine *engine;
    const char *model_type;
    const char *engine_kind;
    uint32_t speed_milli;      /* 0 until this host has been measured */
    uint32_t max_frame;
    uint32_t max_new;
    uint32_t idle_seconds;
    const uint8_t *client_key; /* optional identity bound by an accepted home plan */
} HostState;

static int host_read_hello(int fd, const HostState *h) {
    lmb_set_io_timeout(fd, 2000);
    if (lmb_secure_server(fd) ||
        (h->client_key && !lmb_secure_peer_matches(fd, h->client_key))) return -1;
    LmbMsg m = {0};
    int rc = lmb_recv(fd, &m);
    const char *token = getenv("LUMABRI_TOKEN");
    if (!rc && m.op == LMB_AUTH) {
        char supplied[LMB_TOKEN_MAX + 1] = "", expected[LMB_TOKEN_MAX + 1] = "";
        LmbCur c = {m.body, m.body_len, 0};
        rc = m.pay_len || lmb_cur_str(&c, supplied, sizeof supplied) || c.off != c.len ||
             !token || strlen(token) > LMB_TOKEN_MAX;
        if (!rc) {
            snprintf(expected, sizeof expected, "%s", token);
            rc = !lmb_token_equal(supplied, expected);
        }
        lmb_msg_free(&m);
        if (!rc) rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0) || lmb_recv(fd, &m);
    } else if (token && *token) rc = -1;
    if (!rc) rc = m.op != LMB_HOST_HELLO || m.body_len || m.pay_len;
    lmb_msg_free(&m);
    return rc ? -1 : 0;
}

static int host_greet(int fd, const HostState *h, int busy) {
    LmbBuf b = {0};
    lmb_buf_str(&b, h->model_type ? h->model_type : "");
    lmb_buf_str(&b, h->engine_kind ? h->engine_kind : "");
    lmb_buf_str(&b, "cpu");    /* no Segment adapter advertises a GPU yet */
    lmb_buf_u32(&b, busy ? 0u : 1u);
    lmb_buf_u32(&b, h->speed_milli);
    lmb_buf_u32(&b, h->max_new);
    lmb_buf_u32(&b, h->max_frame);
    lmb_buf_u32(&b, 2);       /* SUBMIT/DATA/DONE, independent of model family */
    /* Optional capability word after codec. Older clients reject the extra
     * field rather than silently request unsupported stochastic sampling. */
    if (h->engine->greedy_only || h->engine->numeric_abi) lmb_buf_u32(&b, (uint32_t)h->engine->greedy_only);
    if (h->engine->numeric_abi) {
        lmb_buf_u32(&b, 0x314d554e); /* NUM1, optional measured runtime metadata */
        lmb_buf_u32(&b, h->engine->numeric_abi);
        lmb_buf_str(&b, h->engine->numeric_class);
    }
    int rc = lmb_send(fd, LMB_HOST_HELLO_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

typedef struct {
    char header[512];
    size_t header_len;
    uint64_t payload_left;
    int need_terminator;
} HostInput;

static int host_header(HostInput *in, Engine *e, const HostState *h) {
    in->header[in->header_len] = 0;
    char id[64], extra;
    unsigned slot = 0, max_new = 0;
    unsigned long long bytes = 0;
    float temp = 0, top_p = 0;
    int fields = sscanf(in->header, "SUBMIT %63s %u %llu %u %f %f %c",
                        id, &slot, &bytes, &max_new, &temp, &top_p, &extra);
    if (fields == 6) {
        if (slot != 0 || bytes > h->max_frame || max_new < 1 ||
            max_new > h->max_new || !isfinite(temp) || !isfinite(top_p) ||
            temp < 0 || top_p <= 0 || top_p > 1) {
            fprintf(stderr, "[host] refused SUBMIT limits (slot=%u bytes=%llu max_new=%u/%u)\n", slot, bytes, max_new, h->max_new);
            return -1;
        }
        if (engine_write_full(e->to, in->header, in->header_len)) return -1;
        in->payload_left = bytes;
        in->need_terminator = bytes == 0;
        in->header_len = 0;
        return 0;
    }
    fields = sscanf(in->header, "CANCEL %63s %c", id, &extra);
    if (fields == 1) {
        int rc = engine_write_full(e->to, in->header, in->header_len);
        in->header_len = 0;
        return rc;
    }
    fields = sscanf(in->header, "STOP %63s %c", id, &extra);
    if (fields == 1) {
        int rc = engine_write_full(e->to, in->header, in->header_len);
        in->header_len = 0;
        return rc;
    }
    fprintf(stderr, "[host] unrecognized codec header (%zu bytes)\n", in->header_len);
    return -1;
}

static int host_input(HostInput *in, Engine *e, const HostState *h,
                      const uint8_t *data, size_t bytes) {
    size_t at = 0;
    while (at < bytes) {
        if (in->payload_left) {
            size_t take = bytes - at;
            if ((uint64_t)take > in->payload_left)
                take = (size_t)in->payload_left;
            if (engine_write_full(e->to, data + at, take)) return -1;
            at += take;
            in->payload_left -= take;
            if (!in->payload_left) in->need_terminator = 1;
            continue;
        }
        if (in->need_terminator) {
            if (data[at++] != '\n' || engine_write_full(e->to, "\n", 1))
                return -1;
            in->need_terminator = 0;
            continue;
        }
        const uint8_t *nl = memchr(data + at, '\n', bytes - at);
        size_t take = nl ? (size_t)(nl - (data + at)) + 1 : bytes - at;
        if (take > sizeof in->header - 1 - in->header_len) return -1;
        memcpy(in->header + in->header_len, data + at, take);
        in->header_len += take;
        at += take;
        if (nl && host_header(in, e, h)) return -1;
    }
    return 0;
}

/* Both directions remain inside LMB_HOST_STREAM frames (authenticated and
 * encrypted whenever the transport's strict mode is enabled). */
static void host_bridge(int fd, Engine *e, const HostState *h) {
    char buf[16384];
    HostInput input = {0};
    double last_input = nowd();
    for (;;) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { e->from, POLLIN, 0 } };
        int n = poll(p, 2, 1000);
        if (g_stopping) return;
        if (n < 0) { if (errno == EINTR) continue; return; }
        if (!n && h->idle_seconds && nowd() - last_input > h->idle_seconds) {
            fprintf(stderr, "[host] idle session expired\n");
            return;
        }
        if (p[0].revents & POLLIN) {
            LmbMsg m = {0};
            if (lmb_recv(fd, &m) || m.op != LMB_HOST_STREAM || m.body_len ||
                !m.pay_len || host_input(&input, e, h, m.pay, m.pay_len)) {
                lmb_msg_free(&m);
                fprintf(stderr, "[host] invalid or oversized client frame\n");
                return;
            }
            last_input = nowd();
            lmb_msg_free(&m);
        }
        if (p[1].revents & POLLIN) {
            ssize_t got = read(e->from, buf, sizeof buf);
            if (got <= 0) return;                 /* engine died: so does this */
            if (lmb_send(fd, LMB_HOST_STREAM, NULL, 0, buf, (uint32_t)got))
                return;
        }
        if ((p[0].revents | p[1].revents) & (POLLERR | POLLHUP)) return;
    }
}

typedef struct {
    int lfd;
    const HostState *host;
    _Atomic int stop;
} BusyAcceptor;

static void *host_busy_acceptor(void *arg) {
    BusyAcceptor *a = (BusyAcceptor *)arg;
    while (!atomic_load(&a->stop) && !g_stopping) {
        struct pollfd p = { a->lfd, POLLIN, 0 };
        int n = poll(&p, 1, 100);
        if (n <= 0) continue;
        int fd = accept(a->lfd, NULL, NULL);
        if (fd < 0) continue;
        if (!host_read_hello(fd, a->host))
            (void)host_greet(fd, a->host, 1);
        lmb_close(fd);
    }
    return NULL;
}

static int cmd_host(int argc, char **argv) {
    g_stopping = 0;
    install_chat_signal_handlers();
    signal(SIGPIPE, SIG_IGN);
    const char *tracker = NULL, *want_model = NULL, *local_dir = NULL;
    const char *engines_dir = NULL, *engine_path = NULL;
    const char *client_key = NULL;
    uint8_t allowed_client[32];
    int port = 7350, ctx = 2048, max_new = 256, cap_experts = 64;
    uint32_t max_frame = 1u << 20;
    uint32_t idle_seconds = 300;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tracker") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) want_model = argv[++i];
        else if (!strcmp(argv[i], "--local") && i + 1 < argc) local_dir = argv[++i];
        else if (!strcmp(argv[i], "--engines-dir") && i + 1 < argc) engines_dir = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--client-key") && i + 1 < argc) client_key = argv[++i];
        else if (!strcmp(argv[i], "--ctx") && i + 1 < argc) ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-new") && i + 1 < argc) max_new = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-frame") && i + 1 < argc)
            max_frame = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-seconds") && i + 1 < argc)
            idle_seconds = (uint32_t)atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: lumabri host --model NAME [--port N] "
                            "[--tracker H:P] [--local DIR]\n"
                            "                   [--ctx N] [--max-new N] "
                            "[--max-frame BYTES] [--idle-seconds N]\n");
            return 2;
        }
    }
    if (client_key && (!lmb_secure_enabled() || strlen(client_key) != 64 ||
                       lmb_unhex(allowed_client, client_key, 32))) return 2;
    if (!want_model && !local_dir) {
        fprintf(stderr, "lumabri host needs --model NAME or --local DIR\n");
        return 2;
    }
    if (max_new < 1 || max_new > (1 << 20) || max_frame < 1 ||
        max_frame > LMB_MAX_PAY || idle_seconds < 1) {
        fprintf(stderr, "invalid host limit\n");
        return 2;
    }
    char dir[1024], shim[1200];
    exe_dir(dir, sizeof dir);
    snprintf(shim, sizeof shim, "%s/" LMB_SHIM_NAME, dir);
    if (access(shim, R_OK))
        snprintf(shim, sizeof shim, "%s/../lib/lumabri/" LMB_SHIM_NAME, dir);

    char model[128];
    snprintf(model, sizeof model, "%s", want_model ? want_model : "local");
    Engine eng = {0};
    Swarm sw;
    int lfd = lmb_home_take_listener(port);
    if (lfd < 0) { fprintf(stderr, "[host] cannot listen on %d\n", port); return 1; }
    if (model_boot(tracker ? tracker : "", model, shim, engines_dir,
                   engine_path, local_dir, ctx, max_new, cap_experts, &eng, &sw))
        { close(lfd); return 1; }
    if (!eng.segment && !kind_is_serve2(eng.kind)) {
        fprintf(stderr, "Hosted chat requires a SUBMIT/DATA/DONE engine; use Segment for this adapter.\n");
        close(lfd); engine_stop(&eng); return 1;
    }

    char mtype[64] = "";
    if (local_dir) local_model_type(local_dir, mtype, sizeof mtype);
    else snprintf(mtype, sizeof mtype, "%s", sw.model_type);
    const char *host_engine = engine_for(mtype);
    if (!host_engine) { close(lfd); engine_stop(&eng); return 1; }
    HostState h = { &eng, mtype, host_engine, 0, max_frame,
                    (uint32_t)max_new, idle_seconds, client_key ? allowed_client : NULL };

    printf("  %shost ready on port %d · %s · one session at a time%s\n",
           C_DIM, port, mtype[0] ? mtype : "?", C_R);
    printf("  %sclients need no checkpoint; this machine holds the model and "
           "sees the text of every conversation%s\n", C_DIM, C_R);
    fflush(stdout);
    if (lmb_ready_notify()) { close(lfd); engine_stop(&eng); return 1; }

    while (!g_stopping) {
        struct pollfd lp = { lfd, POLLIN, 0 };
        int r = poll(&lp, 1, 500);
        if (r <= 0) { if (r < 0 && errno != EINTR) break; continue; }
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        if (host_read_hello(fd, &h)) { lmb_close(fd); continue; }
        if (host_greet(fd, &h, 0)) { lmb_close(fd); continue; }
        BusyAcceptor busy = { lfd, &h, 0 };
        pthread_t busy_thread;
        int watching = pthread_create(&busy_thread, NULL,
                                      host_busy_acceptor, &busy) == 0;
        host_bridge(fd, &eng, &h);
        atomic_store(&busy.stop, 1);
        if (watching) pthread_join(busy_thread, NULL);
        lmb_close(fd);
        /* Between two clients the engine keeps the previous conversation in
         * its KV, so the next client would continue somebody else's chat.
         * Restart it: with one session there is nothing cheaper that is also
         * correct, and correctness here is somebody else's private text. */
        fprintf(stderr, "[host] session closed; restarting the engine so the "
                "next client starts clean\n");
        engine_stop(&eng);
        if (model_boot(tracker ? tracker : "", model, shim, engines_dir,
                       engine_path, local_dir, ctx, max_new, cap_experts,
                       &eng, &sw)) {
            fprintf(stderr, "[host] the engine did not come back; stopping\n");
            break;
        }
        h.engine = &eng;
    }
    close(lfd);
    engine_stop(&eng);
    return 0;
}

/* ---- a host, instead of a local engine -----------------------------------
 *
 * The chat loop below talks to two descriptors and nothing else: it writes
 * SUBMIT, it reads DATA until DONE. That boundary is the whole reason a
 * thin client is a small change rather than a rewrite — put a socket where
 * the pipes were and the templating, the history, the streaming and the
 * report all keep working unmodified.
 *
 * What the client must NOT do is boot an engine. No shim, no vroot, no CAS,
 * no mirror, and not even a tokenizer: the serve-codec engines tokenize the
 * SUBMIT payload themselves, so a hosted chatter needs zero bytes of the
 * checkpoint. That is the promise, and it is only kept by not calling
 * model_boot at all. */
typedef struct { int local_fd, network_fd; } HostPump;

static void *host_client_pump(void *arg) {
    HostPump *pump = (HostPump *)arg;
    char bytes[16384];
    for (;;) {
        struct pollfd p[2] = {
            { pump->local_fd, POLLIN, 0 }, { pump->network_fd, POLLIN, 0 }
        };
        int n = poll(p, 2, 1000);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (!n) continue;
        if (p[0].revents & POLLIN) {
            ssize_t got = read(pump->local_fd, bytes, sizeof bytes);
            if (got <= 0 || lmb_send(pump->network_fd, LMB_HOST_STREAM,
                                     NULL, 0, bytes, (uint32_t)got)) break;
        }
        if (p[1].revents & POLLIN) {
            LmbMsg m = {0};
            if (lmb_recv(pump->network_fd, &m) ||
                m.op != LMB_HOST_STREAM || m.body_len || !m.pay_len ||
                lmb_write_full(pump->local_fd, m.pay, m.pay_len)) {
                lmb_msg_free(&m);
                break;
            }
            lmb_msg_free(&m);
        }
        if ((p[0].revents | p[1].revents) & (POLLERR | POLLHUP)) break;
    }
    shutdown(pump->local_fd, SHUT_RDWR);
    close(pump->local_fd);
    lmb_close(pump->network_fd);
    free(pump);
    return NULL;
}

static int host_connect(const char *addr, const char *model_type, const char *expected_key, Engine *e,
                        int *requested_max_new) {
    memset(e, 0, sizeof *e);
    e->to = e->from = -1;
    e->pid = 0;
    e->transport = ENGINE_SOCKET;
    int fd = lmb_connect(addr);
    if (fd < 0) {
        fprintf(stderr, "[lumabri] no host at %s\n", addr);
        return -1;
    }
    if (expected_key) {
        uint8_t key[32];
        if (strlen(expected_key) != 64 || lmb_unhex(key, expected_key, 32) ||
            !lmb_secure_enabled() || !lmb_secure_peer_matches(fd, key)) {
            fprintf(stderr, "[lumabri] host identity differs from the accepted plan\n");
            lmb_close(fd); return -1;
        }
    }
    /* The host answers with the engine kind, because the client applies the
     * chat template and cannot guess it from an address. A host that will
     * not say is one whose replies we would format wrongly. */
    LmbMsg m = {0};
    if (lmb_auth(fd) || lmb_send(fd, LMB_HOST_HELLO, NULL, 0, NULL, 0) ||
        lmb_recv(fd, &m) || m.op != LMB_HOST_HELLO_R) {
        fprintf(stderr, "[lumabri] %s did not answer as an inference host\n",
                addr);
        lmb_msg_free(&m);
        lmb_close(fd);
        return -1;
    }
    LmbCur c = { m.body, m.body_len, 0 };
    char kind[64] = "", mtype[64] = "", backend[32] = "";
    uint32_t sessions_free = 0, speed_milli = 0;
    uint32_t host_max_new = 0, host_max_frame = 0, codec = 0;
    if (lmb_cur_str(&c, mtype, sizeof mtype) ||
        lmb_cur_str(&c, kind, sizeof kind) ||
        lmb_cur_str(&c, backend, sizeof backend) ||
        lmb_cur_u32(&c, &sessions_free) || lmb_cur_u32(&c, &speed_milli)) {
        fprintf(stderr, "[lumabri] %s sent a greeting this build cannot read\n",
                addr);
        lmb_msg_free(&m); lmb_close(fd); return -1;
    }
    if (c.off < c.len &&
        (lmb_cur_u32(&c, &host_max_new) || lmb_cur_u32(&c, &host_max_frame))) {
        fprintf(stderr, "[lumabri] %s sent invalid host limits\n", addr);
        lmb_msg_free(&m); lmb_close(fd); return -1;
    }
    if (c.off < c.len && (lmb_cur_u32(&c, &codec) || codec != 2)) {
        fprintf(stderr, "[lumabri] unsupported hosted codec\n");
        lmb_msg_free(&m); lmb_close(fd); return -1;
    }
    uint32_t sampling_flags = 0;
    if (c.off < c.len && (lmb_cur_u32(&c, &sampling_flags) || sampling_flags > 1)) {
        fprintf(stderr, "[lumabri] unsupported host sampling capabilities\n");
        lmb_msg_free(&m); lmb_close(fd); return -1;
    }
    uint32_t numeric_tag = 0, numeric_abi = 0;
    char numeric_class[97] = "";
    if (c.off < c.len && (lmb_cur_u32(&c, &numeric_tag) || numeric_tag != 0x314d554e ||
        lmb_cur_u32(&c, &numeric_abi) || !numeric_abi ||
        lmb_inventory_string(&c, numeric_class, sizeof numeric_class) || !numeric_class[0])) {
        fprintf(stderr, "[lumabri] unsupported host numeric metadata\n");
        lmb_msg_free(&m); lmb_close(fd); return -1;
    }
    if (c.off != c.len || m.pay_len) { lmb_msg_free(&m); lmb_close(fd); return -1; }
    lmb_msg_free(&m);
    if (model_type && model_type[0] && strcmp(model_type, mtype)) {
        fprintf(stderr, "[lumabri] %s serves %s, not %s\n", addr, mtype,
                model_type);
        lmb_close(fd); return -1;
    }
    if (!sessions_free) {
        fprintf(stderr, "[lumabri] host %s is busy (0 sessions free)\n", addr);
        lmb_close(fd); return -1;
    }
    if (host_max_new && requested_max_new && *requested_max_new > (int)host_max_new) {
        printf("  %shost limit: max-new reduced from %d to %u%s\n", C_DIM,
               *requested_max_new, host_max_new, C_R);
        *requested_max_new = (int)host_max_new;
    }
    (void)host_max_frame;
    EngKind kind_id = engine_kind_of(kind);
    if (kind_id == EK_INVALID) {
        fprintf(stderr, "[lumabri] host %s announced unknown engine %s\n",
                addr, kind);
        lmb_close(fd); return -1;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        lmb_close(fd); return -1;
    }
    HostPump *pump = calloc(1, sizeof *pump);
    if (!pump) { close(pair[0]); close(pair[1]); lmb_close(fd); return -1; }
    pump->local_fd = pair[1]; pump->network_fd = fd;
    pthread_t thread;
    if (pthread_create(&thread, NULL, host_client_pump, pump)) {
        free(pump); close(pair[0]); close(pair[1]); lmb_close(fd); return -1;
    }
    pthread_detach(thread);
    e->to = e->from = pair[0];
    e->kind = kind_id;
    e->greedy_only = (int)sampling_flags;
    e->numeric_abi = numeric_abi;
    snprintf(e->numeric_class, sizeof e->numeric_class, "%s", numeric_class);
    e->proto = codec == 2 || kind_is_serve2(e->kind) ? PROTO_SERVE2 : PROTO_FRAMED;
    g_signal_engine_fd = (sig_atomic_t)pair[0];

    printf("  %shost %s · %s · %s%s\n", C_DIM, addr, mtype,
           backend[0] ? backend : "cpu", C_R);
    if (e->greedy_only)
        printf("  greedy decoding · this backend does not expose sampling logits\n");
    /* Speed is shown only when the host has measured one. "unknown" is the
     * honest state for a host nobody has run yet, and a CPU host is slow
     * rather than broken — saying so before the first prompt is the whole
     * point of putting it here. */
    if (speed_milli)
        printf("  %s~%.1f tok/s measured · %u session%s free%s\n", C_DIM,
               speed_milli / 1000.0, sessions_free,
               sessions_free == 1 ? "" : "s", C_R);
    else
        printf("  %sspeed not yet measured on this host · %u session%s free%s\n",
               C_DIM, sessions_free, sessions_free == 1 ? "" : "s", C_R);
    printf("  %s%sthe host runs the model and therefore receives the text of "
           "this conversation%s\n", C_CORAL, C_BOLD, C_R);
    return 0;
}

/* ---- chat --------------------------------------------------------------- */

static int resolve_engine(const char *engines_dir, const char *engine_path,
                          const char *model_type, char *out, size_t cap) {
    if (engine_path) {
        if (checked_printf(out, cap, "%s", engine_path)) return -1;
        return access(out, X_OK);
    }
    const LmbModelFamily *family = lumi_family(model_type);
    const char *eng = family ? family->engine : NULL;
    if (!eng) { if (cap) out[0] = 0; return -1; }
    const char *dir = engines_dir ? engines_dir : "../moe-stream/c";
    /* prefer the P2P build when one exists: it is the same engine (identical
     * without expert peers) plus the ability to run the routed experts on
     * the swarm when the tracker offers executors */
    char me[1200];
    exe_dir(me, sizeof me);
    if (family->p2p_engine) {
        if (checked_printf(out, cap, "%s/%s_p2p", dir,
                           family->p2p_engine)) return -1;
        if (access(out, X_OK) == 0) return 0;
        if (checked_printf(out, cap, "%s/%s_p2p", me,
                           family->p2p_engine)) return -1;
        if (access(out, X_OK) == 0) return 0;
    }
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
    printf("  %smirror for %s in %s: %.0f GB free. Stores only accessed blocks "
           "— dense weights always, experts when no peer executes them "
           "(up to %.0f GB)%s\n",
           C_DIM, model, cache, free_gb, need_gb, C_R);
    /* the dense part is the floor; a tenth of the model is a generous guess
     * at it, and below that even a warm phase-2 chatter cannot boot */
    if (free_gb < need_gb * 0.1)
        printf("  %s⚠ %.0f GB free may not be enough for dense weights. "
               "If the model is already on this disk, use `--local DIR`: chat "
               "reads it in place without copying it.%s\n",
               C_RED, free_gb, C_R);
    else if (free_gb < need_gb)
        printf("  %sNot enough space for the full model: this can work while "
               "peers execute experts, but if they leave, chat may "
               "stop when the disk fills.%s\n", C_DIM, C_R);
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


#include "src/ui/lumabri_chat_editor.h"

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
    if (!any) { snprintf(out, cap, "%s", "(empty)"); return 1; }
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

/* How many expert executors the tracker knows for this model right now
 * (origin's --cache executor and resident donors alike). -1 = no answer. */
static int swarm_executors(const char *tracker, const char *model) {
    LmbMsg em = {0};
    LmbBuf eb = {0};
    lmb_buf_str(&eb, model);
    int nexec = -1;
    if (!lmb_request(tracker, LMB_EPEERS, eb.p, (uint32_t)eb.len, &em) &&
        em.op == LMB_EPEERS_R) {
        LmbCur ec = { em.body, em.body_len, 0 };
        uint32_t n = 0;
        if (!lmb_cur_u32(&ec, &n)) nexec = (int)n;
    }
    free(eb.p);
    lmb_msg_free(&em);
    return nexec;
}

/* Prefer one tracker-assigned Segment slice when this chat is already using
 * Segment. Public machines advertise direct TCP; NAT peers expose the same
 * local listener only through their signed outbound tracker tunnel. */
static int role_start_segment(const Role *r, const char *tracker,
                              const char *model, const char *model_type,
                              int context, uint64_t model_bytes) {
    const char *engine = segment_engine_for(model_type ? model_type : "");
    if (!engine) return 0;
    char dir[1024], bin[1200], shim[1200];
    exe_dir(dir, sizeof dir);
    snprintf(bin, sizeof bin, "%s/segment_node", dir);
    if (access(bin, X_OK)) return 0;
    snprintf(shim, sizeof shim, "%s/" LMB_SHIM_NAME, dir);
    if (access(shim, R_OK))
        snprintf(shim, sizeof shim, "%s/../lib/lumabri/" LMB_SHIM_NAME, dir);

    char host[INET_ADDRSTRLEN] = "";
    int relay_only = 0;
    const char *forced = getenv("LUMABRI_ADVERTISE");
    struct in_addr forced_address;
    if (forced && forced[0]) {
        if (strlen(forced) >= sizeof host ||
            inet_pton(AF_INET, forced, &forced_address) != 1)
            return 0;
        snprintf(host, sizeof host, "%s", forced);
    }
    else if (!strncmp(tracker, "127.0.0.1:", 10) ||
             !strncmp(tracker, "localhost:", 10))
        snprintf(host, sizeof host, "127.0.0.1");
    else if (machine_public_ipv4(host, sizeof host)) {
        snprintf(host, sizeof host, "127.0.0.1");
        relay_only = 1;
    }

    /* The executor receives its actual range before applying this budget.
     * This process therefore only publishes what the machine can donate;
     * segment_node compares it with the assigned range and releases the
     * promise immediately when it cannot fit. */
    LmbMachineProfile profile;
    (void)lmb_machine_probe(&profile,
                            r->model_dir[0] ? r->model_dir : ".", tracker);
    uint64_t available = profile.ram_available_bytes;
    uint64_t reserve = (uint64_t)lmb_env_int(
        getenv("LUMABRI_SEGMENT_RAM_RESERVE_MB") ?
        "LUMABRI_SEGMENT_RAM_RESERVE_MB" : "LUMABRI_RAM_RESERVE_MB",
        (int)lmb_machine_default_reserve_mb(profile.ram_total_bytes), 256, 262144) << 20;
    if (!available || reserve >= available)
        return 0;
    uint64_t memory_budget = available - reserve;

    char probe[1100];
    snprintf(probe, sizeof probe, "%s/config.json", r->model_dir);
    int local = r->model_dir[0] && access(probe, R_OK) == 0;
    if (!local && access(shim, R_OK)) return 0;

    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char vroot[1024], cachedir[1024], casdir[1040];
    char e_pre[1216], e_vr[1040], e_ca[1040], e_cs[1056];
    char e_tr[160], e_mo[96];
    char *envv[8];
    int ne = 0;
    if (!local) {
        const char *ve = getenv("LUMABRI_VROOT");
        const char *ce = getenv("LUMABRI_CACHE");
        if (ve && ve[0]) snprintf(vroot, sizeof vroot, "%s", ve);
        else snprintf(vroot, sizeof vroot, "%s/.lumabri/%s/vroot", home, model);
        if (ce && ce[0]) snprintf(cachedir, sizeof cachedir, "%s", ce);
        else snprintf(cachedir, sizeof cachedir, "%s/.lumabri/%s/cache", home, model);
        snprintf(casdir, sizeof casdir, "%s/.lumabri/cas", home);
        mkdir_p(cachedir);
        snprintf(e_pre, sizeof e_pre, LMB_PRELOAD_ENV "=%s", shim);
        snprintf(e_vr, sizeof e_vr, "LUMABRI_VROOT=%s", vroot);
        snprintf(e_ca, sizeof e_ca, "LUMABRI_CACHE=%s", cachedir);
        snprintf(e_cs, sizeof e_cs, "LUMABRI_CAS=%s", casdir);
        snprintf(e_tr, sizeof e_tr, "LUMABRI_TRACKER=%s", tracker);
        snprintf(e_mo, sizeof e_mo, "LUMABRI_MODEL=%s", model);
        envv[ne++] = e_pre; envv[ne++] = e_vr; envv[ne++] = e_ca;
        if (!getenv("LUMABRI_CAS")) envv[ne++] = e_cs;
        envv[ne++] = e_tr; envv[ne++] = e_mo; envv[ne] = NULL;
    }

    int port = free_port(7801);
    if (!port) return 0;
    long cores = profile.physical_cores;
    int threads = cores > 1 ? (int)(cores / 2) : 1;
    int sessions = 2;
    int run_queue = lmb_env_int("LUMABRI_SEGMENT_RUN_QUEUE", 32, 0, 256);
    int run_wait_ms = lmb_env_int("LUMABRI_SEGMENT_RUN_WAIT_MS",
                                  30000, 50, 60000);
    char port_text[16], context_text[16], sessions_text[16], threads_text[16];
    char run_queue_text[16], run_wait_text[16];
    char memory_text[32], model_bytes_text[32], preflight_text[32];
    char address[80], name[64], base[48];
    snprintf(port_text, sizeof port_text, "%d", port);
    snprintf(context_text, sizeof context_text, "%d", context);
    snprintf(sessions_text, sizeof sessions_text, "%d", sessions);
    snprintf(threads_text, sizeof threads_text, "%d", threads);
    snprintf(run_queue_text, sizeof run_queue_text, "%d", run_queue);
    snprintf(run_wait_text, sizeof run_wait_text, "%d", run_wait_ms);
    snprintf(memory_text, sizeof memory_text, "%llu",
             (unsigned long long)(memory_budget >> 20));
    snprintf(model_bytes_text, sizeof model_bytes_text, "%llu",
             (unsigned long long)model_bytes);
    snprintf(address, sizeof address, "%s:%d", host, port);
    donor_base_name(r, base, sizeof base);
    snprintf(name, sizeof name, "%s-segment-%d", base, port);
    int ready_pipe[2];
    if (lmb_ready_pipe(ready_pipe)) return 0;
    int fd_flags = fcntl(ready_pipe[1], F_GETFD);
    if (fd_flags < 0 || fcntl(ready_pipe[1], F_SETFD,
                              fd_flags & ~FD_CLOEXEC)) {
        close(ready_pipe[0]); close(ready_pipe[1]); return 0;
    }
    snprintf(preflight_text, sizeof preflight_text, "%d", ready_pipe[1]);
    char *argv[42];
    int a = 0;
    argv[a++] = bin;
    argv[a++] = "--engine";        argv[a++] = (char *)engine;
    argv[a++] = "--model-dir";     argv[a++] = local ? (char *)r->model_dir : vroot;
    argv[a++] = "--model";         argv[a++] = (char *)model;
    argv[a++] = "--auto-range";
    argv[a++] = "--port";          argv[a++] = port_text;
    argv[a++] = "--tracker";       argv[a++] = (char *)tracker;
    argv[a++] = "--advertise";     argv[a++] = address;
    argv[a++] = "--name";          argv[a++] = name;
    argv[a++] = "--auto-identity";
    if (relay_only) argv[a++] = "--relay-only";
    argv[a++] = "--context";       argv[a++] = context_text;
    argv[a++] = "--max-rows";      argv[a++] = "16";
    argv[a++] = "--sessions";      argv[a++] = sessions_text;
    argv[a++] = "--threads";       argv[a++] = threads_text;
    argv[a++] = "--run-queue";     argv[a++] = run_queue_text;
    argv[a++] = "--run-wait-ms";   argv[a++] = run_wait_text;
    argv[a++] = "--memory-limit-mb"; argv[a++] = memory_text;
    argv[a++] = "--model-bytes";   argv[a++] = model_bytes_text;
    argv[a++] = "--preflight-fd";  argv[a++] = preflight_text;
    argv[a] = NULL;
    if (g_nchildren >= MAX_CHILDREN) {
        close(ready_pipe[0]); close(ready_pipe[1]); return 0;
    }
    char logpath[1200];
    pid_t pid = spawn_argv_logged(argv, local ? NULL : envv,
                                  donor_log_path(name, logpath,
                                                 sizeof logpath));
    close(ready_pipe[1]);
    if (pid <= 0) { close(ready_pipe[0]); return 0; }
    struct pollfd ready_poll = { .fd = ready_pipe[0], .events = POLLIN | POLLHUP };
    char readiness = 0;
    int polled;
    do polled = poll(&ready_poll, 1, 10000); while (polled < 0 && errno == EINTR);
    ssize_t readiness_bytes = polled > 0
                            ? read(ready_pipe[0], &readiness, 1) : 0;
    close(ready_pipe[0]);
    if (readiness_bytes != 1 || readiness != 'P') {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
        printf("  %sthe assigned Segment slice exceeds the RAM budget; "
               "switching to expert sharing%s\n", C_DIM, C_R);
        return 0;
    }
    child_publish(g_nchildren++, pid);
    printf("  %s\xe2\x9c\xa6 executing a tracker-assigned Segment slice "
           "for the swarm%s %s(low priority, %d sessions maximum%s)%s\n",
           C_GRN, C_R, C_DIM, sessions,
           relay_only ? ", relay NAT automatico" : ", P2P diretto + relay",
           C_R);
    return 1;
}

/* Start whatever was chosen. Never fatal: a donation that cannot start is a
 * missed contribution, not a reason to refuse someone a conversation. */
static void role_start(const Role *r, const char *tracker, const char *model,
                       const char *model_type, int segment_active, int context,
                       uint64_t model_bytes) {
    char dir[1024];
    exe_dir(dir, sizeof dir);

    if (r->disk) {
        char bin[1200], portstr[16], gbstr[32], name[64];
        snprintf(bin, sizeof bin, "%s/maintainer", dir);
        int port = free_port(7601);   /* clear of the test ranges */
        if (!port || access(bin, X_OK)) {
            printf("  %scannot start the maintainer: disk sharing skipped%s\n",
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
              if (np > 0) child_publish(g_nchildren++, np); }
            printf("  %s\xe2\x9c\xa6 sharing %.0f GB of %s%s%s%s: the tracker assigns "
                   "the rarest files%s\n",
                   C_GRN, r->gb, C_R, C_BOLD, model, C_DIM, C_R);
        }
    }

    if (r->compute) {
        int compute_children_before = g_nchildren;
        char lease_owner[256] = "";
        int lease = lmb_machine_compute_lease_acquire(
            model, tracker, lease_owner, sizeof lease_owner);
        if (lease < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                printf("  %scompute sharing is already active on this machine%s%s%s; "
                       "not starting another executor that would reserve the "
                       "same RAM%s\n", C_DIM,
                       lease_owner[0] ? " (" : "",
                       lease_owner[0] ? lease_owner : "",
                       lease_owner[0] ? ")" : "", C_R);
            else
                printf("  %scannot acquire the donor RAM lease: %s; "
                       "compute sharing skipped%s\n", C_DIM, strerror(errno), C_R);
        } else {
            g_compute_lease_fd = lease;
            /* Donate what chatters use. The chat runs the expert swarm
             * whenever an expert executor exists, so a donor that took a
             * Segment range there served nobody while its RAM could have
             * held a few hundred resident experts. Segment is the donation
             * only when no expert executor exists for the model. */
            int prefer_segment = segment_active &&
                                 swarm_executors(tracker, model) <= 0;
            int segment_started = prefer_segment &&
                role_start_segment(r, tracker, model, model_type, context,
                                   model_bytes);
            if (!segment_started) {
                const char *node = expert_node_for(model_type ? model_type : "");
                char bin[1200];
                snprintf(bin, sizeof bin, "%s/%s", dir,
                         node ? node : "expert_node");
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
        snprintf(shim, sizeof shim, "%s/" LMB_SHIM_NAME, dir);
        if (access(shim, R_OK))
            snprintf(shim, sizeof shim, "%s/../lib/lumabri/" LMB_SHIM_NAME, dir);
        if (!node || !port || access(bin, X_OK)) {
            printf("  %sno expert node for %s (make engines): compute sharing "
                   "skipped%s\n", C_DIM, model_type ? model_type : "?", C_R);
        } else if (!local && access(shim, R_OK)) {
            printf("  %sliblumabri.so mancante (make): compute sharing skipped%s\n",
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
                snprintf(e_pre, sizeof e_pre, LMB_PRELOAD_ENV "=%s", shim);
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
            snprintf(g_donor_base, sizeof g_donor_base, "%s", base);
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
            argv[a++] = "--hold";       argv[a++] = "auto";   /* the tracker completes the least-covered layers first, then grows replicas (keep limit 2); the node sizes the slice to free RAM */
            argv[a] = NULL;
            { char lp[1200];
              pid_t np = spawn_argv_logged(argv, local ? NULL : envv,
                                           donor_log_path(name, lp, sizeof lp));
              if (np > 0) child_publish(g_nchildren++, np); }
            if (local)
                printf("  %s\xe2\x9c\xa6 executing experts for the swarm%s %s(%s · "
                       "logs in ~/.lumabri/logs; use /debug to view them)%s\n",
                       C_GRN, C_R, C_DIM, node, C_R);
            else
                printf("  %s\xe2\x9c\xa6 executing experts for the swarm%s %s(%s, "
                       "the assigned slice comes from the swarm · /debug for logs)%s\n",
                       C_GRN, C_R, C_DIM, node, C_R);
            }
            }
            if (g_nchildren == compute_children_before) {
                close(g_compute_lease_fd);
                g_compute_lease_fd = -1;
            }
        }
    }
    if (r->disk || r->compute)
        printf("  %swhile this chat remains open. For a donor that "
               "outlives the session: lumabri serve --join%s\n", C_DIM, C_R);
}

/* Kimi K3's HF repo ships tiktoken.model but no tokenizer.json; the engine
 * refuses to serve without one and points at a python tool most users have
 * never heard of. When the model is local and the tool sits right there in
 * the engine checkout, run it — a setup step the machine can do is not the
 * user's job. */
static void kimi_tokenizer_selfheal(const char *model_dir, const char *engines_dir) {
    char tok[1100], tool[1200], cmd[4096];
    snprintf(tok, sizeof tok, "%s/tokenizer.json", model_dir);
    if (access(tok, R_OK) == 0) return;
    if (!engines_dir || !engines_dir[0]) return;
    snprintf(tool, sizeof tool, "%s/tools/k3_tokenizer.py", engines_dir);
    if (access(tool, R_OK)) return;
    printf("  %skimi: checkpoint has no tokenizer.json — generating it with "
           "k3_tokenizer.py…%s\n", C_DIM, C_R);
    snprintf(cmd, sizeof cmd, "python3 '%s' '%s' -o '%s' >/dev/null 2>&1",
             tool, model_dir, tok);
    if (system(cmd) == 0 && access(tok, R_OK) == 0)
        printf("  %s\xe2\x9c\x93 tokenizer.json generated in %s%s\n",
               C_DIM, model_dir, C_R);
    else
        printf("  %s✗ could not generate it (python3 required). "
               "Manual command:\n    python3 %s %s -o %s%s\n",
               C_RED, tool, model_dir, tok, C_R);
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
        printf("  %slocal model %s%s%s%s · no network, no mirror%s\n",
               C_DIM, C_R, C_BOLD, local_dir, C_DIM, C_R);
        const LmbModelFamily *family = lmb_family_for(mtype);
        if (family && !strcmp(family->segment_id, "kimi"))
            kimi_tokenizer_selfheal(local_dir, engines_dir);
    } else {
        printf("  %sasking the swarm who has %s…%s\n", C_DIM, model, C_R);
        if (swarm_inspect(tracker, model, sw)) {
            printf("  %smodel %s: nobody on the swarm has it%s\n", C_RED, model, C_R);
            return -1;
        }
        snprintf(mtype, sizeof mtype, "%s", sw->model_type);
        printf("  %s%d files · %.0f GB · %d peers · type %s%s\n", C_DIM,
               sw->nfiles, (double)sw->total_bytes / 1e9, sw->npeers,
               mtype[0] ? mtype : "?", C_R);
        disk_preflight(model, sw->total_bytes);
    }

    /* Prefer a complete Segment route when the optional runtime is installed.
     * The probe is the real engine boot: Edge files arrive through the same
     * signed mirror, then discovery either publishes READY or exits.  Exiting
     * before READY is an ordinary coverage miss, not a chat failure; the
     * unchanged expert/local engine starts immediately afterwards. */
    /* The expert swarm is the data plane of this project: dense weights and
     * the KV cache stay here, every routed expert runs on whichever peer
     * holds it in RAM, and each machine that joins adds resident experts and
     * therefore speed. A Segment chain keeps whole layer ranges and their
     * state on a few large peers instead, where a small donor cannot help
     * at all. So Segment is tried only when nobody executes experts for this
     * model (or when the operator insists with LUMABRI_SEGMENT_REQUIRED);
     * otherwise the expert path starts at once. */
    int executors = local_dir ? -1 : swarm_executors(tracker, model);
    if (executors > 0 && !getenv("LUMABRI_SEGMENT_REQUIRED"))
        printf("  %s%d expert executor%s in the swarm: dense weights and KV stay "
               "here; experts run on peers%s\n", C_DIM, executors,
               executors == 1 ? "" : "s", C_R);
    if (!local_dir && !getenv("LUMABRI_NO_SEGMENT") &&
        (executors <= 0 || getenv("LUMABRI_SEGMENT_REQUIRED"))) {
        char self[1024], segment_bin[1200];
        exe_dir(self, sizeof self);
        snprintf(segment_bin, sizeof segment_bin, "%s/segment_chat", self);
        if (access(segment_bin, X_OK) == 0 &&
            segment_engine_for(mtype) != NULL) {
            printf("  %sno expert executor: trying a complete Segment chain "
                   "with expert/CAS fallback if permitted%s\n", C_DIM, C_R);
            if (!segment_engine_spawn(segment_bin, shim, tracker, model,
                                      mtype, ctx, e)) {
                g_eng.booting = 1;
                g_eng.last_out = nowd();
                g_eng.spinning = 1;
                pthread_t segment_spinner;
                if (g_tty)
                    pthread_create(&segment_spinner, NULL, spinner_thread,
                                   (void *)"finding compatible segments");
                double segment_started = nowd();
                int segment_ready = engine_wait_ready(e);
                g_eng.spinning = 0;
                if (g_tty) pthread_join(segment_spinner, NULL);
                g_eng.booting = 0;
                if (!segment_ready) {
                    e->proto = PROTO_SERVE2;
                    printf("  %s\xe2\x9c\x93 %s ready via Segment in %.1fs%s%s "
                           "\xc2\xb7 Edge in CAS \xc2\xb7 /swarm /experts /model /debug "
                           "/storage /reset /quit%s\n",
                           C_GRN, model, nowd() - segment_started, C_R,
                           C_DIM, C_R);
                    if (e->greedy_only)
                        printf("  greedy decoding · this backend does not expose sampling logits\n");
                    return 0;
                }
                engine_stop(e);
                if (getenv("LUMABRI_SEGMENT_REQUIRED")) {
                    printf("  %sSegment required but unavailable%s\n",
                           C_RED, C_R);
                    return -1;
                }
                printf("  %sno complete Segment chain: continuing with "
                       "the expert/CAS path%s\n", C_DIM, C_R);
            }
        }
    }

    if (!local_dir && getenv("LUMABRI_SEGMENT_REQUIRED")) {
        fprintf(stderr, "The accepted plan requires Segment; no local or Expert fallback is permitted.\n");
        return -1;
    }
    const char *expected_engine = engine_for(mtype);
    EngKind expected_kind = engine_kind_of(expected_engine);
    if (!expected_engine || expected_kind == EK_INVALID) {
        printf("  %sunsupported model_type: %s%s\n", C_RED,
               mtype[0] ? mtype : "(missing)", C_R);
        return -1;
    }
    char engine[1200];
    if (resolve_engine(engines_dir, engine_path, mtype, engine, sizeof engine)) {
        printf("  %sengine not found: %s%s\n"
               "  point me at a colibri build with --engine or --engines-dir\n",
               C_RED, engine, C_R);
        return -1;
    }
    printf("  %sengine %s%s\n", C_DIM, engine, C_R);
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
            printf("\n  %s⚠ this is not a P2P engine build: expert weights will be "
                   "downloaded instead of delegated for execution%s\n"
                   "  %s%d peers are ready to execute them. Use %s_p2p, "
                   "built with:  make chatters ENGINE=/path/to/colibri/c%s\n\n",
                   C_RED, C_R, C_DIM, nexec,
                   p2p_engine_for(mtype) ? p2p_engine_for(mtype) : "(none)", C_R);
    }
    if (!local_dir)
        printf("  %sdownloading dense weights once — experts "
               "remain in the swarm%s\n", C_DIM, C_R);

    if (engine_spawn(engine, shim, tracker, model, local_dir,
                     ctx, max_new, cap_experts, expected_kind, e)) return -1;

    g_eng.booting = 1;
    g_eng.last_out = nowd();
    g_eng.spinning = 1;
    pthread_t tspin;
    if (g_tty) pthread_create(&tspin, NULL, spinner_thread, (void *)"warming up the swarm");
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
        printf("  %s✗ the engine failed to become ready%s\n", C_RED, C_R);
        engine_diag(e, 1);
        engine_stop(e);
        return -1;
    }
    if (!local_dir) wait_dense_ready();
    printf("  %s\xe2\x9c\x93 %s ready in %.1fs%s%s · net %.0f MB · "
           "/swarm /experts /model /debug /storage /reset /quit%s\n",
           C_GRN, model, nowd() - t0, C_R, C_DIM, g_eng.net_mb, C_R);
    return 0;
}

static int cmd_chat(int argc, char **argv) {
    g_stopping = 0;
    install_chat_signal_handlers();
    const char *tracker = NULL;
    const char *engine_path = NULL, *engines_dir = getenv("LUMABRI_ENGINES");
    const char *want_model = NULL, *local_dir = NULL;
    const char *role_arg = NULL, *model_dir_arg = NULL;
    const char *donor_name_arg = NULL, *host_addr = NULL, *host_key = NULL;
    double donate_gb = 0;
    int max_new = 256, ctx = 2048, cap_experts = 64, quick_probe = 0;
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
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host_addr = argv[++i];
        else if (!strcmp(argv[i], "--host-key") && i + 1 < argc) host_key = argv[++i];
        else if (!strcmp(argv[i], "--calibrate")) quick_probe = 1;
        else if (!strcmp(argv[i], "--plain")) g_tty = 0;
        else { fprintf(stderr, "usage: lumabri chat [--tracker H:P] [--model NAME] "
                               "[--local DIR] [--engine BIN] [--engines-dir DIR]\n"
                               "                    [--host HOST:PORT] "
                               "[--max-new N] [--ctx N] [--cap N]\n"
                               "                    [--role chat|disk|compute|all] "
                               "[--donate GB] [--model-dir DIR] [--donor-name S]\n");
               return 2; }
    }
    if (quick_probe && (!host_addr || !g_recording_calibration || !g_calibration_directory)) {
        fprintf(stderr, "Quick calibration requires an approved Hosted plan. No local engine was started.\n");
        return 2;
    }
    g_chat_term_valid = g_tty && isatty(STDIN_FILENO) &&
                        tcgetattr(STDIN_FILENO, &g_chat_term) == 0;

    /* Explicit CLI flags retain saved settings. Household interaction
     * belongs to cmd_home(); sharing is never implicit. */
    Cfg cfg;
    cfg_load(&cfg);
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
    if (host_addr) {
        /* A hosted client asks the swarm nothing: it does not need a tracker,
         * a model list or an engine, and requiring one would put a whole
         * discovery step in front of the case that is supposed to have none.
         * The host names the model in its greeting. */
        snprintf(model, sizeof model, "%s", want_model ? want_model : "hosted");
    } else if (local_dir) {
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
    snprintf(shim, sizeof shim, "%s/" LMB_SHIM_NAME, dir);
    if (access(shim, R_OK))       /* installed layout: bin/../lib/lumabri/ */
        snprintf(shim, sizeof shim, "%s/../lib/lumabri/" LMB_SHIM_NAME, dir);
    if (!host_addr && !local_dir && access(shim, R_OK)) {
        fprintf(stderr, "%s missing; run make (or make install)\n", LMB_SHIM_NAME);
        return 1;
    }

    Swarm sw;
    Engine eng = {0};

    printf("\n  %s✻ lumabri%s   %sconversation · %s%s\n", C_CORAL, C_R, C_DIM, model, C_R);
    if (nmodels > 1) {
        printf("  %s%d models in the swarm:%s", C_DIM, nmodels, C_R);
        for (int i = 0; i < nmodels; i++) printf(" %s%s%s", C_BOLD, models[i], C_R);
        printf("  %s(/model to switch)%s\n", C_DIM, C_R);
    }
    printf("\n");

    /* the role, before the engine boots: a donor started now warms up while
     * the dense weights cross the wire, instead of after */
    Role role = {0};
    if (donor_name_arg)
        snprintf(role.donor_name, sizeof role.donor_name, "%s", donor_name_arg);
    /* A hosted chat is never also a donor: donating means holding weights,
     * which is the one thing a thin client has said it will not do. The
     * question is not asked, so it cannot be answered by accident. */
    if (!host_addr && !local_dir && role_arg) {
        char bad[40];
        if (role_unknown(role_arg, bad, sizeof bad)) {
            fprintf(stderr, "--role: unknown value \"%s\". Expected chat, disk, "
                            "compute or all (combinations allowed: "
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
    } else {
        /* Sharing requires --role explicitly, or household donor approval. */
    }

    /* A hosted chat boots no engine. Everything model_boot would do —
     * inspect the swarm, size the mirror, start the shim, open Colibri — is
     * skipped, which is exactly what "zero bytes of the checkpoint" means
     * and the only way to actually mean it. */
    if (host_addr) {
        memset(&sw, 0, sizeof sw);
        if (host_connect(host_addr, NULL, host_key, &eng, &max_new)) return 1;
        if (g_execution_view) lmb_execution_print(stdout, g_execution_view);
    } else if (model_boot(tracker, model, shim, engines_dir, engine_path,
                          local_dir, ctx, max_new, cap_experts, &eng, &sw))
        return 1;
    if (quick_probe) {
        if (eng.proto != PROTO_SERVE2 || !eng.numeric_abi) {
            fprintf(stderr, "This host cannot provide a verified quick measurement.\n");
            engine_stop(&eng); return 1;
        }
        if (max_new > (int)LMB_QUICK_PROBE_TOKENS) max_new = (int)LMB_QUICK_PROBE_TOKENS;
        printf("  Quick calibration: at most %u tokens, %.0f seconds of inference. Ctrl-C cancels.\n",
               LMB_QUICK_PROBE_TOKENS, LMB_QUICK_PROBE_SECONDS);
    }
    if (!host_addr && (role.disk || role.compute)) {
        role_start(&role, tracker, model, sw.model_type, eng.segment, ctx,
                   sw.total_bytes);
    }

    char *conv = calloc(1, 1);   /* serve-codec conversation history (after bos) */
    int chat_failed = 0, probe_recorded = 0;
    LmbProbeDeadline probe_deadline = {0};
    char line[4096];
    for (;;) {
        if (g_stopping) break;
        int queued = quick_probe ? 0 : live_take_pending(line, sizeof line);
        int w = term_w() - 2;
        if (g_tty) {
            printf("\n");
            char status[200] = "";
            if (g_donor_base[0]) donor_status_line(status, sizeof status);
            printf("  %sYou%s\n", C_DIM, C_R);
            hline_text(" ", " ", w, status);
            printf("\n  %s%s\xe2\x80\xba%s ", C_CORAL, C_BOLD, C_R);
        } else
            printf("\n> ");
        fflush(stdout);
        int got;
        if (quick_probe) {
            snprintf(line, sizeof line, "Describe a calm sea in one sentence.");
            printf("%s\r\n", line);
            got = 1; /* a disclosed synthetic turn, never added to editor history */
        } else if (queued) {
            printf("%s\r\n", line);
            le_hist_push(line);
            got = 1;
        } else {
            g_slash_completion = 1;
            got = prompt_line(line, sizeof line) == 0;
            g_slash_completion = 0;
        }
        if (g_tty) hline(" ", " ", w);
        if (!got || g_stopping) break;
        size_t L = strlen(line);   /* prompt_line already stripped the newline */
        if (!L) continue;
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
        if (!strcmp(line, "/swarm")) { render_swarm(tracker); continue; }
        if (!strcmp(line, "/hosts")) { render_swarm(tracker); continue; }
        if (!strcmp(line, "/experts")) { render_experts(tracker); continue; }
        if (!strcmp(line, "/debug")) { render_debug(); continue; }
        if (!strcmp(line, "/plan")) { lmb_execution_print(stdout, g_execution_view); continue; }
        if (!strcmp(line, "/storage")) { render_storage(); continue; }
        if (!strcmp(line, "/help")) { render_help(); continue; }
        if (!strncmp(line, "/model", 6)) {
            const char *arg = line + 6;
            while (*arg == ' ') arg++;
            if (host_addr) {
                printf("  Hosted model: %s. Return to the catalogue to request a different plan.\n", model);
                continue;
            }
            if (local_dir) { printf("  %s--local: one model only%s\n", C_DIM, C_R); continue; }
            nmodels = swarm_models(tracker, models, 16);
            if (!*arg) {
                printf("  %smodels:%s", C_DIM, C_R);
                for (int i = 0; i < nmodels; i++)
                    printf(" %s%s%s%s", strcmp(models[i], model) ? "" : "*",
                           C_BOLD, models[i], C_R);
                printf("  %s/model <name> to switch%s\n", C_DIM, C_R);
                continue;
            }
            if (!strcmp(arg, model)) { printf("  %salready using %s%s\n", C_DIM, model, C_R); continue; }
            if (checked_printf(model, sizeof model, "%s", arg)) {
                printf("  %smodel name too long%s\n", C_RED, C_R);
                continue;
            }
            engine_stop(&eng);
            if (model_boot(tracker, model, shim, engines_dir, engine_path, local_dir,
                           ctx, max_new, cap_experts, &eng, &sw))
                return 1;
            continue;
        }

        if (line[0] == '/' && strcmp(line, "/reset")) {
            printf("  %sunknown command: %.40s \xc2\xb7 Tab completes commands, "
                   "/help lists them. Not sent to the model.%s\n", C_RED, line, C_R);
            continue;
        }
        int is_reset = !strcmp(line, "/reset");
        if (quick_probe && lmb_probe_start(&probe_deadline, eng.to, LMB_QUICK_PROBE_SECONDS)) {
            fprintf(stderr, "Cannot start the calibration deadline; no measurement was submitted.\n");
            chat_failed = 1; break;
        }
        if (eng.proto == PROTO_SERVE2) {
            /* the serve codec has no reset command; dropping the local history
             * (and starting a fresh bos) is the conversation reset. */
            if (is_reset) {
                free(conv); conv = calloc(1, 1);
                printf("  %s\xe2\x9c\xa6 new conversation%s\n", C_DIM, C_R);
                continue;
            }
            if (!conv || submit_serve2(&eng, conv, line, max_new)) { chat_failed = 1; break; }
        } else if (eng.proto == PROTO_FRAMED) {
            /* framed dialect: reset is a control byte, everything else is the
             * prompt line as-is */
            const char *send = is_reset ? "\x02RESET" : line;
            if (engine_write_full(eng.to, send, strlen(send))) break;
            if (engine_write_full(eng.to, "\n", 1)) break;
        } else {
            line[L] = '\n';
            if (engine_write_full(eng.to, line, L + 1)) break;
            line[L] = 0;
            if (is_reset) {
                printf("  %s\xe2\x9c\xa6 new conversation%s\n", C_DIM, C_R);
                continue;
            }
        }

        double m0 = g_eng.net_mb, r0 = nowd();
        char stat[512] = "";

        if (eng.proto == PROTO_SERVE2) {
            char *reply = NULL;
            printf("%s%s\xe2\x97\x86 %s%s\n  ", C_BOLD, C_CORAL, model, C_R);
            live_begin(tracker, model,
                       eng.segment ? "routing · finding the Segment chain" :
                                     "prefill · preparing the prompt");
            g_eng.streaming = 1;
            int dead = stream_serve2(&eng, stat, sizeof stat, &reply);
            g_eng.streaming = 0;
            live_end();
            if (quick_probe && lmb_probe_stop(&probe_deadline)) {
                fprintf(stderr, "Quick calibration reached its time limit. No new speed was saved.\n");
                free(reply); chat_failed = 1; break;
            }
            if (g_stopping) {
                free(reply);
                printf("\n  %sinference interrupted%s\n", C_DIM, C_R);
                break;
            }
            if (dead) {
                chat_failed = 1;
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
            live_begin(tracker, model, "prefill · swarm engine");
            g_eng.streaming = 1;
            int dead = stream_until_end(&eng, stat, sizeof stat);
            g_eng.streaming = 0;
            live_end();
            if (g_stopping) {
                printf("\n  %sinference interrupted%s\n", C_DIM, C_R);
                break;
            }
            if (dead) {
                chat_failed = 1;
                fprintf(stderr, "\n%sengine exited%s\n", C_RED, C_R);
                engine_diag(&eng, 0);
                break;
            }
            printf("\n");
            if (is_reset) { printf("  %s\xe2\x9c\xa6 new conversation%s\n", C_DIM, C_R); continue; }
        } else {
            int live = live_begin(tracker, model,
                                  "inference · local/expert engine");
            g_eng.spinning = !live;
            pthread_t tspin;
            if (g_tty && !live) pthread_create(&tspin, NULL, spinner_thread, NULL);
            char *reply = read_until_prompt(eng.from);
            g_eng.spinning = 0;
            if (live) live_end();
            else if (g_tty) pthread_join(tspin, NULL);
            if (g_stopping) {
                free(reply);
                printf("\n  %sinference interrupted%s\n", C_DIM, C_R);
                break;
            }
            if (!reply) {
                chat_failed = 1;
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
            printf("  %s\xe2\x9c\xa6 %d network log lines during the response — "
                   "/debug to view them%s\n", C_DIM, g_eng.deferred, C_R);
            g_eng.deferred = 0;
        }

        /* STAT <tokens> <tok/s> <cache hit%> <rss GB> */
        double tps = 0, hit = 0, rss = 0;
        int ntok = 0;
        unsigned prompt_count = 0;
        int nstat = sscanf(stat, "STAT %d %lf %lf %lf %u", &ntok, &tps, &hit, &rss, &prompt_count);
        double dmb = g_eng.net_mb - m0;
        double end = nowd(), first = g_first_token_at;
        LmbGenerationMetrics metrics;
        int metric_status=lmb_metrics_parse(stat,&metrics);
        if(metric_status==0 && metrics.generated_tokens!=(uint32_t)ntok) metric_status=-1;
        if (quick_probe && (metric_status || metrics.generated_tokens > LMB_QUICK_PROBE_TOKENS ||
                            lmb_metrics_decode_rate(&metrics) <= 0)) {
            fprintf(stderr, "Quick calibration did not produce enough valid timing data. No new speed was saved.\n");
            chat_failed = 1; break;
        }
        if(metric_status==0) {
            if (g_recording_calibration && g_calibration_directory && eng.numeric_abi &&
                nstat >= 5 && prompt_count && lmb_metrics_decode_rate(&metrics) > 0) {
                LmbCalibration record = *g_recording_calibration;
                record.key.adapter_abi = eng.numeric_abi;
                snprintf(record.key.numeric_class, sizeof record.key.numeric_class, "%s", eng.numeric_class);
                record.decode_tok_s = lmb_metrics_decode_rate(&metrics);
                record.ttft_seconds = first > r0 ? first - r0 : metrics.prefill_seconds;
                record.measured_at = (double)time(NULL); record.samples = 1;
                record.prompt_tokens = prompt_count; record.generated_tokens = metrics.generated_tokens;
                if (!lmb_cal_store(g_calibration_directory, &record)) {
                    *g_recording_calibration = record;
                    probe_recorded = 1;
                } else fprintf(stderr, "[lumabri] Could not save this measurement; the reply is unaffected.\n");
            }
            if (g_recording_calibration && !eng.numeric_abi)
                fprintf(stderr, "[calibration] No speed saved: the host did not supply its numeric ABI.\n");
            printf("%s  host prefill %.1fs · %u generated tokens",C_DIM,
                   metrics.prefill_seconds,metrics.generated_tokens);
            double rate=lmb_metrics_decode_rate(&metrics);
            if(rate>0) printf(" · decode %.2f tok/s",rate);
            else printf(" · decode speed not measured");
            if(first>r0) printf(" · first text %.1fs",first-r0);
        } else if(metric_status<0) {
            printf("%s  %.1fs · timing unavailable; no speed recorded",C_DIM,end-r0);
        } else if (first > r0 && ntok > 0 && end > first) {
            /* prefill and decode are two different costs: the prompt crossing
             * every layer once, then one network round per layer per token */
            double decode_s = end - first;
            printf("%s  prefill %.1fs · decode %d token in %.1fs · %.2f tok/s",
                   C_DIM, first - r0, ntok, decode_s, (double)ntok / decode_s);
        } else {
            printf("%s  %.1fs", C_DIM, end - r0);
            if (nstat >= 2 && tps > 0) printf(" · %.1f tok/s", tps);
        }
        if (nstat >= 4 && rss > 0) printf(" · %.1f GB resident", rss);
        if (host_addr)   printf(" · hosted stream · no local checkpoint");
        else if (local_dir) printf(" · local disk");
        else if (dmb > 0.5) printf(" · %.0f MB from swarm · mirror %.0f MB", dmb, g_eng.net_mb);
        else                printf(" · warm mirror, no weight transfers");
        printf("%s\n", C_R);
        if (quick_probe) {
            if (!probe_recorded) {
                fprintf(stderr, "Quick calibration could not save its measurement. The catalogue was not updated.\n");
                chat_failed = 1; break;
            }
            puts("  Quick calibration complete. Short-run indication, not a long-chat speed guarantee.");
            break;
        }
    }

    (void)lmb_probe_stop(&probe_deadline);
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
    if (g_compute_lease_fd >= 0) {
        close(g_compute_lease_fd);
        g_compute_lease_fd = -1;
    }
    if (g_nchildren) printf("  %sresource sharing stopped%s\n", C_DIM, C_R);
    printf("\n");
    return chat_failed;
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

/* ---- the catalogue -------------------------------------------------------
 *
 * What this collection of computers can actually run, model by model, with
 * the three states kept apart and no speed anywhere.
 *
 * The screen answers three questions and refuses a fourth. Can it run — as
 * resident weights, from disk, or not at all. What is missing — in gigabytes
 * and in machines, because gigabytes are not something a person can go and
 * buy. How would it be split — a range per computer. It does NOT say tok/s,
 * because a plan knows what fits and only a calibration knows what it does,
 * and printing them in the same voice is how a catalogue starts lying. */

#include "lumabri_checkpoint_inventory.h"

typedef struct {
    char dir[512];
    char name[64];
    LmbModelShape shape;
    int has_weights;
    uint64_t checkpoint_bytes;
    int inventory_ok;
} CatalogEntry;

static int catalog_scan(const char *root, CatalogEntry *out, int cap) {
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (e->d_name[0] == '.') continue;
        char path[832];
        snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        char cfg[960];
        snprintf(cfg, sizeof cfg, "%s/config.json", path);
        if (access(cfg, R_OK)) continue;
        if (lmb_shape_from_config(path, &out[n].shape)) continue;
        snprintf(out[n].dir, sizeof out[n].dir, "%.511s", path);
        snprintf(out[n].name, sizeof out[n].name, "%.63s", e->d_name);
        LmbCheckpointInventory inventory;
        out[n].inventory_ok = !lmb_checkpoint_inventory(path, &inventory);
        out[n].has_weights = inventory.has_weights;
        out[n].checkpoint_bytes = inventory.bytes;
        n++;
    }
    closedir(d);
    return n;
}

/* This machine as a planner node. The reserve is the governor's, so the
 * catalogue never plans into memory the governor would refuse to give. */
static void catalog_self(LmbClusterNode *n, LmbMachineProfile *profile,
                          const char *disk) {
    LmbMachineProfile p;
    memset(n, 0, sizeof *n);
    if (profile->logical_cpus) {
        p = *profile;
        lmb_machine_refresh_resources(&p, disk);
    } else if (lmb_machine_probe(&p, disk, NULL)) return;
    *profile = p;
    snprintf(n->name, sizeof n->name, "%s", p.hostname);
    uint64_t reserve = lmb_machine_ram_reserve();
    n->ram_budget_bytes = p.ram_available_bytes > reserve
                        ? p.ram_available_bytes - reserve : 0;
    n->vram_budget_bytes = p.vram_available_bytes;
    n->disk_read_bps = p.disk_read_bps;
    n->gpu_backends = p.gpu_backends;
    n->threads = p.logical_cpus;
    n->has_checkpoint = 0;      /* set per catalogue entry, never globally */
}

static void inventory_id_text(const uint8_t id[32], char out[65]) {
    for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", id[i]);
}

static int catalog_inventory(LmbTuiState *st) {
    memset(st->nodes, 0, sizeof st->nodes);
    memset(st->identities, 0, sizeof st->identities);
    memset(st->runtime_ids, 0, sizeof st->runtime_ids);
    memset(st->ages_ms, 0, sizeof st->ages_ms);
    catalog_self(&st->nodes[0], &st->profiles[0], st->disk[0] ? st->disk : ".");
    st->nnodes = 1;
    st->inventory_ok = 1;
    if (!st->tracker[0]) return 0;
    char path[1024]; uint8_t sk[64], pk[32];
    if (lmb_peer_identity(lmb_peer_key_path(path, sizeof path), sk, pk)) {
        st->inventory_ok = 0; return -1;
    }
    memset(sk, 0, sizeof sk);
    inventory_id_text(pk, st->identities[0]);
    LmbMachineReport reports[LMB_INVENTORY_MAX];
    uint32_t n = 0;
    if (lmb_inventory_fetch(st->tracker, reports, &n)) {
        st->inventory_ok = 0; return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        LmbMachineReport *r = &reports[i];
        uint32_t at;
        if (!memcmp(pk, r->identity, 32)) at = 0;
        else {
            if (st->nnodes == LMB_CLUSTER_MAX_NODES) {
                st->nnodes = 1; st->inventory_ok = 0; return -1;
            }
            at = st->nnodes++;
        }
        LmbClusterNode *node = &st->nodes[at];
        snprintf(node->name, sizeof node->name, "%s", r->machine.hostname);
        snprintf(node->addr, sizeof node->addr, "%s", r->control_addr);
        node->ram_budget_bytes = r->ram_budget_bytes;
        node->vram_budget_bytes = r->machine.vram_available_bytes;
        node->threads = r->machine.logical_cpus;
        if (r->runtime_threads && node->threads > r->runtime_threads) node->threads = r->runtime_threads;
        snprintf(st->runtime_ids[at], sizeof st->runtime_ids[at], "%s", r->runtime_id);
        /* Resource reporting is not executable Segment capability. No remote
         * card is promoted to a supported backend by an inventory advert. */
        node->gpu_backends = 0;
        node->disk_read_bps = r->machine.disk_read_bps;
        st->profiles[at] = r->machine;
        st->ages_ms[at] = r->age_ms;
        inventory_id_text(r->identity, st->identities[at]);
    }
    return 0;
}

static int catalog_calibration_dir(char out[1200]) {
    const char *home = getenv("HOME");
    return !home || checked_printf(out, 1200, "%s/.lumabri/calibrations", home);
}

static void catalog_hardware_id(const LmbMachineProfile *p, const char *address, char out[65]) {
    LmbSha sha; uint8_t digest[32]; lmb_sha_init(&sha);
    const char *fields[] = {p->os, p->arch, p->cpu_model, p->isa, address};
    for (unsigned i = 0; i < sizeof fields / sizeof *fields; i++)
        lmb_sha_update(&sha, fields[i], strlen(fields[i]) + 1);
    lmb_sha_le32(&sha, p->logical_cpus); lmb_sha_le32(&sha, p->physical_cores);
    lmb_sha_le32(&sha, p->numa_nodes); lmb_sha_le64(&sha, p->ram_total_bytes);
    lmb_sha_le32(&sha, p->gpu_count); lmb_sha_le64(&sha, p->vram_total_bytes);
    lmb_sha_final(&sha, digest); lmb_hex(out, digest, sizeof digest);
}

static int catalog_calibration_key(const LmbTuiState *st, const LmbTuiModel *m,
    uint32_t abi, const char *numeric, LmbCalKey *key) {
    memset(key, 0, sizeof *key);
    if (!m->planned || m->plan.state != LMB_PLAN_RESIDENT || !m->content_id[0] ||
        !st->build_id[0] || !m->plan.nslices || m->plan.nslices > LMB_CAL_NODES_MAX) return -1;
    snprintf(key->model_root, sizeof key->model_root, "%s", m->content_id);
    snprintf(key->adapter, sizeof key->adapter, "%s", m->shape.segment_id);
    snprintf(key->build_id, sizeof key->build_id, "%s", st->build_id);
    snprintf(key->plan_kind, sizeof key->plan_kind, "segment");
    snprintf(key->numeric_class, sizeof key->numeric_class, "%s", numeric ? numeric : "");
    key->adapter_abi = abi; key->goal = m->plan.goal;
    key->context = st->context; key->sessions = st->sessions; key->nodes = m->plan.nslices;
    int edge = 0;
    for (uint32_t i = 0; i < key->nodes; i++) {
        const LmbSlice *slice = &m->plan.slices[i]; uint32_t n = slice->node;
        if (n >= st->nnodes || !st->identities[n][0] || !st->runtime_ids[n][0]) return -1;
        if (n == m->plan.edge_node) { key->edge_node = i; edge = 1; }
        snprintf(key->node_id[i], sizeof key->node_id[i], "%s", st->identities[n]);
        snprintf(key->node_build_id[i], sizeof key->node_build_id[i], "%s", st->runtime_ids[n]);
        snprintf(key->node_backend[i], sizeof key->node_backend[i], "cpu");
        catalog_hardware_id(&st->profiles[n], st->nodes[n].addr, key->node_hardware_id[i]);
        key->layer_begin[i] = slice->layer_begin; key->layer_end[i] = slice->layer_end;
        key->threads[i] = st->nodes[n].threads > 256 ? 256 : st->nodes[n].threads;
    }
    /* ABI/numeric may be unknown until the real Edge greeting. All other
     * fields must already be bound; the writer validates the completed key. */
    return edge ? 0 : -1;
}

static void catalog_advice_refresh(LmbTuiState *st) {
    LmbAdviceCandidate candidates[LMB_TUI_MAX_MODELS] = {0};
    uint32_t advice[LMB_TUI_MAX_MODELS];
    for (int i = 0; i < st->nmodels; i++) {
        const LmbTuiModel *m = &st->models[i];
        LmbAdviceCandidate *c = &candidates[i];
        c->eligible = st->inventory_ok && m->planned && m->shape.sizing_verified &&
            m->weights_present && m->checkpoint_inventory_ok &&
            m->plan.state == LMB_PLAN_RESIDENT && m->plan.nslices;
        for (uint32_t j = 0; c->eligible && j < m->plan.nslices; j++) {
            uint32_t node = m->plan.slices[j].node;
            if (node >= st->nnodes || !lmb_tui_node_enabled(st, node)) c->eligible = 0;
            c->reserved_bytes = lmb_budget_add(c->reserved_bytes, m->plan.slices[j].bytes_resident);
        }
        c->checkpoint_bytes = m->checkpoint_bytes;
        if (m->has_calibration && m->calibration_key_valid && lmb_cal_valid(&m->calibration) &&
            lmb_cal_matches(&m->calibration.key, &m->calibration_key))
            c->measured_tok_s = m->calibration.decode_tok_s;
    }
    lmb_catalogue_advice(candidates, (size_t)st->nmodels, advice);
    for (int i = 0; i < st->nmodels; i++) st->models[i].advice_flags = advice[i];
}

static int catalog_state_refresh(LmbTuiState *st, void *unused) {
    (void)unused;
    /* Per-layer contracts make each entry larger. Do not place the entire
     * 64-model catalogue on a small native/thread stack. */
    CatalogEntry *found = calloc(LMB_TUI_MAX_MODELS, sizeof *found);
    if (!found) {
        st->nmodels = 0;
        st->inventory_ok = 0;
        return -1;
    }
    int n = catalog_scan(st->root, found, LMB_TUI_MAX_MODELS);
    st->nmodels = 0;
    (void)catalog_inventory(st);
    static LmbBinaryDigest self_digest;
    char bin_dir[1024], binary[1200], calibration_dir[1200]; uint8_t digest[32];
    exe_dir(bin_dir, sizeof bin_dir); st->build_id[0] = 0;
    if (!checked_printf(binary, sizeof binary, "%s/lumabri", bin_dir) &&
        !lmb_binary_digest(binary, &self_digest, digest)) lmb_hex(st->build_id, digest, 32);
    int have_cal_dir = !catalog_calibration_dir(calibration_dir);
    for (int i = 0; i < n; i++) {
        LmbTuiModel *m = &st->models[st->nmodels];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%.63s", found[i].name);
        snprintf(m->dir, sizeof m->dir, "%.511s", found[i].dir);
        m->shape = found[i].shape;
        m->weights_present = found[i].has_weights;
        m->checkpoint_bytes = found[i].checkpoint_bytes;
        m->checkpoint_inventory_ok = found[i].inventory_ok;
        st->nodes[0].has_checkpoint = found[i].has_weights;
        LmbClusterNode selected[LMB_CLUSTER_MAX_NODES];
        uint32_t mapping[LMB_CLUSTER_MAX_NODES], nselected = 0;
        int household = 0;
        for (uint32_t j = 0; j < st->nnodes; j++)
            if (st->nodes[j].addr[0]) household = 1;
        for (uint32_t j = 0; j < st->nnodes; j++) {
            if (household && !lmb_tui_node_enabled(st, j)) continue;
            selected[nselected] = st->nodes[j];
            mapping[nselected++] = j;
        }
        if (household && m->weights_present)
            m->planned = st->inventory_ok && m->checkpoint_inventory_ok &&
                !lmb_home_plan_source(&m->shape, m->checkpoint_bytes, selected,
                    nselected, st->context, st->sessions, LMB_GOAL_ONE_SESSION, 1, &m->plan);
        else
            m->planned = st->inventory_ok && !lmb_plan_cluster_source(&m->shape, selected,
                nselected, st->context, st->sessions, LMB_GOAL_ONE_SESSION, 0, &m->plan);
        if (m->planned) {
            m->plan.edge_node = mapping[m->plan.edge_node];
            for (uint32_t j = 0; j < m->plan.nslices; j++)
                m->plan.slices[j].node = mapping[m->plan.slices[j].node];
        }
        if (have_cal_dir && !lmb_checkpoint_identity(m->dir, m->content_id, NULL, NULL) &&
            !lmb_cal_load(calibration_dir, m->content_id, &m->calibration)) {
            m->has_calibration = 1;
            m->calibration_key_valid = !catalog_calibration_key(st, m, m->calibration.key.adapter_abi,
                m->calibration.key.numeric_class, &m->calibration_key) && lmb_cal_key_valid(&m->calibration_key);
        }
        st->nmodels++;
    }
    catalog_advice_refresh(st);
    free(found);
    return 0;
}

static void catalog_row(const LmbTuiModel *m) {
    LmbClusterPlan plan = m->plan;
    int ok = m->planned;
    const char *state = !ok ? "cannot plan"
                            : lmb_plan_state_name(plan.state);
    const char *mark = !ok || plan.state == LMB_PLAN_UNRUNNABLE ? "x"
                     : plan.state == LMB_PLAN_DISK ? "!" : "+";
    char detail[96] = "";
    if (!m->checkpoint_inventory_ok)
        snprintf(detail, sizeof detail, "checkpoint inventory unavailable");
    else if (!m->weights_present)
        snprintf(detail, sizeof detail, "checkpoint weights missing");
    else if (!m->shape.sizing_verified)
        snprintf(detail, sizeof detail, "adapter sizing unavailable");
    else if (ok && plan.state == LMB_PLAN_UNRUNNABLE && plan.missing_bytes)
        snprintf(detail, sizeof detail, "%.0f GB short (~%u more machine%s)",
                 (double)plan.missing_bytes / 1e9, plan.missing_nodes,
                 plan.missing_nodes == 1 ? "" : "s");
    else if (ok && plan.ready_known && plan.ready_seconds > 0)
        snprintf(detail, sizeof detail, "ready in ~%.0f min",
                 plan.ready_seconds / 60.0);
    else if (ok)
        snprintf(detail, sizeof detail, "%u slice%s", plan.nslices,
                 plan.nslices == 1 ? "" : "s");
    /* The speed column is deliberately not a number. It becomes one when a
     * calibration exists for THIS model on THIS cluster with THIS plan, and
     * not one moment earlier. */
    char speed[128] = "not calibrated";
    if (m->has_calibration) lmb_cal_speed_text(&m->calibration,
        m->calibration_key_valid ? &m->calibration_key : NULL, speed, sizeof speed);
    printf("  %s%-2s%s %-22s %-14s %-28s %s\n",
           mark[0] == 'x' ? C_RED : mark[0] == '!' ? C_CORAL : C_GRN,
           mark, C_R, m->name, state, detail, speed);
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(out, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\r') fputs("\\r", out);
        else if (*p == '\t') fputs("\\t", out);
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static void catalog_json(const LmbTuiState *st) {
    printf("{\"schema\":\"lumabri.models.v1\",\"inventory_ok\":%s,"
           "\"execution_ready\":false,\"nodes\":[", st->inventory_ok ? "true" : "false");
    for (uint32_t i = 0; i < st->nnodes; i++) {
        const LmbClusterNode *node = &st->nodes[i];
        const LmbMachineProfile *p = &st->profiles[i];
        if (i) fputc(',', stdout);
        fputs("{\"name\":", stdout); json_string(stdout, node->name);
        fputs(",\"identity\":", stdout); json_string(stdout, st->identities[i]);
        fputs(",\"cpu_model\":", stdout); json_string(stdout, p->cpu_model);
        fputs(",\"os\":", stdout); json_string(stdout, p->os);
        fputs(",\"arch\":", stdout); json_string(stdout, p->arch);
        fputs(",\"isa\":", stdout); json_string(stdout, p->isa);
        printf(",\"ram_budget_bytes\":%llu,\"ram_total_bytes\":%llu,"
               "\"ram_available_bytes\":%llu,\"vram_inventory_bytes\":%llu,"
               "\"threads\":%u,\"physical_cores\":%u,\"numa_nodes\":%u,"
               "\"gpu_detected\":%u,\"segment_gpu_verified\":false,"
               "\"disk_available_bytes\":%llu,\"disk_read_bps\":%llu,\"age_ms\":%u}",
               (unsigned long long)node->ram_budget_bytes,
               (unsigned long long)p->ram_total_bytes,
               (unsigned long long)p->ram_available_bytes,
               (unsigned long long)node->vram_budget_bytes,
               node->threads, p->physical_cores, p->numa_nodes, p->gpu_count,
               (unsigned long long)p->disk_available_bytes,
               (unsigned long long)node->disk_read_bps, st->ages_ms[i]);
    }
    fputs("],\"models\":[", stdout);
    for (int i = 0; i < st->nmodels; i++) {
        const LmbTuiModel *model = &st->models[i];
        const LmbClusterPlan *plan = &model->plan;
        int planned = model->planned;
        if (i) fputc(',', stdout);
        fputs("{\"name\":", stdout); json_string(stdout, model->name);
        fputs(",\"model_type\":", stdout);
        json_string(stdout, model->shape.model_type);
        fputs(",\"adapter\":", stdout);
        json_string(stdout, model->shape.segment_id);
        printf(",\"sizing_verified\":%s,\"weights_present\":%s,"
               "\"checkpoint_inventory_ok\":%s,\"checkpoint_bytes\":%llu,"
               "\"planned\":%s,\"state\":",
               model->shape.sizing_verified ? "true" : "false",
               model->weights_present ? "true" : "false",
               model->checkpoint_inventory_ok ? "true" : "false",
               (unsigned long long)model->checkpoint_bytes,
               planned ? "true" : "false");
        json_string(stdout, planned ? lmb_plan_state_name(plan->state) :
                                     "cannot plan");
        printf(",\"missing_bytes\":%llu,\"calibration\":",
               (unsigned long long)(planned ? plan->missing_bytes : 0));
        if (!model->has_calibration) fputs("null", stdout);
        else {
            int current = model->calibration_key_valid &&
                lmb_cal_matches(&model->calibration.key, &model->calibration_key);
            printf("{\"state\":\"%s\",\"measured_at\":%.0f,\"samples\":%u,\"prompt_tokens\":%u,\"generated_tokens\":%u,\"decode_tok_s\":",
                current ? "measured" : "stale", model->calibration.measured_at, model->calibration.samples,
                model->calibration.prompt_tokens, model->calibration.generated_tokens);
            if (current) printf("%.6f", model->calibration.decode_tok_s); else fputs("null", stdout);
            fputc('}', stdout);
        }
        fputs(",\"advice\":", stdout); json_string(stdout, lmb_advice_text(model->advice_flags));
        fputc('}', stdout);
    }
    fputs("]}\n", stdout);
}

#include "lumabri_home_runtime.h"

static int cmd_models(int argc, char **argv) {
    const char *root = NULL, *disk = ".", *keys = NULL, *tracker = NULL;
    uint32_t context = 4096, sessions = 1, max_new = 256;
    /* The screen is the default and the listing is the fallback, not the
     * other way round: a plain list is what you want in a pipe or a log, and
     * a pipe is exactly where a full-screen interface is useless. */
    int plain = !isatty(STDOUT_FILENO), snapshot = 0, json = 0, computers = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--models-dir") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--tracker") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--context") && i + 1 < argc)
            context = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sessions") && i + 1 < argc)
            sessions = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-new") && i + 1 < argc)
            max_new = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--computers")) computers = 1;
        else if (!strcmp(argv[i], "--plain")) plain = 1;
        else if (!strcmp(argv[i], "--json")) { json = 1; plain = 1; }
        else if (!strcmp(argv[i], "--snapshot")) { snapshot = 1; plain = 0; }
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else {
            fprintf(stderr, "usage: lumabri models [--models-dir DIR] "
                            "[--disk PATH] [--context N] [--sessions N] [--tracker H:P]\n"
                            "                      [--plain|--json] [--snapshot] "
                            "[--keys SEQUENCE]\n");
            return 2;
        }
    }
    char def[512];
    if (!root) {
        const char *home = getenv("HOME") ? getenv("HOME") : ".";
        snprintf(def, sizeof def, "%s/.lumabri/models", home);
        root = def;
    }
    static LmbTuiState st;
    memset(&st, 0, sizeof st);
    if (!max_new || max_new > 4096) return 2;
    st.context = context; st.sessions = sessions; st.max_new = max_new;
    st.initial_tab = computers;
    if (checked_printf(st.root, sizeof st.root, "%s", root) ||
        checked_printf(st.disk, sizeof st.disk, "%s", disk) ||
        (tracker && checked_printf(st.tracker, sizeof st.tracker, "%s", tracker))) return 2;
    st.refresh = catalog_state_refresh;
    catalog_state_refresh(&st, NULL);
    if (!plain) {
        int action = lmb_tui_run(&st, snapshot, keys);
        if (action == LMB_TUI_REQUEST_CALIBRATION) st.quick_calibration = 1;
        return action == LMB_TUI_REQUEST_CHAT || action == LMB_TUI_REQUEST_CALIBRATION ?
            home_request_chat(&st, st.action_model) : action;
    }
    if (json) { catalog_json(&st); return st.inventory_ok ? 0 : 1; }

    printf("%s%sLUMABRI · %s planning preview%s\n\n", C_BOLD, C_CORAL,
           tracker ? "LAN" : "local", C_R);
    printf("  %s%u computer%s · CPU only execution plan%s\n", C_DIM,
           st.nnodes, st.nnodes == 1 ? "" : "s", C_R);
    if (!st.inventory_ok)
        printf("  tracker unavailable or incompatible; remote inventory cleared, plans unavailable\n");
    for (uint32_t i = 0; i < st.nnodes; i++) {
        const LmbMachineProfile *p = &st.profiles[i];
        printf("  %s · %s · %u cores/%u threads · %.1f GB offered RAM · "
               "%u GPU detected / %.1f GB VRAM\n", st.nodes[i].name, p->cpu_model,
               p->physical_cores, p->logical_cpus, st.nodes[i].ram_budget_bytes / 1e9,
               p->gpu_count, p->vram_available_bytes / 1e9);
    }
    printf("\n");

    if (!st.nmodels) {
        printf("  %sno checkpoints under %s%s\n", C_DIM, root, C_R);
        printf("  %sput a model directory there, or pass --models-dir%s\n",
               C_DIM, C_R);
        return st.inventory_ok ? 0 : 1;
    }
    printf("  %-2s %-22s %-14s %-28s %s\n", "", "MODEL", "STATE", "", "SPEED");
    for (int i = 0; i < st.nmodels; i++) catalog_row(&st.models[i]);
    printf("\n  %sa speed appears only after a calibration on this cluster "
           "with this plan%s\n", C_DIM, C_R);
    return st.inventory_ok ? 0 : 1;
}

/* A worker publishes inventory even before a model is selected. This command
 * does not reserve RAM, launch engines or download checkpoint blocks. */
static int cmd_worker(int argc, char **argv) {
    const char *tracker = NULL, *name = NULL, *disk = ".", *control = "";
    const char *runtime_epoch = NULL;
    uint64_t limit = UINT64_MAX;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--join") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--control-address") && i + 1 < argc) control = argv[++i];
        else if (!strcmp(argv[i], "--runtime-epoch") && i + 1 < argc) runtime_epoch = argv[++i];
        else if (!strcmp(argv[i], "--ram-gb") && i + 1 < argc) {
            char *end;
            double gb = strtod(argv[++i], &end);
            if (!argv[i][0] || *end || !isfinite(gb) || gb <= 0 || gb > 1048576)
                return 2;
            limit = (uint64_t)(gb * 1e9);
        } else {
            fprintf(stderr, "usage: lumabri worker --join HOST:PORT "
                            "[--name NAME] [--ram-gb N] [--disk PATH]\n");
            return 2;
        }
    }
    if (!tracker || !*tracker || strlen(control) >= 64 || lmb_inventory_text(control) ||
        (name && (!*name || strlen(name) >= 64 ||
                                          lmb_inventory_text(name)))) return 2;
    char kp[1024];
    uint8_t sk[64], pk[32];
    if (lmb_peer_identity(lmb_peer_key_path(kp, sizeof kp), sk, pk)) return 1;
    g_stopping = 0;
    install_chat_signal_handlers();
    signal(SIGPIPE, SIG_IGN);
    LmbMachineProfile profile;
    if (lmb_machine_probe(&profile, disk, NULL)) return 1;
    LmbRuntimeIdentityCache runtime_cache = {0};
    char runtime_dir[1024], runtime_binary[1200], runtime_id[65] = "";
    unsigned runtime_threads = 0;
    if (runtime_epoch) {
        exe_dir(runtime_dir, sizeof runtime_dir);
        if (!control[0] || checked_printf(runtime_binary, sizeof runtime_binary, "%s/segment_node", runtime_dir) ||
            lmb_runtime_thread_capacity(runtime_binary, &runtime_threads) ||
            lmb_runtime_identity(runtime_dir, runtime_epoch, &runtime_cache, runtime_id)) return 1;
    }
    printf("worker: reporting this computer to %s; waiting for a model plan\n", tracker);
    fflush(stdout);
    while (!g_stopping) {
        int fd = lmb_connect_ms_io(tracker, 1500, 2000);
        LmbMsg reply = {0};
        uint8_t nonce[32];
        int ready = fd >= 0 && !lmb_auth(fd) &&
                    !lmb_send(fd, LMB_CHALLENGE, NULL, 0, NULL, 0) &&
                    !lmb_recv(fd, &reply) && reply.op == LMB_CHALLENGE_R &&
                    reply.body_len == sizeof nonce && !reply.pay_len;
        if (ready) memcpy(nonce, reply.body, sizeof nonce);
        lmb_msg_free(&reply);
        while (ready && !g_stopping) {
            LmbMachineReport report = {0};
            memcpy(report.identity, pk, sizeof pk);
            snprintf(report.control_addr, sizeof report.control_addr, "%s", control);
            if (runtime_epoch && !lmb_runtime_identity(runtime_dir, runtime_epoch, &runtime_cache, runtime_id)) {
                snprintf(report.runtime_id, sizeof report.runtime_id, "%s", runtime_id);
                report.runtime_threads = runtime_threads;
            }
            lmb_machine_refresh_resources(&profile, disk);
            report.machine = profile;
            if (name) snprintf(report.machine.hostname, sizeof report.machine.hostname, "%s", name);
            uint64_t reserve = lmb_machine_ram_reserve();
            uint64_t available = report.machine.ram_available_bytes;
            report.ram_budget_bytes = available > reserve ? available - reserve : 0;
            if (report.ram_budget_bytes > limit) report.ram_budget_bytes = limit;
            if (lmb_governor_manual_paused()) report.ram_budget_bytes = 0;
            LmbBuf body = {0}, signed_data = {0};
            uint8_t sig[64];
            int bad = lmb_inventory_pack(&body, &report) ||
                      lmb_buf_str(&signed_data, "lumabri.machine.v1") ||
                      lmb_buf_bytes(&signed_data, nonce, sizeof nonce) ||
                      lmb_buf_bytes(&signed_data, body.p, body.len);
            if (!bad) {
                lmb_sign(sig, signed_data.p, signed_data.len, sk);
                bad = lmb_buf_bytes(&body, sig, sizeof sig) ||
                      lmb_send(fd, LMB_MACHINE_REPORT, body.p, (uint32_t)body.len, NULL, 0) ||
                      lmb_recv(fd, &reply) || reply.op != LMB_OK;
            }
            free(signed_data.p); free(body.p); lmb_msg_free(&reply);
            if (bad) break;
            fprintf(stderr, "[worker] %s: %u cores, %.1f GB offered RAM, %u GPU detected\n",
                    report.machine.hostname, report.machine.physical_cores,
                    report.ram_budget_bytes / 1e9, report.machine.gpu_count);
            for (unsigned i = 0; i < LMB_INVENTORY_HEARTBEAT_MS / 100 && !g_stopping; i++)
                (void)poll(NULL, 0, 100);
        }
        if (fd >= 0) lmb_close(fd);
        if (!g_stopping) {
            fprintf(stderr, "[worker] tracker unavailable or report refused; reconnecting\n");
            for (int i = 0; i < 20 && !g_stopping; i++) (void)poll(NULL, 0, 100);
        }
    }
    memset(sk, 0, sizeof sk);
    return 0;
}

static int cmd_machine(int argc, char **argv) {
    int json = 0;
    const char *tracker = NULL, *disk = ".";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--tracker") && i + 1 < argc)
            tracker = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc)
            disk = argv[++i];
        else {
            fprintf(stderr, "usage: lumabri machine [--json] [--tracker HOST:PORT] "
                            "[--disk PATH]\n");
            return 2;
        }
    }
    LmbMachineProfile profile;
    if (lmb_machine_probe(&profile, disk, tracker)) {
        fprintf(stderr, "cannot profile this machine\n"); return 1;
    }
    lmb_machine_print(stdout, &profile, json);
    if (!json) {
        uint64_t reserve = lmb_machine_ram_reserve();
        LmbGovernor governor;
        lmb_governor_init(&governor, reserve);
        printf("governor %s · %.1f GB system reserve\n",
               lmb_governor_state_name(lmb_governor_poll(&governor)),
               reserve / 1e9);
    }
    return 0;
}

static int cmd_limits(int argc, char **argv) {
    (void)argv;
    if (argc) { fprintf(stderr, "usage: lumabri limits\n"); return 2; }
    uint64_t reserve = lmb_machine_ram_reserve();
    LmbMachineProfile profile;
    (void)lmb_machine_probe(&profile, ".", NULL);
    uint64_t donate = profile.ram_available_bytes > reserve ?
                      profile.ram_available_bytes - reserve : 0;
    printf("system reserve     %.1f GB RAM\n", reserve / 1e9);
    printf("currently donable  %.1f GB RAM\n", donate / 1e9);
    printf("donor CPU team     %u physical cores (low priority)\n",
           profile.physical_cores);
    printf("manual state       %s\n",
           lmb_governor_manual_paused() ? "PAUSED" : "ACTIVE");
    printf("overrides          LUMABRI_RAM_RESERVE_MB, "
           "LUMABRI_EXPERT_RAM_RESERVE_MB, LUMABRI_SEGMENT_RAM_RESERVE_MB\n");
    return 0;
}

static int cmd_governor_manual(int argc, char **argv, int paused) {
    (void)argv;
    if (argc) {
        fprintf(stderr, "usage: lumabri %s\n", paused ? "pause" : "resume");
        return 2;
    }
    if (lmb_governor_set_manual(paused)) {
        perror("cannot update governor state"); return 1;
    }
    printf("donation %s; running work will drain before the new state applies\n",
           paused ? "paused" : "resumed");
    return 0;
}

typedef struct {
    char name[64];
    int ok;
    int required;
    char detail[192];
} DoctorCheck;

static void doctor_json_string(const char *value) {
    fputc('"', stdout);
    for (; value && *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else fputc(c, stdout);
    }
    fputc('"', stdout);
}

static void doctor_add(DoctorCheck *checks, size_t *count, const char *name,
                       int ok, int required, const char *detail) {
    if (*count >= 64) return;
    DoctorCheck *check = &checks[(*count)++];
    snprintf(check->name, sizeof check->name, "%s", name);
    check->ok = ok; check->required = required;
    snprintf(check->detail, sizeof check->detail, "%s", detail ? detail : "");
}

static int cmd_doctor(int argc, char **argv) {
    int json = 0, serve_port = 0;
    const char *tracker = NULL, *model = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--tracker") && i + 1 < argc)
            tracker = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model = argv[++i];
        else if (!strcmp(argv[i], "--serve-port") && i + 1 < argc) {
            if (parse_serve_port(argv[++i], &serve_port)) {
                fprintf(stderr, "--serve-port must be an integer from 1 to 65525\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: lumabri doctor [--json] [--tracker HOST:PORT] "
                            "[--model DIR] [--serve-port N]\n");
            return 2;
        }
    }

    DoctorCheck checks[64]; size_t count = 0;
    LmbMachineProfile profile;
    int machine_ok = lmb_machine_probe(&profile, model ? model : ".", tracker) == 0;
    doctor_add(checks, &count, "machine-profile", machine_ok, 1,
               machine_ok ? "CPU, RAM, GPU, disk and network probed" :
                            "machine probe failed");
    if (machine_ok) {
        uint64_t reserve = lmb_machine_ram_reserve();
        doctor_add(checks, &count, "ram-reserve",
                   profile.ram_available_bytes > reserve, 0,
                   profile.ram_available_bytes > reserve ?
                   "donation has RAM above the system reserve" :
                   "chat works, but donation should remain paused at current RAM");
        doctor_add(checks, &count, "swap-pressure",
                   !profile.swap_total_bytes ||
                   profile.swap_free_bytes >= profile.swap_total_bytes / 20u,
                   0, "at least 5% swap remains free");
        doctor_add(checks, &count, "disk-space",
                   profile.disk_available_bytes >= (1ull << 30), 0,
                   "at least 1 GiB is available at the selected path");
    }

    const char *home = getenv("HOME");
    struct stat state_stat;
    char state_dir[1200] = "";
    int state_ok = home && home[0] && access(home, W_OK) == 0;
    if (home && home[0]) {
        snprintf(state_dir, sizeof state_dir, "%s/.lumabri", home);
        if (!stat(state_dir, &state_stat))
            state_ok = S_ISDIR(state_stat.st_mode) && access(state_dir, W_OK) == 0;
        else if (errno != ENOENT) state_ok = 0;
    }
    doctor_add(checks, &count, "state-directory", state_ok, 1,
               state_ok ? "HOME/.lumabri is writable or can be created" :
                          "HOME/.lumabri is not writable");

    char directory[1024];
    exe_dir(directory, sizeof directory);
    const char *required_bins[] = {"tracker", "maintainer", "swarm_probe"};
    for (size_t i = 0; i < sizeof required_bins / sizeof required_bins[0]; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", directory, required_bins[i]);
        char name[64]; snprintf(name, sizeof name, "binary-%s", required_bins[i]);
        doctor_add(checks, &count, name, access(path, X_OK) == 0, 1,
                   access(path, X_OK) == 0 ? "executable found" :
                                            "run make all or reinstall Lumabri");
    }
    char shim[1200];
    snprintf(shim, sizeof shim, "%s/" LMB_SHIM_NAME, directory);
    if (access(shim, R_OK))
        snprintf(shim, sizeof shim, "%s/../lib/lumabri/" LMB_SHIM_NAME, directory);
    doctor_add(checks, &count, "library-liblumabri", access(shim, R_OK) == 0, 1,
               access(shim, R_OK) == 0 ? "CAS interposer found" :
                                        LMB_SHIM_NAME " is missing");
    const char *segment_bins[] = {"segment_node", "segment_chat"};
    for (size_t i = 0; i < sizeof segment_bins / sizeof segment_bins[0]; i++) {
        char path[1200], name[64];
        snprintf(path, sizeof path, "%s/%s", directory, segment_bins[i]);
        snprintf(name, sizeof name, "optional-%s", segment_bins[i]);
        doctor_add(checks, &count, name, access(path, X_OK) == 0, 0,
                   access(path, X_OK) == 0 ? "Segment runtime available" :
                   "classic Expert/CAS remains available; build against Colibri dev for Segment");
    }

    if (model) {
        char config[1200];
        snprintf(config, sizeof config, "%s/config.json", model);
        doctor_add(checks, &count, "model-config", access(config, R_OK) == 0, 1,
                   access(config, R_OK) == 0 ? "config.json is readable" :
                                              "model directory has no readable config.json");
    }
    if (tracker) {
        doctor_add(checks, &count, "tracker", machine_ok &&
                   profile.tracker_rtt_ms >= 0.0, 1,
                   machine_ok && profile.tracker_rtt_ms >= 0.0 ?
                   "tracker TCP endpoint is reachable" : "tracker is unreachable");
        /* Which side of the NAT am I on? Bind an ephemeral port, ask the
         * tracker to dial it back. Purely informational: a relay-only donor
         * is a full member of the swarm, it just rides the tunnel. */
        static char nat_note[160];
        snprintf(nat_note, sizeof nat_note,
                 "tracker too old for the reach probe — assume relay");
        int lfd = lmb_listen(0);
        if (lfd >= 0) {
            struct sockaddr_in me; socklen_t ml = sizeof me;
            if (getsockname(lfd, (struct sockaddr *)&me, &ml) == 0) {
                LmbBuf b = {0};
                lmb_buf_u32(&b, (uint32_t)ntohs(me.sin_port));
                LmbMsg reply = {0};
                if (!lmb_request(tracker, LMB_REACH, b.p, (uint32_t)b.len,
                                 &reply) && reply.op == LMB_REACH_R) {
                    LmbCur c = { reply.body, reply.body_len, 0 };
                    uint32_t direct = 0; char observed[96] = "";
                    if (!lmb_cur_u32(&c, &direct) &&
                        !lmb_cur_str(&c, observed, sizeof observed))
                        snprintf(nat_note, sizeof nat_note, direct ?
                                 "directly dialable at %s" :
                                 "behind NAT/firewall (%s): the tracker "
                                 "relay will carry this peer", observed);
                }
                lmb_msg_free(&reply);
                free(b.p);
            }
            close(lfd);
        }
        doctor_add(checks, &count, "nat-reachability", 1, 0, nat_note);
    }
    if (serve_port) {
        int topology_ok = 1, failed_port = 0;
        for (int offset = 0; offset < 10; offset++)
            if (!serve_port_available(serve_port + offset)) {
                topology_ok = 0; failed_port = serve_port + offset; break;
            }
        char detail[96];
        if (topology_ok) snprintf(detail, sizeof detail,
                                  "ports %d-%d are available", serve_port,
                                  serve_port + 9);
        else snprintf(detail, sizeof detail, "port %d is already in use",
                      failed_port);
        doctor_add(checks, &count, "serve-ports", topology_ok, 1, detail);
    }

    int ok = 1, warnings = 0;
    for (size_t i = 0; i < count; i++) {
        if (!checks[i].ok && checks[i].required) ok = 0;
        if (!checks[i].ok && !checks[i].required) warnings++;
    }
    if (json) {
        printf("{\"schema\":1,\"ok\":%s,\"warnings\":%d,\"checks\":[",
               ok ? "true" : "false", warnings);
        for (size_t i = 0; i < count; i++) {
            if (i) fputc(',', stdout);
            fputs("{\"name\":", stdout); doctor_json_string(checks[i].name);
            printf(",\"ok\":%s,\"required\":%s,\"detail\":",
                   checks[i].ok ? "true" : "false",
                   checks[i].required ? "true" : "false");
            doctor_json_string(checks[i].detail); fputc('}', stdout);
        }
        fputs("]}\n", stdout);
    } else {
        printf("Lumabri doctor: %s (%d warning%s)\n", ok ? "READY" : "NOT READY",
               warnings, warnings == 1 ? "" : "s");
        for (size_t i = 0; i < count; i++)
            printf("  %s %-24s %s\n", checks[i].ok ? "ok" :
                   (checks[i].required ? "FAIL" : "warn"), checks[i].name,
                   checks[i].detail);
    }
    return ok ? 0 : 1;
}

/* ---- main --------------------------------------------------------------- */
#include "src/ui/lumabri_home_ui.h"

int main(int argc, char **argv) {
    (void)setlocale(LC_CTYPE, "");
    g_tty = isatty(1);
    signal(SIGPIPE, SIG_IGN); /* a closed child pipe is an error, not an exit */
    if (argc >= 2 && !strcmp(argv[1], "key")) return cmd_key(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "peer-key"))
        return cmd_peer_key(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "limits"))
        return cmd_limits(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "pause"))
        return cmd_governor_manual(argc - 2, argv + 2, 1);
    if (argc >= 2 && !strcmp(argv[1], "resume"))
        return cmd_governor_manual(argc - 2, argv + 2, 0);
    if (lmb_secure_init()) return 1; /* children inherit the same strict mode */
    if (argc >= 2 && !strcmp(argv[1], "models"))
        return cmd_models(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "worker"))
        return cmd_worker(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "donor"))
        return cmd_donor(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "host"))
        return cmd_host(argc - 2, argv + 2);
    if (argc >= 2 && (!strcmp(argv[1], "machine") || !strcmp(argv[1], "status")))
        return cmd_machine(argc - 2, argv + 2);
    const char *tok = getenv("LUMABRI_TOKEN");
    if (tok && strlen(tok) > LMB_TOKEN_MAX) {
        fprintf(stderr, "LUMABRI_TOKEN must be at most %u bytes\n",
                (unsigned)LMB_TOKEN_MAX);
        return 2;
    }
    if (argc >= 2 && !strcmp(argv[1], "doctor"))
        return cmd_doctor(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    if (argc == 2 && !strcmp(argv[1], "chat") && g_tty && isatty(0)) return cmd_home();
    if (argc >= 2 && !strcmp(argv[1], "chat")) return cmd_chat(argc - 2, argv + 2);
    /* Interactive entry points share one workspace. Explicit flags remain
     * available to scripts without inheriting an implicit sharing role. */
    if (argc == 1 && g_tty) return cmd_home();
    fprintf(stderr,
        "lumabri: your computers, one shared model\n\n"
        "  lumabri                                                    household workspace\n"
        "  lumabri machine [--json] [--tracker HOST:PORT]             profile this machine\n"
        "  lumabri worker --join HOST:PORT [--ram-gb N]              publish LAN inventory\n"
        "  lumabri models --tracker HOST:PORT                       cluster planning preview\n"
        "  lumabri status | limits | pause | resume                    resource governor\n"
        "  lumabri doctor [--json] [--tracker H:P] [--model DIR]      deployment preflight\n"
        "  lumabri peer-key                                           print this machine's endpoint identity\n"
        "  lumabri serve --model DIR [--port 7300] [--join TRACKER]   share a model\n"
        "  lumabri chat  [--tracker HOST:7300] [--model NAME]         chat with it\n"
        "  lumabri key   [--out NAME]                                 operator keypair\n");
    return 2;
}
