/* Included by lumabri.c after its process, chat and catalogue helpers.
 * Only this translation unit owns the secure transport sessions. */
#ifndef LUMABRI_HOME_RUNTIME_H
#define LUMABRI_HOME_RUNTIME_H
#include "lumabri_home_net.h"
#include "lumabri_runtime_probe.h"
#include "src/runtime/lumabri_prepare_progress.h"

static char home_error[512];
static int home_resident_required(void) {
    const char *value = getenv("LUMABRI_RESIDENT_REQUIRED");
    /* Household is resident by default. Explicit zero remains available to
     * the legacy cache regression harness, not as an automatic fallback. */
    if (!value) { setenv("LUMABRI_RESIDENT_REQUIRED", "1", 0); return 1; }
    return value && !strcmp(value, "1");
}
static int home_fail(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    vsnprintf(home_error, sizeof home_error, fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", home_error);
    return 1;
}
#include "src/runtime/lumabri_resident_plan.h"

typedef struct {
    struct termios saved;
    int active;
} HomeTerminal;

static void home_terminal_begin(HomeTerminal *term) {
    (void)setlocale(LC_CTYPE, "");
    memset(term, 0, sizeof *term);
    if (!g_tty || !isatty(0) || tcgetattr(0, &term->saved)) return;
    struct termios t = term->saved;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t)) return;
    term->active = 1;
    fputs("\033[?1049h\033[?25l", stdout);
}

static void home_terminal_end(HomeTerminal *term) {
    if (!term->active) return;
    tcsetattr(0, TCSANOW, &term->saved);
    fputs("\033[?25h\033[?1049l", stdout); fflush(stdout);
    term->active = 0;
}

static int home_key(void) {
    static int pending = -1;
    if (pending >= 0) { int next = pending; pending = -1; return next; }
    struct pollfd p = {0, POLLIN, 0};
    char key;
    if (poll(&p, 1, 0) <= 0) return -1;
    if (read(0, &key, 1) != 1) return (p.revents & POLLHUP) ? 3 : -1;
    if ((unsigned char)key != 27) return (unsigned char)key;
    char seq[2];
    if (poll(&p, 1, 30) <= 0 || read(0, seq, 1) != 1) return 27;
    /* Two quick Esc presses are two actions, not an unknown CSI sequence.
     * Preserve a following ordinary byte instead of swallowing both. */
    if (seq[0] != '[') { pending = (unsigned char)seq[0]; return 27; }
    if (poll(&p, 1, 30) <= 0 || read(0, seq + 1, 1) != 1) return 0;
    return seq[1] == 'A' ? 1001 : seq[1] == 'B' ? 1002 : 0;
}

static int home_private_network(void) {
    if (!getenv("LUMABRI_ENCRYPT")) setenv("LUMABRI_ENCRYPT", "1", 0);
    if (lmb_secure_init() || !lmb_secure_enabled()) {
        fprintf(stderr, "Household requests require authenticated encryption.\n");
        return -1;
    }
    return 0;
}

static int home_local_ip(const char *tracker, char out[INET_ADDRSTRLEN]) {
    int fd = lmb_connect_ms_io(tracker, 1500, 1500);
    if (fd < 0) return -1;
    struct sockaddr_in local;
    socklen_t len = sizeof local;
    int ok = !getsockname(fd, (struct sockaddr *)&local, &len) &&
             local.sin_family == AF_INET &&
             inet_ntop(AF_INET, &local.sin_addr, out, INET_ADDRSTRLEN);
    lmb_close(fd);
    return ok ? 0 : -1;
}

static int home_listen(int *port) {
    return lmb_home_listen_service(port);
}

static int home_control_io_ms(int fallback) {
    /* Respect the existing operator/test timeout, but a partial control
     * frame must not block the donor UI for the general five-minute default. */
    int ms = lmb_env_int("LUMABRI_IO_TIMEOUT_MS", fallback, 100, 600000);
    return ms > 10000 ? 10000 : ms;
}

/* Consumes listener, including on failure; other child descriptors stay CLOEXEC. */
static pid_t home_spawn(char *const argv[], char *const envv[], const char *log, int *ready_fd, int listener) {
    int ready[2] = {-1, -1};
    if (ready_fd && pipe(ready)) { if (listener >= 0) close(listener); return -1; }
    if (ready_fd) (void)fcntl(ready[0], F_SETFD, FD_CLOEXEC);
    pid_t parent = getpid(), pid = fork();
    if (!pid) {
        if (setpgid(0, 0) || child_follow_parent(parent)) _exit(125);
#ifdef __APPLE__
        /* Darwin has no PR_SET_PDEATHSIG. An owning supervisor keeps the
         * dedicated group alive only while both the TUI and service live.
         * No watcher thread is lost at exec, and no orphan retains a RAM
         * lease when the terminal is killed. The engine still owns READY. */
        pid_t service = fork();
        if (service < 0) _exit(125);
        if (service > 0) {
            int limit = getdtablesize();
            for (int fd = 3; fd < limit; fd++) close(fd);
            for (;;) {
                int status;
                pid_t result = waitpid(service, &status, WNOHANG);
                if (getppid() != parent || result == service ||
                    (result < 0 && errno != EINTR)) {
                    (void)kill(-getpgrp(), SIGKILL);
                    _exit(125);
                }
                (void)poll(NULL, 0, 100);
            }
        }
#endif
        int nullfd = open("/dev/null", O_RDONLY);
        int logfd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (nullfd < 0 || logfd < 0) _exit(125);
        dup2(nullfd, 0); dup2(logfd, 1); dup2(logfd, 2);
        if (nullfd > 2) close(nullfd);
        if (logfd > 2) close(logfd);
        for (int i = 0; envv && envv[i]; i++) {
            const size_t preload_prefix = sizeof(LMB_PRELOAD_ENV "=") - 1;
            if (!strncmp(envv[i], LMB_PRELOAD_ENV "=", preload_prefix)) {
                if (lmb_preload_file(envv[i] + preload_prefix)) {
                    perror("Cannot load household weight loader"); _exit(125);
                }
            } else if (putenv(envv[i])) _exit(125);
        }
        if (listener >= 0) {
            char descriptor[32];
            if (fcntl(listener, F_SETFD, 0)) _exit(125);
            snprintf(descriptor, sizeof descriptor, "%d", listener);
            setenv("LUMABRI_HOME_LISTEN_FD", descriptor, 1);
        } else unsetenv("LUMABRI_HOME_LISTEN_FD");
        if (ready_fd) {
            char descriptor[32];
            close(ready[0]);
            snprintf(descriptor, sizeof descriptor, "%d", ready[1]);
            setenv("LUMABRI_READY_FD", descriptor, 1);
        } else unsetenv("LUMABRI_READY_FD");
        execv(argv[0], argv); _exit(127);
    }
    if (listener >= 0) close(listener);
    if (ready_fd) {
        close(ready[1]);
        if (pid < 0) close(ready[0]);
        else *ready_fd = ready[0];
    }
    if (pid > 0) (void)setpgid(pid, pid);
    return pid;
}

static void home_stop_child(pid_t *pid) {
    if (*pid <= 0) return;
    pid_t owned = *pid;
    (void)kill(-owned, SIGTERM);
    for (int i = 0; i < 100; i++) {
        pid_t r = waitpid(owned, NULL, WNOHANG);
        if (r == owned || (r < 0 && errno == ECHILD)) break;
        (void)poll(NULL, 0, 20);
    }
    /* Descendants belong to the same dedicated group, including an Edge
     * engine whose immediate host parent exited first. */
    (void)kill(-owned, SIGKILL);
    while (waitpid(owned, NULL, 0) < 0 && errno == EINTR) { }
    *pid = 0;
}

/* Readiness comes from the owned child after engine_open and listen succeed.
 * A free-port probe cannot prove ownership or readiness. */
