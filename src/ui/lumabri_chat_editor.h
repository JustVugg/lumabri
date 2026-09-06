/* Included by lumabri.c: editor shares the chat lifecycle and live status. */
#ifndef LUMABRI_CHAT_EDITOR_H
#define LUMABRI_CHAT_EDITOR_H
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

/* The popup owns only freshly reserved rows below the input. It never
 * clears transcript rows above it; Enter and Escape remove just these rows. */
static void le_command_menu(const char *buf, int pos, int selected, int show, int *reserved) {
    int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
    int count = show && g_slash_completion ? chat_command_matches(buf, indices) : 0;
    if (count && !*reserved) {
        int rows = term_h() > 12 ? 6 : 3;
        for (int i = 0; i < rows; i++) printf("\r\n");
        printf("\x1b[%dA\x1b[%dG", rows, 5 + le_cols(buf, 0, pos));
        *reserved = rows;
    }
    if (!*reserved) return;
    int rows = *reserved;
    if (selected < 0 || selected >= count) selected = 0;
    int first = selected >= rows - 1 ? selected - rows + 2 : 0;
    printf("\x1b" "7");
    for (int i = 0; i < rows; i++) {
        printf("\x1b[1B\r\x1b[2K");
        if (!count) continue;
        int at = first + i;
        if (i == rows - 1) printf("  %s↑↓ choose · Tab complete · Enter run · Esc dismiss%s", C_DIM, C_R);
        else if (at < count) {
            int k = indices[at];
            printf("  %s%s %-10s %.*s%s", at == selected ? "\x1b[7m" : "",
                   at == selected ? ">" : " ", CHAT_COMMANDS[k],
                   term_w() > 20 ? term_w() - 20 : 1, CHAT_COMMAND_HELP[k], C_R);
        }
    }
    printf("\x1b" "8");
    if (!count) *reserved = 0;
    fflush(stdout);
}

/* This machine as a donor, for the frame above the idle prompt: experts
 * held, calls served and the rate since the last look, work in flight,
 * bytes served by its storage. Read from the tracker's nominative counters
 * (the donor children never talk to the TUI directly). */
static char g_donor_base[48];
static uint64_t g_donor_last_calls;
static double g_donor_last_at;
static int donor_status_line(char *out, size_t cap) {
    if (!g_donor_base[0] || !g_live.tracker[0]) return 0;
    SwarmDetailRow rows[64];
    int n = swarm_detail(g_live.tracker, rows, 64);
    if (n <= 0) return 0;
    uint64_t calls = 0, inflight = 0, resident = 0, resident_bytes = 0, served = 0;
    int mine = 0, nexperts = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(rows[i].name, g_donor_base, strlen(g_donor_base))) continue;
        mine++;
        if (rows[i].roles & LMB_SWARM_ROLE_EXPERT) {
            nexperts += (int)rows[i].nexperts;
            calls += rows[i].exec_calls; inflight += rows[i].exec_inflight;
            resident += rows[i].resident_experts;
            resident_bytes += rows[i].expert_resident_bytes;
        }
        if (rows[i].roles & LMB_SWARM_ROLE_STORAGE) served += rows[i].served_bytes;
    }
    if (!mine) return 0;
    double now = nowd(), rate = 0;
    if (g_donor_last_at > 0 && now > g_donor_last_at && calls >= g_donor_last_calls)
        rate = (double)(calls - g_donor_last_calls) / (now - g_donor_last_at);
    g_donor_last_calls = calls; g_donor_last_at = now;
    int len = snprintf(out, cap, "donor: %d experts", nexperts);
    if (resident)
        len += snprintf(out + len, cap - (size_t)len, " (%llu in RAM, %.1f GB)",
                        (unsigned long long)resident, (double)resident_bytes / 1e9);
    len += snprintf(out + len, cap - (size_t)len, " \xc2\xb7 %llu calls",
                    (unsigned long long)calls);
    if (rate > 0) len += snprintf(out + len, cap - (size_t)len, " (%.1f/s)", rate);
    if (inflight) len += snprintf(out + len, cap - (size_t)len, " \xc2\xb7 %llu in flight",
                                  (unsigned long long)inflight);
    if (served) len += snprintf(out + len, cap - (size_t)len, " \xc2\xb7 disk %.1f GB served",
                                (double)served / 1e9);
    return len > 0;
}

