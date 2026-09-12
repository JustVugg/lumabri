/* Household launcher. LAN discovery suggests endpoints; joining still
 * requires a verified host identity and the household key. */
#ifndef LUMABRI_HOME_UI_H
#define LUMABRI_HOME_UI_H
#include "lumabri_home_discovery.h"
typedef struct {
    char tracker[256], token[LMB_TOKEN_MAX + 1], models[512], ram[32];
    int owner;
} HomeSettings;

static int home_settings_path(char *path, size_t cap) {
    char directory[1100];
    if (checked_printf(directory, sizeof directory, "%s/.lumabri",
                       getenv("HOME") ? getenv("HOME") : ".")) return -1;
    mkdir_p(directory);
    return checked_printf(path, cap, "%s/home.conf", directory);
}

static void home_settings_load(HomeSettings *s) {
    memset(s, 0, sizeof *s);
    snprintf(s->models, sizeof s->models, "%s/.lumabri/models", getenv("HOME") ? getenv("HOME") : ".");
    snprintf(s->ram, sizeof s->ram, "4");
    char path[1200], line[1500];
    if (home_settings_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0; eq[strcspn(eq, "\r\n")] = 0;
        if (!strcmp(line, "tracker")) (void)checked_printf(s->tracker, sizeof s->tracker, "%s", eq);
        else if (!strcmp(line, "token")) (void)checked_printf(s->token, sizeof s->token, "%s", eq);
        else if (!strcmp(line, "models")) (void)checked_printf(s->models, sizeof s->models, "%s", eq);
        else if (!strcmp(line, "ram")) (void)checked_printf(s->ram, sizeof s->ram, "%s", eq);
        else if (!strcmp(line, "owner")) s->owner = !strcmp(eq, "1");
    }
    fclose(f);
}

static int home_settings_save(const HomeSettings *s) {
    char path[1200], temporary[1232];
    if (home_settings_path(path, sizeof path) ||
        checked_printf(temporary, sizeof temporary, "%s.XXXXXX", path)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); return -1; }
    int bad = fprintf(f, "tracker=%s\ntoken=%s\nmodels=%s\nram=%s\nowner=%d\n",
                      s->tracker, s->token, s->models, s->ram, s->owner) < 0;
    if (fflush(f) || fsync(fd)) bad = 1;
    if (fclose(f)) bad = 1;
    if (!bad && !rename(temporary, path)) return 0;
    unlink(temporary); return -1;
}

static int home_field(const char *label, char *value, size_t cap, int secret) {
    printf("\n%s%s%s\n", C_BOLD, label, C_R);
    if (value[0]) printf("Enter keeps %s\n", secret ? "the saved household key" : value);
    printf("> "); fflush(stdout);
    char line[1200];
    struct termios old, hidden;
    int hide = secret && !tcgetattr(0, &old);
    if (hide) { hidden = old; hidden.c_lflag &= (tcflag_t)~ECHO; hide = !tcsetattr(0, TCSANOW, &hidden); }
    int got = fgets(line, sizeof line, stdin) != NULL;
    if (hide) { tcsetattr(0, TCSANOW, &old); putchar('\n'); }
    if (!got) return -1;
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) return 0;
    return lmb_inventory_text(line) || checked_printf(value, cap, "%s", line) ? -1 : 0;
}

static int home_interface_ip(char *out, size_t cap) {
    return lmb_home_interface_ip(out, cap);
}

static int home_connection_check(const char *address, const uint8_t *expected_identity) {
    int fd = lmb_connect_ms_io(address, 1200, 1500);
    if (fd < 0) return -1;
    int rc = (expected_identity && !lmb_secure_peer_matches(fd, expected_identity)) || lmb_auth(fd);
    LmbMsg response = {0};
    if (!rc) rc = lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) ||
                  lmb_recv(fd, &response) || response.op != LMB_OK;
    lmb_msg_free(&response); lmb_close(fd);
    return rc ? -2 : 0;
}

/* Failure is an acknowledged screen, not a footer erased by the next frame.
 * Never include household secrets or captured engine output here. */