static int home_child_ready(int *fd) {
    if (*fd < 0) return 0;
    struct pollfd p = {*fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0) return 0;
    char byte;
    int ok = read(*fd, &byte, 1) == 1 && byte == 'R';
    close(*fd); *fd = -1;
    return ok;
}

static int home_status_send(int fd, const LmbHomeTransaction *t, int segment_port,
                             int host_port) {
    LmbBuf b = {0};
    int bad = lmb_buf_u32(&b, LMB_HOME_VERSION) ||
        lmb_buf_bytes(&b, t->offer.id, 32) || lmb_buf_u32(&b, t->phase) ||
        lmb_buf_u32(&b, (uint32_t)segment_port) || lmb_buf_u32(&b, (uint32_t)host_port) ||
        lmb_buf_str(&b, t->reason);
    int rc = bad ? -1 : lmb_send(fd, LMB_HOME_STATUS, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p); return rc;
}

typedef struct {
    LmbHomeTransaction transaction;
    unsigned thread_capacity;
    int retained; /* owner's sharing lifetime, independent of any chat */
    int client, lease, weight_lease, segment_port, host_port, segment_ready, host_ready;
    pid_t segment, host;
    char bin_dir[1024], cache_base[1024], disk[512], ip[INET_ADDRSTRLEN];
    char log[1200];
    char runtime_epoch[65], runtime_id[65];
    LmbRuntimeIdentityCache runtime_cache;
} HomeDonor;

/* --disk can point two different homes at the same persistent weight cache.
 * Their RAM leases are separate files, so also serialize mutable mirrors by
 * cache directory. Fail immediately instead of blocking a second accepted
 * plan behind the first model's lifetime-long shared mirror lock. */
#include "src/runtime/lumabri_weight_cache.h"
static int home_weight_lease(const char *base) {
    return lmb_weight_cache_lock(base);
}

static void home_donor_release(HomeDonor *d, LmbHomePhase why, const char *reason) {
    d->retained = 0;
    home_stop_child(&d->host); home_stop_child(&d->segment);
    if (d->segment_ready >= 0) { close(d->segment_ready); d->segment_ready = -1; }
    if (d->host_ready >= 0) { close(d->host_ready); d->host_ready = -1; }
    if (d->lease >= 0) { close(d->lease); d->lease = -1; }
    if (d->weight_lease >= 0) { close(d->weight_lease); d->weight_lease = -1; }
    d->host_port = d->segment_port = 0;
    lmb_home_released(&d->transaction, why, reason);
}

static void home_donor_disconnect(HomeDonor *d, const char *reason) {
    home_donor_release(d, LMB_HOME_CLOSED, reason);
    if (d->client >= 0) { lmb_close(d->client); d->client = -1; }
}

static int home_donor_can_retain(const HomeDonor *d) {
    return home_resident_required() && d->segment > 0 && d->transaction.reservation_held &&
        (d->transaction.phase == LMB_HOME_READY ||
         (!d->transaction.offer.runs_edge && d->transaction.phase == LMB_HOME_SEGMENT_READY));
}

/* A disappearing chatter is not permission to evict an owner's loaded model.
 * Explicit CANCEL still rolls back a failed/incomplete plan; owner Stop and
 * donor shutdown still free everything. Only fully prepared ranges survive. */
static void home_donor_lost_requester(HomeDonor *d, const char *reason) {
    if (!home_donor_can_retain(d)) { home_donor_disconnect(d, reason); return; }
    d->retained = 1;
    snprintf(d->transaction.reason, sizeof d->transaction.reason,
             "Requester disconnected. Weights stay in RAM until you stop sharing.");
    if (d->client >= 0) { lmb_close(d->client); d->client = -1; }
}

/* The scope is immutable after acceptance. The virtual path remains private
 * to this request, while verified weights survive it: one working mirror per
 * adapter and one content-addressed store per donor. The protocol model root
 * includes the session's routing name, so it is NOT a stable cache key.
 * A mirror's name grants no trust: the loader resets its maps on every
 * signed-identity change, then restores matching blocks from the shared CAS.
 * Session/KV state never goes into these weight caches. The shim still checks
 * current signed inventory, accepted root and each block hash on reuse. */
static int home_donor_launch(HomeDonor *d, int edge) {
    char current_runtime[65];
    if (lmb_runtime_identity(d->bin_dir, d->runtime_epoch, &d->runtime_cache, current_runtime) ||
        strcmp(current_runtime, d->runtime_id)) {
        fprintf(stderr, "The installed runtime changed. Restart Share resources before accepting a new plan.\n");
        return -1;
    }
    const LmbHomeOffer *o = &d->transaction.offer;
    const LmbModelFamily *family = lmb_family_for(o->model_type);
    if (!family) return -1;
    char root[65], requester[65], edge_peer[65], id[65];
    lmb_hex(root, o->model_root, 32); lmb_hex(requester, o->requester, 32);
    lmb_hex(edge_peer, o->edge_peer, 32); lmb_hex(id, o->id, 32);
    char shim[1200], binary[1200], vroot[1200], cache[1200], cas[1200];
    char e_shim[1232], e_vroot[1232], e_cache[1232], e_cas[1232];
    char e_tracker[300], e_model[100], e_root[100], e_key[100], e_limit[100], e_log[1232];
    char range[40], port[20], addr[64], name[64], context[20], threads[20], ram[32];
    char bytes[32], layers[20], max_new[20], e_omp[64], e_omp_limit[64];
    if (checked_printf(shim, sizeof shim, "%s/" LMB_SHIM_NAME, d->bin_dir) ||
        checked_printf(binary, sizeof binary, "%s/%s", d->bin_dir, edge ? "lumabri" : "segment_node") ||
        checked_printf(vroot, sizeof vroot, "%s/%.16s/vroot", d->cache_base, id) ||
        checked_printf(cache, sizeof cache, "%s/mirrors/%s/cache", d->cache_base, family->segment_id) ||
        checked_printf(cas, sizeof cas, "%s/cas", d->cache_base) ||
        access(binary, X_OK)) return -1;
    if (access(shim, R_OK) &&
        (checked_printf(shim, sizeof shim, "%s/../lib/lumabri/" LMB_SHIM_NAME, d->bin_dir) ||
         access(shim, R_OK))) return -1;
    mkdir_p(cache); mkdir_p(cas);
    snprintf(e_shim, sizeof e_shim, LMB_PRELOAD_ENV "=%s", shim);
    snprintf(e_vroot, sizeof e_vroot, "LUMABRI_VROOT=%s", vroot);
    snprintf(e_cache, sizeof e_cache, "LUMABRI_CACHE=%s", cache);
    snprintf(e_cas, sizeof e_cas, "LUMABRI_CAS=%s", cas);
    snprintf(e_tracker, sizeof e_tracker, "LUMABRI_TRACKER=%s", o->tracker);
    snprintf(e_model, sizeof e_model, "LUMABRI_MODEL=%s", o->model);
    snprintf(e_root, sizeof e_root, "LUMABRI_EXPECT_MODEL_ROOT=%s", root);
    snprintf(e_key, sizeof e_key, "LUMABRI_SEGMENT_CLIENT_PK=%s", edge_peer);
    snprintf(e_limit, sizeof e_limit, "LUMABRI_EDGE_RAM_BYTES=%llu", (unsigned long long)o->edge_ram_bytes);
    snprintf(e_log, sizeof e_log, "LUMABRI_ENGINE_LOG=%s/edge.log", d->cache_base);
    unsigned usable_threads = o->threads < d->thread_capacity ? o->threads : d->thread_capacity;
    if (!usable_threads) return -1;
    snprintf(e_omp, sizeof e_omp, "OMP_NUM_THREADS=%u", usable_threads);
    snprintf(e_omp_limit, sizeof e_omp_limit, "OMP_THREAD_LIMIT=%u", usable_threads);
    char *envv[] = {e_shim, e_vroot, e_cache, e_cas, e_tracker, e_model, e_root,
                   e_key, "LUMABRI_SEGMENT_REQUIRED=1", "LUMABRI_VERIFY=0",
                   "LUMABRI_PREFETCH=0", "LUMABRI_NO_EXEC=1",
                   "LUMABRI_ENGINE_BACKEND=cpu", e_log,
                   edge ? e_limit : "LUMABRI_HOME_SEGMENT=1", e_omp, e_omp_limit, NULL};
    int chosen_port = 0, listener = home_listen(&chosen_port);
    if (listener < 0) return -1;
    snprintf(port, sizeof port, "%d", chosen_port);
    snprintf(addr, sizeof addr, "%s:%d", d->ip, chosen_port);
    snprintf(name, sizeof name, "home-%.12s-%u", id, o->begin);
    snprintf(range, sizeof range, "%u:%u", o->begin, o->end);
    snprintf(context, sizeof context, "%u", o->context);
    snprintf(threads, sizeof threads, "%u", usable_threads);
    snprintf(ram, sizeof ram, "%llu", (unsigned long long)((o->ram_bytes - o->edge_ram_bytes) >> 20));
    snprintf(bytes, sizeof bytes, "%llu", (unsigned long long)o->model_bytes);
    snprintf(layers, sizeof layers, "%u", o->layers);
    snprintf(max_new, sizeof max_new, "%u", o->max_new);
    char *segment_argv[] = {binary, "--engine", (char *)family->segment_id,
        "--model-dir", vroot, "--model", (char *)o->model, "--range", range,
        "--port", port, "--tracker", (char *)o->tracker, "--advertise", addr,
        "--name", name, "--model-root", root, "--tokenizer-root", root,
        "--context", context, "--max-rows", "16", "--sessions", "1",
        "--threads", threads, "--memory-limit-mb", ram,
        "--model-bytes", bytes, "--model-layers", layers, NULL};
    char *host_argv[] = {binary, "host", "--model", (char *)o->model,
        "--tracker", (char *)o->tracker, "--port", port, "--ctx", context,
        "--max-new", max_new, "--client-key", requester, NULL};
    if (edge) {
        /* The host itself must not be preloaded; model_boot passes the mirror
         * to its Edge child, while its own networking remains ordinary C. */
        envv[0] = LMB_PRELOAD_ENV "=";
        d->host = home_spawn(host_argv, envv, d->log, &d->host_ready, listener);
        d->host_port = chosen_port;
        return d->host > 0 ? 0 : -1;
    }
    d->segment = home_spawn(segment_argv, envv, d->log, &d->segment_ready, listener);
    d->segment_port = chosen_port;
    return d->segment > 0 ? 0 : -1;
}

