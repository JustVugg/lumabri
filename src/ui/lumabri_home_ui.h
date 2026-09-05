/* Household launcher. Connections are configured in the terminal, not flags.
 * No multicast discovery claim: membership uses one explicit LAN endpoint. */
#ifndef LUMABRI_HOME_UI_H
#define LUMABRI_HOME_UI_H
typedef struct {
    char tracker[256], token[LMB_TOKEN_MAX + 1], models[512], ram[32];
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
    int bad = fprintf(f, "tracker=%s\ntoken=%s\nmodels=%s\nram=%s\n",
                      s->tracker, s->token, s->models, s->ram) < 0;
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
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return -1;
    int found = -1;
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *a = (struct sockaddr_in *)p->ifa_addr;
        uint32_t host = ntohl(a->sin_addr.s_addr);
        if ((host >> 24) == 127 || !host) continue;
        if (inet_ntop(AF_INET, &a->sin_addr, out, cap)) { found = 0; break; }
    }
    freeifaddrs(all); return found;
}

static int cmd_home(void) {
    if (!isatty(0) || home_private_network()) return 1;
    HomeSettings s; home_settings_load(&s);
    pid_t tracker_child = 0;
    char notice[200] = "";
    g_stopping = 0; install_chat_signal_handlers();
    while (!g_stopping) {
        HomeTerminal term; home_terminal_begin(&term);
        fputs("\x1b[2J\x1b[HLUMABRI / YOUR COMPUTERS\n\n", stdout);
        printf("Household: %s\n\n[c] Chat\n[d] Share resources (requests require your approval)\n\n"
               "[n] Create a household on this computer\n[j] Join a household\n"
               "[s] Model folder and RAM limit\n[q] Exit\n\n%s\n",
               s.tracker[0] ? s.tracker : "not connected", notice);
        fflush(stdout);
        int key = -1;
        while (!g_stopping && key < 0) { key = home_key(); (void)poll(NULL, 0, 50); }
        home_terminal_end(&term);
        if (key == 'q' || key == 3 || g_stopping) break;
        notice[0] = 0;
        if (key == 'j') {
            if (tracker_child > 0) {
                snprintf(notice, sizeof notice, "Close this household before joining another."); continue;
            }
            HomeSettings next = s;
            if (home_field("Household LAN address (IP:port)", next.tracker, sizeof next.tracker, 0) ||
                home_field("Household key from its owner", next.token, sizeof next.token, 1) ||
                !next.tracker[0] || !next.token[0]) continue;
            char normalized[256];
            if (tracker_addr_set(normalized, sizeof normalized, next.tracker)) continue;
            snprintf(next.tracker, sizeof next.tracker, "%s", normalized);
            s = next;
            if (home_settings_save(&s)) snprintf(notice, sizeof notice, "Could not save household settings.");
        } else if (key == 's') {
            HomeSettings next = s;
            if (home_field("Folder containing your model directories", next.models, sizeof next.models, 0) ||
                home_field("Maximum RAM to offer, in GB", next.ram, sizeof next.ram, 0)) continue;
            char *end; double ram = strtod(next.ram, &end);
            if (*end || !isfinite(ram) || ram <= 0 || ram > 1048576) {
                snprintf(notice, sizeof notice, "RAM limit must be a positive number of GB."); continue;
            }
            s = next;
            if (home_settings_save(&s)) snprintf(notice, sizeof notice, "Could not save settings.");
        } else if (key == 'n') {
            if (tracker_child > 0) { snprintf(notice, sizeof notice, "This household is already running."); continue; }
            char ip[INET_ADDRSTRLEN], dir[1024], binary[1200], log[1200], port[20];
            if (home_interface_ip(ip, sizeof ip)) { snprintf(notice, sizeof notice, "No active IPv4 LAN interface found."); continue; }
            int chosen = home_free_port(); if (!chosen) continue;
            uint8_t secret[16]; lmb_random(secret, sizeof secret); lmb_hex(s.token, secret, sizeof secret);
            snprintf(s.tracker, sizeof s.tracker, "%s:%d", ip, chosen);
            snprintf(port, sizeof port, "%d", chosen);
            exe_dir(dir, sizeof dir);
            snprintf(binary, sizeof binary, "%s/tracker", dir);
            snprintf(log, sizeof log, "%s/.lumabri/home-tracker.log", getenv("HOME") ? getenv("HOME") : ".");
            if (setenv("LUMABRI_TOKEN", s.token, 1)) continue;
            char *args[] = {binary, "--port", port, NULL};
            tracker_child = home_spawn(args, NULL, log, NULL);
            if (tracker_child <= 0) { snprintf(notice, sizeof notice, "Could not start the household tracker."); continue; }
            int ready = 0;
            for (int attempt = 0; attempt < 20 && !g_stopping; attempt++) {
                int fd = lmb_connect_ms_io(s.tracker, 250, 500);
                if (fd >= 0) { ready = !lmb_auth(fd); lmb_close(fd); }
                if (ready || waitpid(tracker_child, NULL, WNOHANG) == tracker_child) break;
                (void)poll(NULL, 0, 100);
            }
            if (!ready) {
                home_stop_child(&tracker_child);
                home_settings_load(&s);
                snprintf(notice, sizeof notice, "Household tracker did not become ready. Check home-tracker.log."); continue;
            }
            (void)home_settings_save(&s);
            printf("\nOn your other computers choose Join a household.\n\nAddress: %s\nHousehold key: %s\n\n"
                   "Keep this Lumabri window open. Only share the key with your household.\nPress Enter to continue.\n", s.tracker, s.token);
            char line[16]; if (!fgets(line, sizeof line, stdin)) break;
        } else if (key == 'c' || key == '\r' || key == '\n' || key == 'd') {
            if (!s.tracker[0] || !s.token[0]) { snprintf(notice, sizeof notice, "Create or join a household first."); continue; }
            setenv("LUMABRI_TOKEN", s.token, 1);
            int rc;
            if (key == 'd') {
                char *args[] = {"--join", s.tracker, "--ram-gb", s.ram};
                rc = cmd_donor(4, args);
            } else {
                char *args[] = {"--tracker", s.tracker, "--models-dir", s.models};
                rc = cmd_models(4, args);
            }
            g_stopping = 0; install_chat_signal_handlers();
            if (rc) snprintf(notice, sizeof notice, "The operation did not finish. No plan is running; check diagnostics before retrying.");
        }
    }
    home_stop_child(&tracker_child);
    return 0;
}
#endif