static void home_error_dialog(const char *title, const char *message) {
    HomeTerminal term; home_terminal_begin(&term);
    while (!g_stopping) {
        ui_begin("action could not complete");
        ui_text(5, 4, UI_SAND, title);
        const char *p = message;
        int width = ui_w > 12 ? ui_w - 8 : 10;
        if (width > 120) width = 120;
        for (int row = 8; *p && row < ui_h - 4; row++) {
            char line[121];
            size_t n = strlen(p);
            if (n > (size_t)width) {
                n = (size_t)width;
                while (n > 0 && p[n] != ' ') n--;
                if (!n) n = (size_t)width;
            }
            memcpy(line, p, n); line[n] = 0;
            ui_text(row, 4, UI_TEXT, line);
            p += n; while (*p == ' ') p++;
        }
        ui_footer("Your previous household settings are unchanged.", "Enter or Esc back   Ctrl-C exit");
        ui_present();
        int key = home_key();
        if (key == 3) { g_stopping = 1; break; }
        if (key == '\r' || key == '\n' || key == 27) break;
        (void)poll(NULL, 0, 50);
    }
    home_terminal_end(&term);
}

/* Persist the household key and fixed tracker endpoint. A second local
 * window may attach to our authenticated tracker; an unrelated listener
 * must never be adopted or displaced. */
static int home_tracker_resume(HomeSettings *s, pid_t *child, char *notice, size_t cap) {
    char ip[INET_ADDRSTRLEN], dir[1024], binary[1200], log[1200], port[20];
    int chosen = lmb_home_port_base();
    if (chosen < 0 || home_interface_ip(ip, sizeof ip)) {
        snprintf(notice, cap, "No usable LAN address or valid household port range."); return -1;
    }
    HomeSettings next = *s;
    if (!next.owner || !next.token[0]) {
        uint8_t secret[16]; lmb_random(secret, sizeof secret);
        lmb_hex(next.token, secret, sizeof secret);
    }
    next.owner = 1;
    snprintf(next.tracker, sizeof next.tracker, "%s:%d", ip, chosen);
    snprintf(port, sizeof port, "%d", chosen);
    setenv("LUMABRI_TOKEN", next.token, 1);
    int probe = lmb_listen(chosen);
    if (probe < 0) {
        if (errno != EADDRINUSE) {
            int saved = errno; setenv("LUMABRI_TOKEN", s->token, 1);
            snprintf(notice, cap, "Cannot open household port %d: %s", chosen, strerror(saved)); return -1;
        }
        if (home_connection_check(next.tracker, g_sec_pk)) {
            setenv("LUMABRI_TOKEN", s->token, 1);
            snprintf(notice, cap, "Household port %d is occupied. No random fallback; close the conflicting service.", chosen);
            return -1;
        }
    } else {
        if (fcntl(probe, F_SETFD, FD_CLOEXEC)) { close(probe); return -1; }
        exe_dir(dir, sizeof dir);
        snprintf(binary, sizeof binary, "%s/tracker", dir);
        if (access(binary, X_OK)) {
            close(probe);
            setenv("LUMABRI_TOKEN", s->token, 1);
            snprintf(notice, cap, "Tracker is not installed. Build the household services before creating a network."); return -1;
        }
        snprintf(log, sizeof log, "%s/.lumabri/home-tracker.log", getenv("HOME") ? getenv("HOME") : ".");
        char *args[] = {binary, "--port", port, "--household-discovery", NULL};
        *child = home_spawn(args, NULL, log, NULL, probe);
        int ready = 0;
        for (int attempt = 0; *child > 0 && attempt < 20 && !g_stopping; attempt++) {
            if (!home_connection_check(next.tracker, g_sec_pk)) { ready = 1; break; }
            if (waitpid(*child, NULL, WNOHANG) == *child) break;
            (void)poll(NULL, 0, 100);
        }
        if (!ready) {
            home_stop_child(child); setenv("LUMABRI_TOKEN", s->token, 1);
            snprintf(notice, cap, "Household tracker did not become ready. Check home-tracker.log."); return -1;
        }
    }
    if (home_settings_save(&next)) {
        home_stop_child(child); setenv("LUMABRI_TOKEN", s->token, 1);
        snprintf(notice, cap, "Could not save the household. No new settings were applied."); return -1;
    }
    *s = next;
    snprintf(notice, cap, "Household ready at %.63s. TCP %d-%d stays fixed across restarts.",
             s->tracker, chosen, chosen + LMB_HOME_PORT_COUNT - 1);
    return 0;
}