static void home_donor_screen(const HomeDonor *d, const char *name, uint64_t ram, int clear, int choice) {
    const LmbHomeTransaction *t = &d->transaction;
    if (clear) {
        ui_begin("share resources");
        ui_printf(5, 5, UI_TEXT, "%s · up to %.1f GB RAM · CPU execution", name, ram / 1e9);
        ui_text(7, 5, UI_SAND, d->retained ? "Weights retained in RAM; ready for another chat" : lmb_home_phase_name(t->phase));
        if (t->phase != LMB_HOME_IDLE) {
            char who[65]; lmb_hex(who, t->offer.requester, 32);
            ui_printf(10, 5, UI_TEXT, "Model: %s (%s)", t->offer.model, t->offer.model_type);
            ui_printf(12, 5, UI_MUTED, "Requester identity: %.24s…", who);
            ui_printf(14, 5, UI_TEXT, "Layers %u–%u of %u · %.2f GB RAM · %.2f GB %s",
                t->offer.begin, t->offer.end - 1, t->offer.layers, t->offer.ram_bytes / 1e9,
                t->offer.disk_bytes / 1e9, home_resident_required() ? "metadata headroom" : "disk headroom");
            ui_printf(16, 5, UI_MUTED, "%u context · one session · %u threads · up to %u new tokens per turn", t->offer.context,
                      t->offer.threads < d->thread_capacity ? t->offer.threads : d->thread_capacity, t->offer.max_new);
            ui_text(18, 5, UI_MUTED, t->offer.runs_edge ?
                "This computer hosts chat and receives the conversation text." :
                "This computer processes activations and keeps state for its layers.");
        } else {
            ui_text(11, 5, UI_TEXT, "Visible to your household. Waiting for a request.");
            ui_printf(14, 5, UI_MUTED, "Runtime supports up to %u thread(s). Nothing loads without approval.", d->thread_capacity);
        }
        int y = ui_h >= 34 ? 23 : 20;
        if (t->phase == LMB_HOME_PENDING) {
            ui_item(y, choice == 0, "Accept this request", home_resident_required() ?
                "Keep these weights in RAM until you stop sharing." : "Reserve only the displayed resources for this plan.");
            ui_item(y + 3, choice == 1, "Decline", "No weights or model state will be loaded.");
        } else ui_item(y, 0, "Keep this window open to share", d->retained ?
            "Weights stay in RAM. Unload only to accept a different plan." :
            "Press Esc to stop sharing and release resources.");
        ui_footer(t->reason[0] ? t->reason : d->log, t->phase == LMB_HOME_PENDING ?
            "↑ ↓ choose   Enter confirm   Esc stop and return" :
            d->retained ? "x unload model, keep sharing   Esc stop sharing" : "Esc stop sharing and return");
        if (ui_w < 60 || ui_h < 28) {
            ui_begin("share resources"); ui_text(5, 4, UI_SAND, "Resize to at least 60 × 28. Esc stops sharing.");
        }
        ui_present(); return;
    }
    printf("LUMABRI / SHARE RESOURCES\n\n%s · up to %.1f GB RAM · CPU\n\n%s\n",
           name, ram / 1e9, d->retained ? "Weights retained in RAM; ready for another chat" : lmb_home_phase_name(t->phase));
    if (t->phase != LMB_HOME_IDLE) {
        char who[65]; lmb_hex(who, t->offer.requester, 32);
        printf("\nRequester identity: %.24s…\nModel: %s (%s)\nLayers: %u–%u of %u\n"
               "RAM budget: %.2f GB · %s headroom: %.2f GB\n"
               "Context: %u · one session · %u threads · up to %u new tokens per turn\n",
               who, t->offer.model, t->offer.model_type, t->offer.begin,
               t->offer.end - 1, t->offer.layers, t->offer.ram_bytes / 1e9,
               home_resident_required() ? "metadata" : "estimated disk",
               t->offer.disk_bytes / 1e9, t->offer.context,
               t->offer.threads < d->thread_capacity ? t->offer.threads : d->thread_capacity, t->offer.max_new);
        if (t->offer.runs_edge)
            puts("This computer also hosts chat and receives the conversation text.");
        else puts("This computer processes activations and keeps state for its layers.");
        if (t->reason[0]) printf("\n%s\n", t->reason);
    }
    puts(t->phase == LMB_HOME_PENDING ? "\n[y] Accept   [n] Decline   [q] Exit" :
                                      "\n[x] Stop and release   [q] Exit");
    printf("\nEngine log: %s\n", d->log);
    fflush(stdout);
}