static int line_edit(char *buf, size_t cap) {
    struct termios old, raw;
    if (g_chat_term_valid) old = g_chat_term;
    else if (tcgetattr(0, &old)) return -2;    /* not a real tty -> caller fgets */
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
    int command_selected = 0, command_rows = 0, command_hidden = 0;
    if (!save) { tcsetattr(0, TCSANOW, &old); return -2; }   /* -> caller fgets */
    save[0] = 0;
    buf[0] = 0;

    for (;;) {
        unsigned char c;
        /* A donor can disappear while this editor is idle. The control
         * worker sets g_stopping; do not wait for another user keystroke. */
        if (g_stopping) { rc = -1; break; }
        struct pollfd input_ready = {0, POLLIN, 0};
        int input_poll = poll(&input_ready, 1, 100);
        if ((input_poll == 0 && !g_donor_base[0]) || (input_poll < 0 && errno == EINTR)) continue;
        if (input_poll < 0) { rc = -1; break; }
        if (g_donor_base[0]) {
            /* a donor refreshes the frame above the prompt every 3 s while
             * the user is idle: calls served, experts held, work in flight */
            struct pollfd pfd = { 0, POLLIN, 0 };
            int pr = poll(&pfd, 1, 3000);
            if (pr < 0 && errno != EINTR) { rc = -1; break; }
            if (pr == 0) {
                char status[200] = "";
                if (donor_status_line(status, sizeof status)) {
                    printf("\x1b" "7\x1b[1A\r\x1b[2K");
                    hline_text("\xe2\x95\xad", "\xe2\x95\xae", term_w() - 2, status);
                    printf("\x1b" "8");
                    fflush(stdout);
                }
                continue;
            }
            if (pr < 0) { if (g_stopping) { rc = -1; break; } continue; }
        }
        ssize_t rn = read(0, &c, 1);
        if (rn < 0 && errno == EINTR) {
            if (g_stopping) { rc = -1; break; }
            continue;
        }
        if (rn <= 0) { rc = -1; break; }

        if (c == '\r' || c == '\n') {
            int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
            int n = command_hidden || !g_slash_completion ? 0 : chat_command_matches(buf, indices);
            if (n) le_set(buf, cap, &len, &pos, CHAT_COMMANDS[indices[command_selected % n]]);
            le_command_menu(buf, pos, 0, 0, &command_rows);
            printf("\r\n"); break;
        }
        if (c == 3) {                            /* Ctrl-C: cancel, keep old semantics */
            tcsetattr(0, TCSANOW, &old);
            printf("\r\n");
            raise(SIGINT);
            rc = -1; len = 0; break;
        }
        if (c == 26) {                           /* Ctrl-Z: suspend, terminal restored */
            tcsetattr(0, TCSANOW, &old);
            raise(SIGTSTP);
            if (g_stopping) { rc = -1; len = 0; break; }
            tcsetattr(0, TCSANOW, &raw);         /* resumed: back to raw */
            continue;
        }
        if (c == 28) {                           /* Ctrl-\: conventional SIGQUIT */
            tcsetattr(0, TCSANOW, &old);
            printf("\r\n");
            raise(SIGQUIT);
            rc = -1; len = 0; break;
        }
        if (c != 27 && c != '\t') { command_selected = 0; command_hidden = 0; }
        if (c == '\t') {
            int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
            int n = g_slash_completion ? chat_command_matches(buf, indices) : 0;
            if (n) le_set(buf, cap, &len, &pos, CHAT_COMMANDS[indices[command_selected % n]]);
            command_selected = 0;
        } else if (c == 4) {                    /* Ctrl-D: EOF on empty, else Delete */
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
            struct pollfd escape = {0, POLLIN, 0};
            if (poll(&escape, 1, 100) <= 0) {
                command_hidden = 1;
                le_command_menu(buf, pos, 0, 0, &command_rows);
                continue;
            }
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
                int indices[sizeof CHAT_COMMANDS / sizeof *CHAT_COMMANDS];
                int matches = command_hidden || !g_slash_completion ? 0 : chat_command_matches(buf, indices);
                if (matches) {
                    command_selected = (command_selected + (b == 'A' ? matches - 1 : 1)) % matches;
                    le_command_menu(buf, pos, command_selected, 1, &command_rows);
                    continue;
                }
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
        le_command_menu(buf, pos, command_selected, !command_hidden, &command_rows);
        fflush(stdout);
    }

    buf[len] = 0;
    le_command_menu(buf, pos, 0, 0, &command_rows);
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
#endif