/* A hint only becomes an endpoint after an explicit visual identity check.
 * Manual joining stays available when broadcast is filtered by the router. */
static int home_pick_household(HomeSettings *s, uint8_t expected[32]) {
    HomeTerminal term; home_terminal_begin(&term);
    ui_begin("find your household");
    ui_text(7, 5, UI_TEXT, "Looking for households on this LAN...");
    ui_footer("No key or conversation is broadcast.", "Discovery takes about one second."); ui_present();
    uint8_t nonce[16]; lmb_random(nonce, sizeof nonce);
    LmbHomeFound found[LMB_HOME_DISC_MAX]; int count = lmb_home_find(found, nonce), selected = 0;
    if (!count) { home_terminal_end(&term); return 0; }
    for (;;) {
        ui_begin("join a household");
        ui_text(5, 5, UI_TEXT, "Choose your computer, then verify its identity.");
        int rows = (ui_h - 12) / 3; if (rows < 1) rows = 1;
        int top = selected >= rows ? selected - rows + 1 : 0;
        for (int i = top; i <= count && i < top + rows; i++) {
            char label[160], description[160], hex[65];
            if (i == count) {
                snprintf(label, sizeof label, "Enter an address manually");
                snprintf(description, sizeof description, "For networks where discovery is blocked.");
            } else {
                lmb_hex(hex, found[i].identity, 32);
                snprintf(label, sizeof label, "%s", found[i].name);
                snprintf(description, sizeof description, "%s · identity %.16s · not yet trusted", found[i].address, hex);
            }
            ui_item(8 + (i - top) * 3, selected == i, label, description);
        }
        ui_footer("Only pair with a computer you own and recognize.", "↑ ↓ move   Enter select   Esc cancel"); ui_present();
        int key = home_key();
        if (key == 27 || key == 3 || g_stopping) { home_terminal_end(&term); return -1; }
        if (key == 1001) selected = (selected + count) % (count + 1);
        if (key == 1002) selected = (selected + 1) % (count + 1);
        if (key == '\r' || key == '\n') break;
        (void)poll(NULL, 0, 80);
    }
    home_terminal_end(&term);
    if (selected == count) return 0;
    char hex[65], answer[16] = ""; lmb_hex(hex, found[selected].identity, 32);
    printf("\nOn the host, open /create to view its identity. It must match: %.16s\n", hex);
    if (home_field("Type yes only if the identity matches your host", answer, sizeof answer, 0) || strcmp(answer, "yes")) return -1;
    snprintf(s->tracker, sizeof s->tracker, "%s", found[selected].address);
    memcpy(expected, found[selected].identity, 32);
    return 1;
}