static int home_donor_offer(HomeDonor *d, int incoming, const char *tracker,
    uint64_t ram, uint64_t disk) {
    (void)fcntl(incoming, F_SETFD, FD_CLOEXEC);
    lmb_set_io_timeout(incoming, home_control_io_ms(1000));
    if (lmb_secure_server(incoming)) return -1;
    LmbMsg m = {0}; LmbHomeOffer offer;
    int rc = lmb_recv(incoming, &m);
    const char *token = getenv("LUMABRI_TOKEN");
    if (!rc && token && *token) {
        char supplied[LMB_TOKEN_MAX + 1] = "", expected[LMB_TOKEN_MAX + 1] = "";
        LmbCur c = {m.body, m.body_len, 0};
        rc = m.op != LMB_AUTH || m.pay_len || strlen(token) > LMB_TOKEN_MAX ||
             lmb_cur_str(&c, supplied, sizeof supplied) || c.off != c.len;
        if (!rc) {
            snprintf(expected, sizeof expected, "%s", token);
            rc = !lmb_token_equal(supplied, expected);
        }
        lmb_msg_free(&m);
        if (!rc) rc = lmb_send(incoming, LMB_OK, NULL, 0, NULL, 0) || lmb_recv(incoming, &m);
    }
    if (!rc) {
        LmbCur c = {m.body, m.body_len, 0};
        rc = m.op != LMB_HOME_OFFER || m.pay_len ||
             lmb_home_offer_unpack(&c, &offer) ||
             !lmb_secure_peer_matches(incoming, offer.requester);
    }
    lmb_msg_free(&m);
    if (!rc && (d->client >= 0 || d->retained)) {
        /* Reply against the NEW request ID without changing the admitted
         * transaction, its leases, or its controller connection. An EOF is
         * not a useful capacity signal to a second household chatter. */
        LmbHomeTransaction busy = { .offer = offer, .phase = LMB_HOME_REJECTED };
        snprintf(busy.reason, sizeof busy.reason,
                 "%s", d->retained ?
                 "BUSY: this model is still resident. Stop sharing before replacing its allocation." :
                 "BUSY: this computer already has an active household request.");
        (void)home_status_send(incoming, &busy, 0, 0);
        return -1; /* caller closes only this unadmitted connection */
    }
    if (!rc) rc = lmb_home_offer_begin(&d->transaction, &offer, tracker,
                                       ram, disk, (uint64_t)(nowd() * 1000));
    if (rc) return -1;
    d->client = incoming;
    return home_status_send(incoming, &d->transaction, 0, 0);
}

static int home_donor_message(HomeDonor *d) {
    LmbMsg m = {0};
    if (lmb_recv(d->client, &m)) { lmb_msg_free(&m); return -1; }
    LmbHomeTransaction *t = &d->transaction;
    int rc = m.pay_len || m.body_len != 32 || memcmp(m.body, t->offer.id, 32);
    if (!rc) {
        t->last_seen_ms = (uint64_t)(nowd() * 1000);
        switch (m.op) {
        case LMB_HOME_PULSE: break;
        case LMB_HOME_DETACH:
            if (!home_donor_can_retain(d)) {
                rc = -1; break;
            }
            d->retained = 1;
            snprintf(t->reason, sizeof t->reason,
                     "Chat closed. Weights stay in RAM until this computer stops sharing.");
            break;
        case LMB_HOME_CANCEL:
            home_donor_release(d, LMB_HOME_CLOSED, "The requester cancelled the plan."); break;
        case LMB_HOME_COMMIT:
            rc = lmb_home_commit(t, m.body);
            if (!rc && home_donor_launch(d, 0))
                home_donor_release(d, LMB_HOME_FAILED, "Cannot start the accepted Segment engine.");
            break;
        case LMB_HOME_START_HOST:
            rc = lmb_home_start_host(t, m.body);
            if (!rc && home_donor_launch(d, 1))
                home_donor_release(d, LMB_HOME_FAILED, "Cannot start the chat host.");
            break;
        default: rc = -1; break;
        }
    }
    lmb_msg_free(&m);
    if (!rc) rc = home_status_send(d->client, t, d->segment_port, d->host_port);
    if (d->retained) {
        lmb_close(d->client); d->client = -1;
        return 0; /* a lost acknowledgement must not unload prepared RAM */
    }
    return rc;
}

static int cmd_donor(int argc, char **argv) {
    (void)home_resident_required();
    home_error[0] = 0;
    const char *tracker = NULL, *name = NULL, *disk = NULL;
    uint64_t limit = UINT64_MAX;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--join") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--ram-gb") && i + 1 < argc) {
            char *end; double gb = strtod(argv[++i], &end);
            if (*end || !isfinite(gb) || gb <= 0 || gb > 1048576) return 2;
            limit = (uint64_t)(gb * 1e9);
        } else return 2;
    }
    if (!tracker || !*tracker || strlen(tracker) >= 256 || !isatty(0) ||
        (name && (!*name || strlen(name) >= 64 || lmb_inventory_text(name))) || home_private_network())
        return 2;
    HomeDonor d = {0}; d.client = d.lease = d.weight_lease = d.segment_ready = d.host_ready = -1;
    exe_dir(d.bin_dir, sizeof d.bin_dir);
    const char *services[] = {"segment_node", "segment_chat"};
    char service_path[1200];
    for (size_t i = 0; i < sizeof services / sizeof *services; i++) {
        if (checked_printf(service_path, sizeof service_path, "%s/%s", d.bin_dir, services[i]) ||
            access(service_path, X_OK))
            return home_fail("Household runtime is not installed: %s is missing. Install or build the household services, not only the TUI.", services[i]);
    }
    if (checked_printf(service_path, sizeof service_path, "%s/" LMB_SHIM_NAME, d.bin_dir) ||
        (access(service_path, R_OK) &&
         (checked_printf(service_path, sizeof service_path, "%s/../lib/lumabri/" LMB_SHIM_NAME, d.bin_dir) ||
          access(service_path, R_OK))))
        return home_fail("The household weight loader (%s) is missing. Install or build the complete household runtime.", LMB_SHIM_NAME);
    if (checked_printf(service_path, sizeof service_path, "%s/segment_node", d.bin_dir) ||
        lmb_runtime_thread_capacity(service_path, &d.thread_capacity))
        return home_fail("Cannot query the installed Segment runtime. Rebuild the complete household runtime; no resources were shared.");
    uint8_t epoch[32]; lmb_random(epoch, sizeof epoch); lmb_hex(d.runtime_epoch, epoch, sizeof epoch);
    if (lmb_runtime_identity(d.bin_dir, d.runtime_epoch, &d.runtime_cache, d.runtime_id))
        return home_fail("Cannot identify the installed household runtime. No resources were shared.");
    if (checked_printf(d.cache_base, sizeof d.cache_base, "%s/%s", disk ? disk :
                       (getenv("HOME") ? getenv("HOME") : "."), disk ? "lumabri-home" : ".lumabri/home") ||
        home_local_ip(tracker, d.ip))
        return home_fail("Cannot reach the household. Check its address and LAN access; no resources were shared.");
    int auth = lmb_connect_ms_io(tracker, 1200, 1500);
    if (auth < 0) return home_fail("Cannot connect to the household tracker. No resources were shared.");
    int rejected = lmb_auth(auth);
    lmb_close(auth);
    if (rejected) return home_fail("Household authentication failed. Use /join to check the household key. No resources were shared.");
    mkdir_p(d.cache_base);
    if (checked_printf(d.log, sizeof d.log, "%s/engines.log", d.cache_base)) return 1;
    LmbMachineProfile profile;
    if (lmb_machine_probe(&profile, d.cache_base, NULL) || !profile.ram_total_bytes)
        return home_fail("Cannot read this computer's memory. Sharing is disabled until hardware detection succeeds.");
    uint64_t reserve = lmb_machine_ram_reserve();
    uint64_t ram = profile.ram_available_bytes > reserve ? profile.ram_available_bytes - reserve : 0;
    if (limit < ram) ram = limit;
    if (ram < (32u << 20)) return home_fail("Not enough available RAM to share safely: %.2f GB available, %.2f GB system reserve. Close other applications and retry.", profile.ram_available_bytes / 1e9, reserve / 1e9);
    if (!name) name = profile.hostname;
    int port, listener = home_listen(&port);
    if (listener < 0) return home_fail("Cannot open a donor port in the household range: %s. Close another sharing window and retry.", strerror(errno));
    char own_bin[1200], addr[64], budget[32], report_log[1200];
    snprintf(own_bin, sizeof own_bin, "%s/lumabri", d.bin_dir);
    snprintf(addr, sizeof addr, "%s:%d", d.ip, port);
    snprintf(budget, sizeof budget, "%.9f", ram / 1e9);
    snprintf(report_log, sizeof report_log, "%s/inventory.log", d.cache_base);
    char *worker_argv[] = {own_bin, "worker", "--join", (char *)tracker,
        "--name", (char *)name, "--ram-gb", budget, "--disk", d.cache_base,
        "--control-address", addr, "--runtime-epoch", d.runtime_epoch, NULL};
    pid_t reporter = home_spawn(worker_argv, NULL, report_log, NULL, -1);
    if (reporter <= 0) { close(listener); return home_fail("Cannot start the inventory reporter: %s.", strerror(errno)); }
    g_stopping = 0; install_chat_signal_handlers(); signal(SIGPIPE, SIG_IGN);
    HomeTerminal term; home_terminal_begin(&term);
    double redraw = 0;
    int donor_choice = 1; /* Enter alone must never accept a new allocation. */
    while (!g_stopping) {
        if (waitpid(reporter, NULL, WNOHANG) == reporter) {
            (void)home_fail("The inventory reporter stopped. See %s. No new requests can be accepted.", report_log);
            break;
        }
        double now = nowd();
        if (now - redraw >= .25) { home_donor_screen(&d, name, ram, term.active, donor_choice); redraw = now; }
        struct pollfd ready[2] = {{listener, POLLIN, 0}, {d.client, POLLIN, 0}};
        (void)poll(ready, 2, 50);
        if (ready[0].revents & POLLIN) {
            int incoming = accept(listener, NULL, NULL);
            if (incoming >= 0) {
                /* An unrelated preflight/offer must not reset the choice
                 * the owner is making on an existing pending request. */
                if (d.client < 0) donor_choice = 1;
                lmb_machine_refresh_resources(&profile, d.cache_base);
                uint64_t free_ram = profile.ram_available_bytes > reserve ? profile.ram_available_bytes - reserve : 0;
                if (free_ram > ram) free_ram = ram;
                if (home_donor_offer(&d, incoming, tracker, free_ram, profile.disk_available_bytes)) {
                    if (d.client == incoming) home_donor_disconnect(&d, "Request connection failed.");
                    else lmb_close(incoming);
                }
            }
        }
        if (d.client >= 0 && ready[1].fd == d.client && ready[1].revents && home_donor_message(&d))
            home_donor_lost_requester(&d, "The requester disconnected or sent an invalid command.");
        int key = home_key();
        if (key == 'q' || key == 3 || key == 27) break;
        if (key == 1001 || key == 1002) { donor_choice = !donor_choice; redraw = 0; }
        if (key == '\r' || key == '\n') {
            if (term.active && (ui_w < 60 || ui_h < 28)) continue;
            if (d.transaction.phase != LMB_HOME_PENDING) continue;
            key = donor_choice ? 'n' : 'y';
        }
        if (key == 'x') home_donor_disconnect(&d, "Stopped by this computer's owner.");
        if ((key == 'y' || key == 'n') && d.transaction.phase == LMB_HOME_PENDING) {
            lmb_machine_refresh_resources(&profile, d.cache_base);
            uint64_t free_ram = profile.ram_available_bytes > reserve ? profile.ram_available_bytes - reserve : 0;
            if (key == 'y') {
                char owner[256];
                if (!lmb_governor_manual_paused() && d.transaction.offer.threads <= profile.logical_cpus)
                    d.lease = lmb_machine_compute_lease_acquire(d.transaction.offer.model, tracker, owner, sizeof owner);
                if (d.lease >= 0) d.weight_lease = home_weight_lease(d.cache_base);
            }
            if ((key == 'y' && (d.lease < 0 || d.weight_lease < 0)) ||
                lmb_home_decide(&d.transaction, key == 'y', free_ram,
                                profile.disk_available_bytes, (uint64_t)(nowd() * 1000)))
                home_donor_release(&d, LMB_HOME_FAILED, "Resources are unavailable or another plan owns this computer or weight cache.");
            if (d.client >= 0 && home_status_send(d.client, &d.transaction, 0, 0))
                home_donor_disconnect(&d, "The requester disconnected.");
        }
        if (!d.retained && lmb_home_expired(&d.transaction, (uint64_t)(nowd() * 1000)))
            home_donor_lost_requester(&d, "Request lease expired before preparation completed.");
        if (d.segment > 0 && waitpid(d.segment, NULL, WNOHANG) == d.segment)
            home_donor_release(&d, LMB_HOME_FAILED, "Segment stopped. See the engine log.");
        if (d.host > 0 && waitpid(d.host, NULL, WNOHANG) == d.host)
            home_donor_release(&d, LMB_HOME_FAILED, "Chat host stopped. See the engine log.");
        if (d.transaction.phase == LMB_HOME_LOADING && home_child_ready(&d.segment_ready))
            (void)lmb_home_mark_segment_ready(&d.transaction);
        if (d.transaction.phase == LMB_HOME_STARTING_HOST && home_child_ready(&d.host_ready))
            d.transaction.phase = LMB_HOME_READY;
    }
    home_terminal_end(&term);
    home_donor_disconnect(&d, "Donor closed.");
    home_stop_child(&reporter); close(listener);
    return home_error[0] ? 1 : 0;
}
typedef struct {
    int fd[LMB_CLUSTER_MAX_NODES];
    LmbHomeOffer offers[LMB_CLUSTER_MAX_NODES];
    LmbHomePhase phase[LMB_CLUSTER_MAX_NODES];
    uint32_t host_port[LMB_CLUSTER_MAX_NODES];
    char names[LMB_CLUSTER_MAX_NODES][64], addresses[LMB_CLUSTER_MAX_NODES][64];
    char reason[LMB_CLUSTER_MAX_NODES][160];
    uint32_t count, edge;
    pid_t source;
    _Atomic int stop, failed;
} HomeSession;

static int home_session_receive(HomeSession *s, uint32_t i) {
    LmbMsg m = {0};
    if (lmb_recv(s->fd[i], &m)) { lmb_msg_free(&m); return -1; }
    LmbCur c = {m.body, m.body_len, 0};
    uint32_t version = 0, phase = 0, segment_port = 0, host_port = 0;
    int bad = m.op != LMB_HOME_STATUS || m.pay_len ||
        lmb_cur_u32(&c, &version) || version != LMB_HOME_VERSION || c.len - c.off < 32;
    if (!bad) { bad = memcmp(c.p + c.off, s->offers[i].id, 32); c.off += 32; }
    if (!bad) bad = lmb_cur_u32(&c, &phase) || phase > LMB_HOME_CLOSED ||
        lmb_cur_u32(&c, &segment_port) || segment_port > 65535 ||
        lmb_cur_u32(&c, &host_port) || host_port > 65535 ||
        lmb_inventory_string(&c, s->reason[i], sizeof s->reason[i]) || c.off != c.len;
    lmb_msg_free(&m);
    if (bad) return -1;
    s->phase[i] = (LmbHomePhase)phase; s->host_port[i] = host_port;
    return phase >= LMB_HOME_REJECTED ? -1 : 0;
}