static void home_network_setup(char *notice, size_t cap) {
    char ip[INET_ADDRSTRLEN], subnet[64], answer[16] = "", port[20];
    int base = lmb_home_port_base();
    if (base < 0 || home_interface_ip(ip, sizeof ip) || lmb_home_subnet(ip, subnet, sizeof subnet)) {
        snprintf(notice, cap, "Cannot determine the LAN subnet. No firewall changes made."); return;
    }
    printf("\nHousehold network: %s\nStable TCP ports: %d-%d\nDiscovery UDP port: %d\n",
           subnet, base, base + LMB_HOME_PORT_COUNT - 1, base);
    if (!getenv("WSL_DISTRO_NAME")) {
        printf("\nAllow Lumabri in your system firewall on this trusted LAN.\n"
               "Automatic permission setup here is available for Windows/WSL only.\n");
        (void)home_field("Press Enter to return", answer, sizeof answer, 0); return;
    }
    printf("\nWindows will ask for administrator approval. Only this LAN and these ports\n"
           "are allowed. The firewall stays enabled. Use only on a trusted home network.\n");
    if (home_field("Type allow to configure once, remove to undo, or Enter to cancel", answer, sizeof answer, 0) ||
        (strcmp(answer, "allow") && strcmp(answer, "remove"))) return;
    char dir[1024], script[1200], windows[1600];
    exe_dir(dir, sizeof dir);
    if (checked_printf(script, sizeof script, "%s/tools/setup-household-firewall.ps1", dir) ||
        (access(script, R_OK) &&
         (checked_printf(script, sizeof script, "%s/../lib/lumabri/setup-household-firewall.ps1", dir) || access(script, R_OK)))) {
        snprintf(notice, cap, "Network helper is missing beside Lumabri. No firewall changes made."); return;
    }
    int pipefd[2];
    if (lmb_ready_pipe(pipefd)) return;
    pid_t converter = fork();
    if (!converter) {
        close(pipefd[0]); dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
        execlp("wslpath", "wslpath", "-w", script, (char *)NULL); _exit(127);
    }
    close(pipefd[1]);
    FILE *output = fdopen(pipefd[0], "r");
    int got = output && fgets(windows, sizeof windows, output) != NULL;
    if (output) fclose(output); else close(pipefd[0]);
    int status = 0;
    if (converter <= 0 || waitpid(converter, &status, 0) != converter ||
        !WIFEXITED(status) || WEXITSTATUS(status) || !got) {
        snprintf(notice, cap, "Cannot locate the Windows network helper. No firewall changes made."); return;
    }
    windows[strcspn(windows, "\r\n")] = 0;
    snprintf(port, sizeof port, "%d", base);
    pid_t helper = fork();
    if (!helper) {
        char *args[] = {"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", windows, "-Subnet", subnet, "-PortBase", port,
            !strcmp(answer, "remove") ? "-Remove" : NULL, NULL};
        execvp(args[0], args); _exit(127);
    }
    if (helper > 0) {
        pid_t result;
        do result = waitpid(helper, &status, 0); while (result < 0 && errno == EINTR);
        snprintf(notice, cap, "%s", result == helper && WIFEXITED(status) && !WEXITSTATUS(status) ?
            "Network permission updated. Reopen Join/Your computers to test the connection." :
            "Network setup was cancelled or failed. Review the Windows error; no firewall was disabled.");
    }
}

typedef struct {
    double painted;
    int clearing;
} HomeStorageProgress;

static int home_storage_progress(void *arg, const LmbWeightCacheUsage *usage, int erase) {
    HomeStorageProgress *p = arg;
    int key = home_key();
    if (key == 3) g_stopping = 1;
    if (g_stopping || key == 27) return 1;
    double now = nowd();
    if (p->painted && now - p->painted < .1) return 0;
    p->painted = now;
    ui_begin("storage");
    ui_text(5, 5, UI_TEXT, erase ? "Clearing unused household weights..." :
        p->clearing ? "Checking both cache trees before clearing..." :
        "Inspecting household weight caches...");
    ui_printf(8, 5, UI_MUTED, "%llu files visited   %.2f GiB allocated blocks counted",
        (unsigned long long)usage->files, usage->allocated_bytes / 1073741824.0);
    ui_text(11, 5, UI_MUTED, "Cancellation is checked between filesystem operations.");
    ui_footer(p->clearing ? "If clearing has started, removed weights are not restored." :
              "Inspection does not remove any weights.", "Esc cancel   Ctrl-C exit");
    ui_present();
    return 0;
}