static int home_session_poll(HomeSession *s, int pulse) {
    struct pollfd fds[LMB_CLUSTER_MAX_NODES];
    for (uint32_t i = 0; i < s->count; i++) {
        fds[i] = (struct pollfd){s->fd[i], POLLIN, 0};
        if (pulse && lmb_send(s->fd[i], LMB_HOME_PULSE, s->offers[i].id, 32, NULL, 0)) return -1;
    }
    int n = poll(fds, s->count, 50);
    if (n < 0 && errno != EINTR) return -1;
    for (uint32_t i = 0; i < s->count; i++)
        if (fds[i].revents && home_session_receive(s, i)) return -1;
    return 0;
}

static void *home_session_keepalive(void *arg) {
    HomeSession *s = arg;
    double pulse = 0;
    while (!atomic_load(&s->stop)) {
        int send_pulse = nowd() - pulse >= 1;
        if (send_pulse) pulse = nowd();
        if (home_session_poll(s, send_pulse)) {
            atomic_store(&s->failed, 1);
            g_stopping = 1;
            if (g_signal_engine_fd >= 0) shutdown(g_signal_engine_fd, SHUT_RDWR);
            break;
        }
    }
    return NULL;
}

static void home_session_close(HomeSession *s, int retain_weights) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->fd[i] < 0) continue;
        if (retain_weights) {
            if (lmb_send(s->fd[i], LMB_HOME_DETACH, s->offers[i].id, 32, NULL, 0) ||
                home_session_receive(s, i))
                fprintf(stderr, "[resident] Could not confirm retained allocation on %s. Check its donor screen.\n", s->names[i]);
        } else (void)lmb_send(s->fd[i], LMB_HOME_CANCEL, s->offers[i].id, 32, NULL, 0);
        shutdown(s->fd[i], SHUT_RDWR); lmb_close(s->fd[i]); s->fd[i] = -1;
    }
    home_stop_child(&s->source);
}