static void home_storage_screen(void) {
    char base[1200], message[200] = "";
    if (checked_printf(base, sizeof base, "%s/.lumabri/home",
                       getenv("HOME") ? getenv("HOME") : ".")) return;
    LmbWeightCacheUsage usage = {0};
    int refresh = 1, bad = 0, busy = 0, confirm = 0, choice = 0;
    HomeTerminal term; home_terminal_begin(&term);
    while (!g_stopping) {
        if (refresh) {
            HomeStorageProgress progress = {0};
            bad = lmb_weight_cache_inspect_progress(base, &usage, home_storage_progress, &progress);
            if (bad && errno == ECANCELED) break;
            int lease = home_weight_lease(base);
            busy = lease < 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
            if (lease >= 0) close(lease);
            refresh = 0;
        }
        ui_begin("storage");
        ui_text(5, 5, UI_TEXT, confirm ? "Clear unused household weights?" : "Household weight storage");
        ui_printf(7, 5, UI_MUTED, "%.110s", base);
        if (bad) ui_text(9, 5, UI_SAND, "Cannot safely inspect this cache. Nothing has been removed.");
        else ui_printf(9, 5, UI_TEXT, "Approx. allocated: %.2f GiB   Files: %llu",
            usage.allocated_bytes / 1073741824.0, (unsigned long long)usage.files);
        ui_text(11, 5, busy ? UI_SAND : UI_MUTED, busy ?
            "Weights are in use. Clearing is blocked until the plan releases them." :
            "Only household cas/ and mirrors/ are included; no source model folders.");
        ui_text(13, 5, UI_MUTED, "Keys, settings, conversations and engine logs are kept.");
        ui_text(14, 5, UI_MUTED, "Cleared weights must be fetched again. This does not compact a WSL disk.");
        if (confirm) {
            ui_item(17, choice == 0, "Keep cached weights", "Return without deleting anything.");
            ui_item(20, choice == 1, "Clear unused weights", "Permanent removal; no move to Trash.");
        } else {
            ui_item(17, choice == 0, "Refresh storage", "Recount allocated file blocks; hardlinked entries may be counted twice.");
            ui_item(20, choice == 1, "Clear unused weights", "Review and confirm first; active caches cannot be cleared.");
            ui_item(23, choice == 2, "Back to workspace", "Keep all cached weights.");
        }
        ui_footer(message[0] ? message : "This view covers this user's default household cache only.",
                  "↑ ↓ move   Enter select   Esc back   Ctrl-C exit");
        int compact = ui_h < 28 || ui_w < 60;
        if (compact) {
            ui_begin("storage");
            ui_text(5, 4, UI_SAND, "Resize the terminal to at least 60 × 28.");
            ui_text(7, 4, UI_MUTED, "Esc returns without clearing weights.");
        }
        ui_present();
        int key = home_key(), choices = confirm ? 2 : 3;
        if (key == 3 || key == 27) { if (confirm && key == 27) { confirm = 0; choice = 0; } else break; }
        if (key == 1001) choice = (choice + choices - 1) % choices;
        if (key == 1002) choice = (choice + 1) % choices;
        if (!compact && (key == '\r' || key == '\n')) {
            if (confirm) {
                if (choice == 1) {
                    HomeStorageProgress progress = { .clearing = 1 };
                    int rc = lmb_weight_cache_clear_progress(base, home_storage_progress, &progress);
                    int cancelled = rc == -1 && errno == ECANCELED;
                    snprintf(message, sizeof message, "%s", !rc ?
                        "Household weights cleared. Source checkpoints and settings were preserved." :
                        rc == -2 ?
                        "A plan is using this cache. No cleanup was started." :
                        cancelled ?
                        "Cleanup cancelled. Already removed weights are not restored; remaining weights are kept." :
                        "Cleanup could not finish. Some weights may remain; refresh or check permissions.");
                    refresh = 1;
                }
                confirm = 0; choice = 0;
            } else if (choice == 0) refresh = 1;
            else if (choice == 2) break;
            else if (busy || bad) snprintf(message, sizeof message,
                "Cleanup is unavailable while the cache is active or cannot be inspected safely.");
            else { confirm = 1; choice = 0; }
        }
        (void)poll(NULL, 0, 100);
    }
    home_terminal_end(&term);
}