static int home_request_chat(LmbTuiState *st, int selected) {
    home_error[0] = 0;
    if (selected < 0 || selected >= st->nmodels || !st->tracker[0] ||
        !st->inventory_ok || home_private_network())
        return home_fail("Cannot prepare chat: refresh the household inventory and select a model.");
    LmbTuiModel *m = &st->models[selected];
    if (!m->checkpoint_inventory_ok)
        return home_fail("Cannot inventory the source checkpoint. Check its files and refresh the model list.");
    if (!m->weights_present || !m->shape.sizing_verified || st->sessions != 1) {
        return home_fail("This checkpoint needs verified sizing, local source weights and a one-session plan.");
    }
    LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES];
    uint32_t indices[LMB_CLUSTER_MAX_NODES], count = 0;
    for (uint32_t i = 0; i < st->nnodes; i++) {
        if (!st->nodes[i].addr[0] || !st->identities[i][0] || !lmb_tui_node_enabled(st, i)) continue;
        indices[count] = i; nodes[count] = st->nodes[i];
        count++;
    }
    LmbClusterPlan plan;
    if (!count || lmb_home_plan_source(&m->shape, m->checkpoint_bytes, nodes, count,
        st->context, 1, LMB_GOAL_ONE_SESSION, 1, &plan) ||
        plan.state != LMB_PLAN_RESIDENT) {
        return home_fail("No complete resident plan found for the selected computers. Keep Share resources open and check their offered RAM.");
    }
    /* Discovery is not proof that inbound connections work. Authenticate the
     * planned donors before indexing/starting a source, without any OFFER,
     * reservation or engine launch. Recheck again when actually sending. */
    for (uint32_t j = 0; j < plan.nslices; j++) {
        uint32_t i = plan.slices[j].node;
        uint8_t expected[32];
        if (lmb_unhex(expected, st->identities[indices[i]], 32))
            return home_fail("Invalid identity for %.64s. Refresh the computer list.", nodes[i].name);
        int fd = lmb_connect_ms_io(nodes[i].addr, 2000, home_control_io_ms(2000));
        if (fd < 0) return home_fail("Cannot reach %.64s at %.64s: %s. Check inbound permissions on that computer; no model was loaded.",
                                    nodes[i].name, nodes[i].addr, lmb_connect_why());
        int matched = lmb_secure_peer_matches(fd, expected);
        int authenticated = matched && !lmb_auth(fd);
        lmb_close(fd);
        if (!authenticated) return home_fail("Cannot authenticate %.64s at %.64s. %s No model was loaded.",
            nodes[i].name, nodes[i].addr, matched ? "Check the household key and whether the donor is responding in time." : "The donor identity changed; refresh the computer list.");
    }
    HomeSession s = {0};
    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++) s.fd[i] = -1;
    char kp[1024], pkhex[65], idhex[65];
    uint8_t sk[64], pk[32], id[32];
    if (lmb_peer_identity(lmb_peer_key_path(kp, sizeof kp), sk, pk))
        return home_fail("Cannot load this computer's identity. Check home-directory permissions.");
    memset(sk, 0, sizeof sk); lmb_hex(pkhex, pk, 32);
    lmb_random(id, sizeof id); lmb_hex(idhex, id, 32);
    char model[64]; snprintf(model, sizeof model, "home-%.12s-%.12s-%.24s", pkhex, idhex, m->name);
    for (char *p = model; *p; p++)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-')) *p = '_';
    char dir[1024], maintainer[1200], ip[INET_ADDRSTRLEN], port[20], addr[64], name[64];
    char logdir[1100], logfile[1200];
    exe_dir(dir, sizeof dir); snprintf(maintainer, sizeof maintainer, "%s/maintainer", dir);
    if (access(maintainer, X_OK)) return home_fail("The checkpoint source service is missing. Build the complete household runtime.");
    if (home_local_ip(st->tracker, ip)) return home_fail("Cannot reach the household tracker. Keep Lumabri open on the household owner.");
    int source_port = 0, source_listener = home_listen(&source_port);
    if (source_listener < 0) return home_fail("Cannot open a checkpoint source port: %s.", strerror(errno));
    snprintf(port, sizeof port, "%d", source_port); snprintf(addr, sizeof addr, "%s:%d", ip, source_port);
    snprintf(name, sizeof name, "home-source-%.16s", idhex);
    snprintf(logdir, sizeof logdir, "%s/.lumabri/logs", getenv("HOME") ? getenv("HOME") : ".");
    mkdir_p(logdir); snprintf(logfile, sizeof logfile, "%s/%s.log", logdir, name);
    char *source_argv[] = {maintainer, "--root", m->dir, "--port", port,
        "--tracker", st->tracker, "--name", name, "--model-name", model,
        "--advertise", addr, "--key", kp, NULL};
    HomeTerminal term; home_terminal_begin(&term);
    g_stopping = 0; install_chat_signal_handlers(); signal(SIGPIPE, SIG_IGN);
    s.source = home_spawn(source_argv, NULL, logfile, NULL, source_listener);
    int result = -1;
    const char *stage = "starting the checkpoint source";
    if (s.source <= 0) goto done;
    LmbModelIdentity identity;
    Swarm swarm = {0};
    double started = nowd(), pulse = 0;
    LmbPrepareProgress progress = {0};
    int found = 0;
    stage = "indexing and verifying the checkpoint source";
    while (!g_stopping && nowd() - started < 600) {
        char bar[29], detail[180];
        lmb_prepare_read(&progress, logfile, nowd());
        lmb_prepare_display(&progress.index, 1, nowd(), bar, detail, sizeof detail);
        if (term.active) { ui_begin("prepare chat");
            ui_printf(7, 5, UI_TEXT, "Indexing %s", m->name);
            ui_text(10, 5, UI_MUTED, "Verifying checkpoint identity before requesting allocations.");
            ui_text(12, 5, UI_SAND, bar);
            ui_text(14, 5, UI_TEXT, detail);
            ui_footer("No donor starts without approval.", "Esc cancels"); ui_present();
        } else {
            printf("LUMABRI / PREPARE CHAT\n\nIndexing %s and verifying its checkpoint identity.\n"
                   "No donor engine is running yet.\n\n[q] Cancel\n", m->name);
            printf("%s\n%s\n", bar, detail);
        }
        fflush(stdout);
        if (!lmb_model_identity_get(st->tracker, model, &identity) &&
            !swarm_inspect(st->tracker, model, &swarm) && swarm.total_bytes) { found = 1; break; }
        if (waitpid(s.source, NULL, WNOHANG) == s.source) break;
        int key = home_key(); if (key == 'q' || key == 27 || key == 3) break;
        (void)poll(NULL, 0, 200);
    }
    if (!found) goto done;
    uint8_t indexed_root[32];
    if (lmb_checkpoint_identity(m->dir, m->content_id, model, indexed_root) ||
        memcmp(indexed_root, identity.root, sizeof indexed_root)) m->content_id[0] = 0;
    stage = "validating the indexed plan";
    if (lmb_home_plan_budgets(&m->shape, swarm.total_bytes, nodes, count,
                             st->context, &plan) || plan.state != LMB_PLAN_RESIDENT) {
        home_fail("The indexed checkpoint does not fit the selected budgets. Refresh the plan; no allocation was committed.");
        goto done;
    }
    uint8_t edge_pk[32];
    if (lmb_unhex(edge_pk, st->identities[indices[plan.edge_node]], 32)) goto done;
    /* Each recipient gets one exact range, and only the selected Edge owner
     * gets permission to host text. Edge must also own a Segment slice in v1. */
    int have_edge = 0;
    for (uint32_t j = 0; j < plan.nslices; j++) {
        const LmbSlice *slice = &plan.slices[j]; uint32_t n = slice->node;
        LmbHomeOffer *o = &s.offers[s.count];
        memcpy(o->id, id, 32); memcpy(o->requester, pk, 32);
        memcpy(o->edge_peer, edge_pk, 32); memcpy(o->model_root, identity.root, 32);
        snprintf(o->model, sizeof o->model, "%s", model);
        snprintf(o->model_type, sizeof o->model_type, "%s", m->shape.model_type);
        snprintf(o->tracker, sizeof o->tracker, "%s", st->tracker);
        o->begin = slice->layer_begin; o->end = slice->layer_end; o->layers = m->shape.layers;
        o->context = st->context; o->threads = nodes[n].threads ? nodes[n].threads : 1;
        if (o->threads > 256) o->threads = 256;
        o->max_new = st->quick_calibration ? LMB_QUICK_PROBE_TOKENS : (st->max_new ? st->max_new : 256);
        o->model_bytes = swarm.total_bytes;
        o->runs_edge = n == plan.edge_node;
        LmbHomeReservation reservation;
        if (lmb_home_reservation(&m->shape, swarm.total_bytes, o->begin, o->end,
                                o->context, o->runs_edge, &reservation)) {
            home_fail("The model's memory requirements cannot be represented safely.");
            goto done;
        }
        o->ram_bytes = reservation.total_bytes;
        o->edge_ram_bytes = reservation.edge_bytes;
        /* Resident weights never occupy the file mirror. Keep a bounded
         * allowance for metadata, signed block hashes, sparse-file maps and
         * logs; do not reserve two complete checkpoints on each donor. */
        uint64_t disk_input = home_resident_required() ? swarm.metadata_bytes : swarm.total_bytes;
        o->disk_bytes = lmb_budget_add(lmb_budget_add(disk_input, disk_input), UINT64_C(256) << 20);
        if (home_resident_required())
            o->disk_bytes = lmb_budget_add(o->disk_bytes, swarm.total_bytes / 1024);
        if (o->disk_bytes == UINT64_MAX) {
            home_fail("The model's disk requirements exceed the supported size.");
            goto done;
        }
        if (o->ram_bytes > nodes[n].ram_budget_bytes) {
            home_fail("%.64s needs %.2f GB for this plan, but offers %.2f GB. No allocation was committed.",
                      nodes[n].name, o->ram_bytes / 1e9, nodes[n].ram_budget_bytes / 1e9);
            goto done;
        }
        if (!lmb_home_offer_valid(o)) { home_fail("The proposed allocation failed validation. No allocation was committed."); goto done; }
        snprintf(s.names[s.count], sizeof s.names[0], "%s", nodes[n].name);
        snprintf(s.addresses[s.count], sizeof s.addresses[0], "%s", nodes[n].addr);
        uint8_t recipient[32];
        if (lmb_unhex(recipient, st->identities[indices[n]], 32)) goto done;
        int fd = lmb_connect_ms_io(nodes[n].addr, 1500, home_control_io_ms(1000));
        if (fd < 0) {
            home_fail("Cannot reach %.64s at %.64s while sending the request: %s.", nodes[n].name, nodes[n].addr, lmb_connect_why());
            goto done;
        }
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (!lmb_secure_peer_matches(fd, recipient) || lmb_auth(fd)) {
            home_fail("Authentication failed while requesting %.64s. Refresh the household identity and key.", nodes[n].name);
            lmb_close(fd); goto done;
        }
        LmbBuf b = {0};
        int bad = lmb_home_offer_pack(&b, o) || lmb_send(fd, LMB_HOME_OFFER, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        if (bad) { home_fail("Cannot send the request to %.64s. Check its connection.", nodes[n].name); lmb_close(fd); goto done; }
        s.fd[s.count] = fd; s.phase[s.count] = LMB_HOME_PENDING;
        if (o->runs_edge) { s.edge = s.count; have_edge = 1; }
        s.count++;
        /* Drain the initial acknowledgement before starting heartbeats. A
         * busy donor replies then closes; sending PULSE first can replace its
         * queued rejection with a write error and hide the actual reason. */
        if (home_session_receive(&s, s.count - 1)) {
            if (!s.reason[s.count - 1][0])
                home_fail("No valid allocation acknowledgement from %.64s. Check its connection; no plan was committed.", nodes[n].name);
            goto done;
        }
        if (s.phase[s.count - 1] != LMB_HOME_PENDING) {
            home_fail("Unexpected initial allocation status from %.64s. No plan was committed.", nodes[n].name);
            goto done;
        }
    }
    if (!have_edge || !s.count) goto done;
    int committed = 0, host_started = 0, first_visible = 0;
    started = nowd();
    while (!g_stopping && nowd() - started < 900) {
        stage = !committed ? "waiting for donor approval" :
                !host_started ? "loading the approved segments" : "starting the chat host";
        int send_pulse = nowd() - pulse >= 1;
        if (send_pulse) pulse = nowd();
        if (home_session_poll(&s, send_pulse)) goto done;
        lmb_prepare_read(&progress, logfile, nowd());
        if (term.active) {
            ui_begin("prepare chat");
            ui_printf(6, 5, UI_TEXT, "%s · %u computer(s) · one session", m->name, s.count);
        } else printf("LUMABRI / PREPARE CHAT\n\n%s · %u computer(s) · one session\n\n", m->name, s.count);
        char bar[29], detail[180];
        lmb_prepare_display(&progress.transfer, 0, nowd(), bar, detail, sizeof detail);
        const char *loading = !committed ? "Waiting for approval; no weights are loading" :
                              !host_started ? "Transferring weights and loading approved segments" :
                                              "Loading the chat host; segments are ready";
        if (term.active) {
            ui_text(8, 5, UI_SAND, loading);
            if (committed) {
                ui_text(10, 5, UI_SAND, bar);
                ui_text(12, 5, UI_TEXT, detail);
                ui_text(13, 5, UI_MUTED, home_resident_required() ?
                    "All assigned weights load into RAM before chat. They stay until you stop sharing." :
                    "Weights load on demand; cache reuse and retries change transfer totals.");
            }
        } else {
            printf("%s\n", loading);
            if (committed) printf("%s\n%s\n", bar, detail);
        }
        int first_row = committed ? 15 : 10;
        int visible = term.active ? (ui_h - 5 - first_row) / 2 : (int)s.count;
        if (visible < 1) visible = 1;
        if (first_visible > (int)s.count - visible) first_visible = (int)s.count - visible;
        if (first_visible < 0) first_visible = 0;
        int accepted = 1, ready = 1;
        for (uint32_t i = 0; i < s.count; i++) {
            if (term.active) {
                if ((int)i >= first_visible && (int)i < first_visible + visible)
                    ui_printf(first_row + ((int)i - first_visible) * 2, 5, UI_TEXT,
                              "%s · %s", s.names[i], lmb_home_phase_name(s.phase[i]));
            } else printf("%-20s %s\n", s.names[i], lmb_home_phase_name(s.phase[i]));
            if (s.phase[i] != LMB_HOME_ACCEPTED) accepted = 0;
            if (s.phase[i] < LMB_HOME_SEGMENT_READY) ready = 0;
        }
        if (!term.active) { puts("\nNothing loads until every selected computer accepts.\n[q] Cancel and release all computers"); fflush(stdout); }
        if (term.active) { ui_footer("Chat starts only when the entire approved chain is ready.",
            "↑ ↓ scroll computers   Esc cancels and releases"); ui_present(); }
        int key = home_key(); if (key == 'q' || key == 27 || key == 3) goto done;
        if (key == 1001 && first_visible > 0) first_visible--;
        if (key == 1002 && first_visible + visible < (int)s.count) first_visible++;
        if (!committed && accepted) {
            for (uint32_t i = 0; i < s.count; i++)
                if (lmb_send(s.fd[i], LMB_HOME_COMMIT, id, 32, NULL, 0)) goto done;
            committed = 1;
        }
        if (committed && !host_started && ready) {
            if (lmb_send(s.fd[s.edge], LMB_HOME_START_HOST, id, 32, NULL, 0)) goto done;
            host_started = 1;
        }
        if (host_started && s.phase[s.edge] == LMB_HOME_READY && s.host_port[s.edge]) {
            LmbExecutionView execution = { .count = s.count, .layers = m->shape.layers };
            for (uint32_t i = 0; i < s.count; i++) {
                LmbExecutionNode *node = &execution.nodes[i];
                snprintf(node->name, sizeof node->name, "%s", s.names[i]);
                snprintf(node->address, sizeof node->address, "%s", s.addresses[i]);
                node->begin = s.offers[i].begin; node->end = s.offers[i].end;
                node->edge = s.offers[i].runs_edge;
                node->reserved_bytes = s.offers[i].ram_bytes;
            }
            if (!lmb_execution_valid(&execution)) {
                home_fail("The approved layer allocation is incomplete. Chat was not started.");
                goto done;
            }
            char host[64], ctx[20], token_limit[20];
            const char *colon = strrchr(s.addresses[s.edge], ':');
            if (!colon) goto done;
            snprintf(host, sizeof host, "%.*s:%u", (int)(colon - s.addresses[s.edge]),
                     s.addresses[s.edge], s.host_port[s.edge]);
            snprintf(ctx, sizeof ctx, "%u", st->context);
            snprintf(token_limit, sizeof token_limit, "%u", s.offers[s.edge].max_new);
            home_terminal_end(&term);
            pthread_t heartbeat;
            if (pthread_create(&heartbeat, NULL, home_session_keepalive, &s)) goto done;
            char expected_host[65]; lmb_hex(expected_host, edge_pk, 32);
            char expected_root[65]; lmb_hex(expected_root, identity.root, 32);
            char *chat_argv[] = {"--host", host, "--model", model, "--ctx", ctx,
                                 "--role", "chat", "--max-new", token_limit, "--host-key", expected_host,
                                 "--tracker", st->tracker, "--host-root", expected_root, "--calibrate"};
            if (home_resident_required()) {
                LmbResidentPlan saved = {.context = st->context, .max_new = s.offers[s.edge].max_new,
                    .execution = execution};
                snprintf(saved.tracker, sizeof saved.tracker, "%s", st->tracker);
                snprintf(saved.host, sizeof saved.host, "%s", host);
                snprintf(saved.host_key, sizeof saved.host_key, "%s", expected_host);
                snprintf(saved.root, sizeof saved.root, "%s", expected_root);
                snprintf(saved.model, sizeof saved.model, "%s", model);
                if (home_resident_plan_save(&saved)) {
                    atomic_store(&s.stop, 1); pthread_join(heartbeat, NULL);
                    home_fail("Cannot save the approved resident plan. Check home-directory permissions; no chat was started.");
                    goto done;
                }
            }
            g_execution_view = &execution;
            LmbCalibration measurement = {0}; char measurement_dir[1200];
            LmbTuiModel measured = *m;
            measured.plan = plan;
            measured.planned = 1; /* the actual revalidated plan, not an earlier UI snapshot */
            measured.plan.edge_node = indices[plan.edge_node];
            for (uint32_t j = 0; j < measured.plan.nslices; j++)
                measured.plan.slices[j].node = indices[plan.slices[j].node];
            int can_record = !catalog_calibration_dir(measurement_dir) &&
                !catalog_calibration_key(st, &measured, 0, NULL, &measurement.key);
            if (can_record) {
                LmbMachineReport current[LMB_INVENTORY_MAX]; uint32_t current_count = 0;
                can_record = !lmb_inventory_fetch(st->tracker, current, &current_count);
                for (uint32_t j = 0; can_record && j < measurement.key.nodes; j++) {
                    int matched = 0;
                    for (uint32_t k = 0; k < current_count; k++) {
                        char peer[65], hardware[65]; lmb_hex(peer, current[k].identity, 32);
                        catalog_hardware_id(&current[k].machine, current[k].control_addr, hardware);
                        if (!strcmp(peer, measurement.key.node_id[j]) &&
                            !strcmp(hardware, measurement.key.node_hardware_id[j]) &&
                            !strcmp(current[k].runtime_id, measurement.key.node_build_id[j])) matched = 1;
                    }
                    if (!matched) {
                        fprintf(stderr, "[calibration] Donor %u no longer matches the approved runtime inventory.\n", j + 1);
                        can_record = 0;
                    }
                }
            }
            g_recording_calibration = can_record ? &measurement : NULL;
            g_calibration_directory = can_record ? measurement_dir : NULL;
            if (!can_record)
                fprintf(stderr, "[calibration] No speed will be saved: %s.\n",
                        !m->content_id[0] ? "checkpoint content identity is unavailable" :
                        !st->build_id[0] ? "client binary identity is unavailable" :
                        "the approved plan's runtime identities are incomplete or changed");
            result = cmd_chat(st->quick_calibration ? 17 : 16, chat_argv);
            g_recording_calibration = NULL; g_calibration_directory = NULL;
            g_execution_view = NULL;
            atomic_store(&s.stop, 1); pthread_join(heartbeat, NULL);
            if (atomic_load(&s.failed)) result = -1;
            break;
        }
    }
done:
    home_terminal_end(&term);
    home_session_close(&s, !result && home_resident_required());
    if (result) {
        if (!home_error[0]) {
            for (uint32_t i = 0; i < s.count; i++) if (s.reason[i][0]) {
                home_fail("%.64s: %.240s", s.names[i], s.reason[i]);
                break;
            }
        }
        if (!home_error[0]) home_fail("Chat stopped while %s. Resources were released. Source log: %.240s", stage, logfile);
        fprintf(stderr, "Household plan did not complete; allocations have been cancelled. Source log: %s\n", logfile);
        for (uint32_t i = 0; i < s.count; i++) if (s.reason[i][0])
            fprintf(stderr, "%s: %s\n", s.names[i], s.reason[i]);
    }
    return result;
}
#endif