static int cmd_home(void) {
    if (!isatty(0) || home_private_network()) return 1;
    HomeSettings s; home_settings_load(&s);
    pid_t tracker_child = 0;
    char notice[200] = "";
    int selected = 0, actions = 0;
    g_stopping = 0; install_chat_signal_handlers();
    if (s.owner) (void)home_tracker_resume(&s, &tracker_child, notice, sizeof notice);
    while (!g_stopping) {
        LmbResidentPlan resident_plan;
        int have_resident = s.tracker[0] && !home_resident_plan_load(s.tracker, &resident_plan);
        HomeTerminal term; home_terminal_begin(&term);
        int key = -1;
        while (!g_stopping) {
            ui_begin("your workspace");
            int top = 5;
            if (ui_h >= (actions ? 38 : 34) && ui_w >= 64) {
                for (int r = 0; r < 6; r++) ui_text(top + r, 5, r < 3 ? UI_ACCENT : UI_SAND, WORDMARK[r]);
                ui_text(top + 7, 5, UI_MUTED, "tiny engine, immense swarm");
                if (ui_w >= 100) {
                    ui_text(top + 1, 67, UI_TEXT, "Your computers.");
                    ui_text(top + 2, 67, UI_TEXT, "One shared possibility.");
                    ui_text(top + 4, 67, UI_MUTED, "A private cluster, at home.");
                }
                top += 10;
            }
            static const char *titles[] = {"Start a conversation", "Explore models", "Your computers", "Share resources"};
            static const char *help[] = {"Choose a model and ask your selected donors.", "Memory needs, plans and measured speed.",
                "See resources. Choose who participates.", "Review a request before anything is loaded."};
            static const char *commands[] = {"/create", "/join", "/settings", "/quit", "/network", "/storage"};
            static const char *command_help[] = {"Create or show this household", "Find your household on the LAN and pair with its key",
                "Model folder and maximum RAM to share", "Close Lumabri", "Set up trusted LAN access once (Windows/WSL)",
                "Inspect and clear unused household weight caches"};
            ui_text(top, 5, UI_TEXT, actions ? "Workspace actions" : "What would you like to do?");
            for (int i = 0; i < (actions ? 6 : 4); i++) ui_item(top + 2 + i * 3, selected == i,
                actions ? commands[i] : i == 0 && have_resident ? "New conversation · retained model" : titles[i],
                actions ? command_help[i] : i == 0 && have_resident ?
                    "Reuse the approved plan. Host identity and model are checked on connection." : help[i]);
            ui_footer(notice[0] ? notice : s.tracker[0] ? s.tracker : "Create or join a household with / actions.",
                      "↑ ↓ move   Enter select   / actions   Esc back   Ctrl-C exit");
            if (ui_h < 28 || ui_w < 60) {
                ui_begin("your workspace");
                ui_text(5, 4, UI_SAND, "Resize the terminal to at least 60 × 28.");
                ui_text(7, 4, UI_MUTED, "Esc or Ctrl-C exits.");
            }
            ui_present();
            key = home_key();
            if (key == 3 || (key == 27 && !actions)) break;
            if (key == 27) { actions = 0; selected = 0; }
            if (key == '/') { actions = !actions; selected = 0; }
            int choices = actions ? 6 : 4;
            if (key == 1001) selected = (selected + choices - 1) % choices;
            if (key == 1002) selected = (selected + 1) % choices;
            if ((key == '\r' || key == '\n') && ui_h >= 28 && ui_w >= 60) {
                key = actions ? "njsqfk"[selected] : selected == 0 && have_resident ? 'r' : "ccpd"[selected];
                actions = 0; selected = 0; break;
            }
            (void)poll(NULL, 0, 100);
        }
        home_terminal_end(&term);
        if (key == 'q' || key == 3 || key == 27 || g_stopping) break;
        notice[0] = 0;
        if (key == 'k') {
            home_storage_screen();
        } else if (key == 'f') {
            home_network_setup(notice, sizeof notice);
        } else if (key == 'j') {
            if (tracker_child > 0) {
                snprintf(notice, sizeof notice, "Close this household before joining another."); continue;
            }
            HomeSettings next = s;
            uint8_t expected[32]; int discovered = home_pick_household(&next, expected);
            if (discovered < 0) continue;
            if ((!discovered && home_field("Household LAN address (IP:port)", next.tracker, sizeof next.tracker, 0)) ||
                home_field("Household key from its owner", next.token, sizeof next.token, 1) ||
                !next.tracker[0] || !next.token[0]) continue;
            char normalized[256];
            if (tracker_addr_set(normalized, sizeof normalized, next.tracker)) continue;
            snprintf(next.tracker, sizeof next.tracker, "%s", normalized);
            setenv("LUMABRI_TOKEN", next.token, 1);
            int connected = home_connection_check(next.tracker, discovered ? expected : NULL);
            if (connected) {
                setenv("LUMABRI_TOKEN", s.token, 1);
                snprintf(notice, sizeof notice, "%s", connected == -1 ?
                    "Connection or secure handshake failed. Check the LAN address and Windows/WSL firewall." :
                    "Household authentication failed. Check the household key and tracker.");
                home_error_dialog("Could not join the household", notice);
                continue;
            }
            next.owner = s.owner && !strcmp(s.tracker, next.tracker);
            if (home_settings_save(&next)) {
                setenv("LUMABRI_TOKEN", s.token, 1);
                home_error_dialog("Could not save household settings", "Check that your home directory is writable, then retry.");
            } else {
                s = next;
                snprintf(notice, sizeof notice, "Connected. Open Your computers; helpers must keep Share resources active.");
            }
        } else if (key == 's') {
            HomeSettings next = s;
            if (home_field("Folder containing your model directories", next.models, sizeof next.models, 0) ||
                home_field("Maximum RAM to offer, in GB", next.ram, sizeof next.ram, 0)) continue;
            char *end; double ram = strtod(next.ram, &end);
            if (*end || !isfinite(ram) || ram <= 0 || ram > 1048576) {
                snprintf(notice, sizeof notice, "RAM limit must be a positive number of GB."); continue;
            }
            if (home_settings_save(&next)) home_error_dialog("Could not save settings", "Check that your home directory is writable, then retry.");
            else {
                s = next;
                snprintf(notice, sizeof notice, "Settings saved. Models: %.110s · RAM limit: %.20s GB", s.models, s.ram);
            }
        } else if (key == 'n') {
            if (tracker_child <= 0 && home_tracker_resume(&s, &tracker_child, notice, sizeof notice)) continue;
            char identity[65]; lmb_hex(identity, g_sec_pk, 32);
            printf("\nOn your other computers choose Join a household.\n\nAddress: %s\nHousehold key: %s\n\n"
                   "Host identity: %.16s\nKeep this Lumabri window open. Only share the key with your household.\nPress Enter to continue.\n", s.tracker, s.token, identity);
            char line[16]; if (!fgets(line, sizeof line, stdin)) break;
        } else if (key == 'c' || key == 'p' || key == 'd' || key == 'r') {
            if (!s.tracker[0] || !s.token[0]) { snprintf(notice, sizeof notice, "Create or join a household first."); continue; }
            setenv("LUMABRI_TOKEN", s.token, 1);
            home_error[0] = 0;
            int rc;
            if (key == 'r') {
                rc = home_resident_plan_chat(&resident_plan);
            } else if (key == 'd') {
                char *args[] = {"--join", s.tracker, "--ram-gb", s.ram};
                rc = cmd_donor(4, args);
            } else {
                char *args[] = {"--tracker", s.tracker, "--models-dir", s.models, "--computers"};
                rc = cmd_models(key == 'p' ? 5 : 4, args);
            }
            g_stopping = 0; install_chat_signal_handlers();
            if (rc || home_error[0]) home_error_dialog("Could not complete the operation", home_error[0] ? home_error :
                "The operation did not finish. No plan is running. Check diagnostics before retrying.");
        }
    }
    home_stop_child(&tracker_child);
    return 0;
}
#endif
