/* lumabri_tui.c — see lumabri_tui.h.
 *
 * Written as two screens rather than as a widget system. The roadmap's rule
 * was that the abstraction should come out of having written the same thing
 * twice, and after two screens what actually repeated is small: a frame, a
 * row, a bar, a key hint. Those are functions here. Nothing else was built
 * in advance, and the layout code is straight-line because straight-line is
 * what two views need.
 */
#include "lumabri_tui.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ---- terminal ----------------------------------------------------------- */

static struct termios g_saved;
static int g_saved_valid;
static volatile sig_atomic_t g_resized;
static volatile sig_atomic_t g_quit;

static void on_winch(int sig) { (void)sig; g_resized = 1; }
static void on_int(int sig)   { (void)sig; g_quit = 1; }

/* Leaving a terminal in raw mode is the one bug a TUI can inflict on the
 * shell that outlives it, so the restore runs from the normal exit path, from
 * a signal, and from atexit. Three routes to one idempotent function. */
static void cooked(void) {
    if (!g_saved_valid) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    g_saved_valid = 0;
    fputs("\x1b[?25h\x1b[?1049l", stdout);   /* cursor back, main screen back */
    fflush(stdout);
}

static int raw(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_saved)) return -1;
    struct termios t = g_saved;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t)) return -1;
    g_saved_valid = 1;
    atexit(cooked);
    fputs("\x1b[?1049h\x1b[?25l", stdout);   /* alternate screen, hide cursor */
    return 0;
}

typedef struct { int w, h; } Size;

static Size term_size(void) {
    struct winsize ws;
    Size s = { 100, 30 };
    if (ioctl(1, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) s.w = ws.ws_col;
        if (ws.ws_row > 0) s.h = ws.ws_row;
    }
    /* A terminal narrower than this cannot show the table without wrapping
     * into nonsense, so the compact view takes over rather than the layout
     * silently degrading. */
    if (s.w > 200) s.w = 200;
    return s;
}

/* ---- drawing ------------------------------------------------------------ */

#define DIM   "\x1b[2m"
#define BOLD  "\x1b[1m"
#define OFF   "\x1b[0m"
#define GRN   "\x1b[32m"
#define RED   "\x1b[31m"
#define AMBER "\x1b[38;5;209m"
#define INV   "\x1b[7m"

static int g_color = 1;
static const char *c(const char *seq) { return g_color ? seq : ""; }

static int g_snapshot;
static void clear_screen(void) { if (!g_snapshot) fputs("\x1b[2J\x1b[H", stdout); }
/* Cursor moves are what makes a full screen a screen, and exactly what makes
 * a snapshot unreadable. In snapshot mode a row break is a newline: the same
 * content, in a form a person or a test can read. */
static int g_snap_row;
static void at(int row, int col) {
    if (g_snapshot) {
        /* One newline per row that is actually new, so a snapshot reads as
         * the lines the screen would show rather than as double-spacing. */
        if (col == 1 && row != g_snap_row) { fputc('\n', stdout); g_snap_row = row; }
        return;
    }
    printf("\x1b[%d;%dH", row, col);
}

/* A rule the width of the screen, with a label sitting on it. */
static void rule(int w, const char *label) {
    int used = 0;
    if (label && label[0]) {
        printf("%s%s%s ", c(DIM), label, c(OFF));
        used = (int)strlen(label) + 1;
    }
    for (int i = used; i < w; i++) fputs("─", stdout);
    fputc('\n', stdout);
}

/* The three states, as a mark and a colour. The mark carries the meaning on
 * a terminal with no colour, which is most of the ones this will run on. */
static void state_mark(LmbPlanState s, int planned) {
    if (!planned || s == LMB_PLAN_UNRUNNABLE)
        printf("%s x %s", c(RED), c(OFF));
    else if (s == LMB_PLAN_DISK)
        printf("%s ! %s", c(AMBER), c(OFF));
    else
        printf("%s + %s", c(GRN), c(OFF));
}

static const char *state_word(const LmbTuiModel *m) {
    if (!m->planned) return "cannot plan";
    switch (m->plan.state) {
    case LMB_PLAN_RESIDENT: return "resident";
    case LMB_PLAN_DISK:     return "from disk";
    default:                return "not runnable";
    }
}

/* What the SPEED column says, and the rule it obeys: a number only when a
 * calibration exists for this exact plan. */
static void speed_text(const LmbTuiModel *m, char *out, size_t cap) {
    if (!m->calibration) { snprintf(out, cap, "not calibrated"); return; }
    if (!m->calibration_key_valid) {
        snprintf(out, cap, "stale (plan key unavailable)");
        return;
    }
    lmb_cal_speed_text(m->calibration, &m->calibration_key, out, cap);
}

static void human_bytes(uint64_t b, char *out, size_t cap) {
    if (b >= 1000ull * 1000 * 1000)
        snprintf(out, cap, "%.0f GB", (double)b / 1e9);
    else if (b >= 1000 * 1000)
        snprintf(out, cap, "%.0f MB", (double)b / 1e6);
    else
        snprintf(out, cap, "%llu B", (unsigned long long)b);
}

/* ---- the catalogue ------------------------------------------------------ */

static void draw_header(const LmbTuiState *st, Size sz, int tab) {
    uint64_t ram = 0, vram = 0;
    uint32_t gpu = 0;
    for (uint32_t i = 0; i < st->nnodes; i++) {
        ram += st->nodes[i].ram_budget_bytes;
        vram += st->nodes[i].vram_budget_bytes;
        if (st->nodes[i].gpu_backends) gpu++;
    }
    char rt[32], vt[32];
    human_bytes(ram, rt, sizeof rt);
    human_bytes(vram, vt, sizeof vt);
    at(1, 1);
    printf("%s%sLUMABRI%s   %s%u computer%s · %s usable",
           c(BOLD), c(AMBER), c(OFF), c(DIM), st->nnodes,
           st->nnodes == 1 ? "" : "s", rt);
    if (vram) printf(" · %s VRAM", vt);
    printf(" · %s%s", gpu ? "engine can use a GPU" : "CPU only", c(OFF));
    if (!g_snapshot) fputc('\n', stdout);
    at(2, 1);
    static const char *tabs[] = { "Models", "Computers" };
    for (int i = 0; i < 2; i++)
        printf(" %s%s%s ", i == tab ? c(INV) : c(DIM), tabs[i], c(OFF));
    if (!g_snapshot) printf("\n");
    at(3, 1);
    rule(sz.w, NULL);
}

static void draw_models(const LmbTuiState *st, Size sz, int sel, int top) {
    at(4, 1);
    printf("  %s%-3s %-24s %-14s %-24s %s%s", c(DIM), "", "MODEL", "STATE",
           "", "SPEED", c(OFF));
    if (!g_snapshot) fputc('\n', stdout);
    int rows = sz.h - 7;
    if (rows < 1) rows = 1;
    for (int i = 0; i < rows; i++) {
        int idx = top + i;
        if (idx >= st->nmodels) {
            if (g_snapshot) break;         /* no point padding a snapshot */
            at(5 + i, 1); fputs("\x1b[K", stdout);
            continue;
        }
        at(5 + i, 1);
        if (!g_snapshot) fputs("\x1b[K", stdout);
        const LmbTuiModel *m = &st->models[idx];
        char detail[64] = "", speed[64];
        if (m->planned && m->plan.state == LMB_PLAN_UNRUNNABLE &&
            m->plan.missing_bytes) {
            char miss[32];
            human_bytes(m->plan.missing_bytes, miss, sizeof miss);
            snprintf(detail, sizeof detail, "%s short (~%u more machine%s)",
                     miss, m->plan.missing_nodes,
                     m->plan.missing_nodes == 1 ? "" : "s");
        } else if (m->planned && m->plan.ready_known && m->plan.ready_seconds > 0)
            snprintf(detail, sizeof detail, "ready in ~%.0f min",
                     m->plan.ready_seconds / 60.0);
        else if (m->planned)
            snprintf(detail, sizeof detail, "%u slice%s across %u machine%s",
                     m->plan.nslices, m->plan.nslices == 1 ? "" : "s",
                     st->nnodes, st->nnodes == 1 ? "" : "s");
        speed_text(m, speed, sizeof speed);
        printf("%s", idx == sel ? c(INV) : "");
        printf("%s", idx == sel ? "▸" : " ");
        state_mark(m->planned ? m->plan.state : LMB_PLAN_UNRUNNABLE, m->planned);
        /* The detail column is the one that carries "98 GB short (~6 more
         * machines)", and cutting it mid-word turns the only actionable
         * sentence on the screen into noise. It gets whatever the terminal
         * has left after the fixed columns, and the speed goes last so it
         * can be dropped on a narrow terminal rather than truncating the
         * thing a person acts on. */
        int fixed = 4 + 24 + 1 + 14 + 1;
        int room = sz.w - fixed - 17;
        if (room < 12) room = 12;
        printf("%-24.24s %-14.14s %-*.*s", m->name, state_word(m),
               room, room, detail);
        if (sz.w >= fixed + room + 17) printf(" %-16.16s", speed);
        printf("%s", idx == sel ? c(OFF) : "");
        if (!g_snapshot) fputc('\n', stdout);
    }
    at(sz.h - 1, 1);
    rule(sz.w, NULL);
    at(sz.h, 1);
    printf("%s ↑↓ select   ⏎ details   tab switch   r refresh   "
           "q quit%s", c(DIM), c(OFF));
    fflush(stdout);
}

static void draw_nodes(const LmbTuiState *st, Size sz) {
    at(4, 1);
    printf("  %s%-20s %-12s %-12s %-12s %s%s", c(DIM), "COMPUTER", "RAM",
           "VRAM", "DISK READ", "ENGINE", c(OFF));
    if (!g_snapshot) fputc('\n', stdout);
    for (uint32_t i = 0; i < st->nnodes && (int)i < sz.h - 7; i++) {
        const LmbClusterNode *n = &st->nodes[i];
        char ram[32], vram[32], disk[32];
        human_bytes(n->ram_budget_bytes, ram, sizeof ram);
        human_bytes(n->vram_budget_bytes, vram, sizeof vram);
        if (n->disk_read_bps) snprintf(disk, sizeof disk, "%.0f MB/s",
                                       n->disk_read_bps / 1e6);
        else snprintf(disk, sizeof disk, "unmeasured");
        at(5 + (int)i, 1);
        if (!g_snapshot) fputs("\x1b[K", stdout);
        printf("  %-20.20s %-12s %-12s %-12s %s", n->name, ram,
               n->vram_budget_bytes ? vram : "—", disk,
               /* A card the engine cannot drive is not a GPU machine, and
                * saying so here is the difference between a plan that works
                * and a promise that does not. */
               n->gpu_backends ? "can use its GPU" : "CPU only");
        if (!g_snapshot) fputc('\n', stdout);
    }
    at(sz.h - 1, 1);
    rule(sz.w, NULL);
    at(sz.h, 1);
    printf("%s tab switch   r refresh   q quit%s", c(DIM), c(OFF));
    fflush(stdout);
}

static void draw_compact(const LmbTuiState *st, Size sz, int tab, int sel) {
    clear_screen();
    at(1, 1); printf("%s%sLUMABRI%s  %u computer%s", c(BOLD), c(AMBER),
                     c(OFF), st->nnodes, st->nnodes == 1 ? "" : "s");
    at(2, 1); printf("%s%s%s", c(INV), tab ? "COMPUTERS" : "MODELS", c(OFF));
    int room = sz.w > 8 ? sz.w - 8 : 8;
    if (tab) {
        for (uint32_t i = 0; i < st->nnodes && (int)i + 4 < sz.h; i++) {
            at(4 + (int)i, 1);
            printf("%c %-*.*s", (int)i == sel ? '>' : ' ', room, room,
                   st->nodes[i].name);
        }
    } else {
        for (int i = 0; i < st->nmodels && i + 4 < sz.h; i++) {
            at(4 + i, 1);
            printf("%c %-*.*s %s", i == sel ? '>' : ' ', room / 2, room / 2,
                   st->models[i].name, state_word(&st->models[i]));
        }
    }
    at(sz.h > 1 ? sz.h : 1, 1);
    printf("%s↑↓  tab  r  q%s", c(DIM), c(OFF));
    fflush(stdout);
}

/* ---- the detail --------------------------------------------------------- */

static void draw_detail(const LmbTuiState *st, Size sz, int sel) {
    const LmbTuiModel *m = &st->models[sel];
    LmbRangeCost whole = lmb_estimate_segment(&m->shape, 0, m->shape.layers,
                                              st->context, st->sessions);
    LmbRangeCost edge = lmb_estimate_edge(&m->shape, st->context, st->sessions);
    uint64_t budget = 0;
    for (uint32_t i = 0; i < st->nnodes; i++)
        budget += st->nodes[i].ram_budget_bytes;

    clear_screen();
    at(1, 1);
    printf("%s%s%s%s   %s%u layers · %s%s\n", c(BOLD), c(AMBER), m->name,
           c(OFF), c(DIM), m->shape.layers,
           m->shape.model_type[0] ? m->shape.model_type : "?", c(OFF));
    at(2, 1); rule(sz.w, NULL);

    int row = 4;
    at(row++, 1);
    printf("  %sWHAT IT NEEDS%s", c(DIM), c(OFF));
    if (!whole.ok || !edge.ok) {
        at(row++, 1);
        printf("    adapter-specific sizing is unavailable; no fit decision");
        at(sz.h, 1);
        printf("%s ↵ back   q quit%s", c(DIM), c(OFF));
        fflush(stdout);
        return;
    }
    char t[32];
    human_bytes(whole.resident_bytes, t, sizeof t);
    at(row++, 1); printf("    every weight resident      %10s", t);
    human_bytes(whole.working_set_bytes, t, sizeof t);
    at(row++, 1); printf("    working set (the floor)    %10s", t);
    human_bytes(edge.resident_bytes, t, sizeof t);
    at(row++, 1); printf("    edge, embedding and head   %10s", t);
    human_bytes(whole.state_bytes + edge.state_bytes, t, sizeof t);
    at(row++, 1); printf("    state for %u session%s at %u  %10s", st->sessions,
                         st->sessions == 1 ? "" : "s", st->context, t);
    if (g_snapshot) fputc('\n', stdout);
    row++;
    human_bytes(budget, t, sizeof t);
    at(row++, 1); printf("  %sTHIS CLUSTER HAS%s          %10s", c(DIM), c(OFF), t);

    if (m->planned && m->plan.state == LMB_PLAN_UNRUNNABLE) {
        human_bytes(m->plan.missing_bytes, t, sizeof t);
        at(row++, 1);
        printf("  %smissing%s                   %s%10s%s   ≈ %u more machine%s "
               "the size of these", c(RED), c(OFF), c(RED), t, c(OFF),
               m->plan.missing_nodes, m->plan.missing_nodes == 1 ? "" : "s");
        if (g_snapshot) fputc('\n', stdout);
        row++;
        at(row++, 1);
        printf("  %sadding computers raises what this cluster can hold. It "
               "raises the speed of one%s", c(DIM), c(OFF));
        at(row++, 1);
        printf("  %schat only when the new plan removes disk reads or brings "
               "faster hardware.%s", c(DIM), c(OFF));
    } else if (m->planned) {
        if (g_snapshot) fputc('\n', stdout);
        row++;
        at(row++, 1);
        printf("  %sHOW IT WOULD BE SPLIT%s", c(DIM), c(OFF));
        for (uint32_t i = 0; i < m->plan.nslices && row < sz.h - 4; i++) {
            const LmbSlice *s = &m->plan.slices[i];
            const LmbClusterNode *n = &st->nodes[s->node];
            char held[32], fetch[32];
            human_bytes(s->bytes_resident, held, sizeof held);
            human_bytes(s->bytes_to_fetch, fetch, sizeof fetch);
            at(row++, 1);
            printf("    %-18.18s %s%slayers %u–%u%s   %10s%s",
                   n->name, s->node == m->plan.edge_node ? "edge + " : "",
                   "", s->layer_begin, s->layer_end - 1, "", held,
                   s->state == LMB_PLAN_DISK ? "  (partly from disk)" : "");
            if (s->bytes_to_fetch)
                printf("  %s+%s to fetch%s", c(DIM), fetch, c(OFF));
        }
        row++;
        at(row++, 1);
        if (m->plan.ready_known)
            printf("  %sready in about %.0f minutes once started%s", c(DIM),
                   m->plan.ready_seconds / 60.0, c(OFF));
        else
            printf("  %sready-in unknown: no bandwidth has been measured "
                   "between these machines%s", c(DIM), c(OFF));
    }

    if (g_snapshot) fputc('\n', stdout);
    row++;
    char speed[64];
    speed_text(m, speed, sizeof speed);
    at(row++, 1);
    printf("  %sSPEED%s   %s", c(DIM), c(OFF), speed);
    if (!m->calibration) {
        at(row++, 1);
        printf("  %sa number appears here after a calibration on these "
               "machines with this plan%s", c(DIM), c(OFF));
    }

    at(sz.h, 1);
    printf("%s ↵ back   q quit%s", c(DIM), c(OFF));
    fflush(stdout);
}

/* ---- input and the loop ------------------------------------------------- */

/* One key, or 0 when nothing arrived before the timeout. Arrow keys arrive
 * as three bytes; anything else unrecognised is dropped rather than acted
 * on, because a stray escape sequence must not move a selection. */
static int read_key(int timeout_ms) {
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return 0;
    unsigned char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1) return 0;
    if (ch != 0x1b) return ch;
    unsigned char seq[2];
    struct pollfd q = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&q, 1, 20) <= 0) return 0x1b;
    if (read(STDIN_FILENO, seq, 1) != 1) return 0x1b;
    if (seq[0] != '[') return 0x1b;
    if (poll(&q, 1, 20) <= 0) return 0x1b;
    if (read(STDIN_FILENO, seq + 1, 1) != 1) return 0x1b;
    switch (seq[1]) {
    case 'A': return 'k';        /* up    */
    case 'B': return 'j';        /* down  */
    default:  return 0;
    }
}

int lmb_tui_run(LmbTuiState *st, int snapshot, const char *keys) {
    int sel = 0, top = 0, tab = 0, detail = 0;
    Size sz = term_size();

    if (snapshot) {
        /* One frame, plain, to stdout: this is how the screen is tested
         * without a pty, and how it behaves when its output is a pipe.
         *
         * The keys are applied for real rather than looked at — a snapshot
         * that ignored navigation would let the selection break without any
         * test noticing, which is precisely the bug a screen test exists to
         * catch. Same switch as the live loop, deliberately. */
        g_color = 0;
        g_snapshot = 1;
        for (const char *k = keys ? keys : ""; *k; k++) {
            if (detail) { if (*k == '\r' || *k == '\n') detail = 0; continue; }
            switch (*k) {
            case 'j': if (sel + 1 < st->nmodels) sel++; break;
            case 'k': if (sel > 0) sel--; break;
            case '\t': tab = !tab; break;
            case 'r':
                if (st->refresh) {
                    (void)st->refresh(st, st->refresh_context);
                    if (!st->nmodels) sel = top = 0;
                    else if (sel >= st->nmodels) sel = st->nmodels - 1;
                }
                break;
            case '\r': case '\n':
                if (st->nmodels && sz.w >= 60 && sz.h >= 12) detail = 1;
                break;
            default: break;
            }
        }
        int rows = sz.h - 7; if (rows < 1) rows = 1;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        if (sz.w < 60 || sz.h < 12) draw_compact(st, sz, tab, sel);
        else if (detail) draw_detail(st, sz, sel);
        else {
            draw_header(st, sz, tab);
            if (tab == 0) draw_models(st, sz, sel, top);
            else draw_nodes(st, sz);
        }
        fputc('\n', stdout);
        return 0;
    }

    if (raw()) {
        fprintf(stderr, "lumabri: not a terminal; try `lumabri models` for a "
                        "plain listing\n");
        return 1;
    }
    signal(SIGWINCH, on_winch);
    signal(SIGINT, on_int);

    const char *kp = keys;
    for (;;) {
        if (g_quit) break;
        if (g_resized) { g_resized = 0; sz = term_size(); clear_screen(); }
        if (sz.w < 60 || sz.h < 12) draw_compact(st, sz, tab, sel);
        else if (detail) draw_detail(st, sz, sel);
        else {
            clear_screen();
            draw_header(st, sz, tab);
            if (tab == 0) draw_models(st, sz, sel, top);
            else draw_nodes(st, sz);
        }
        int k;
        if (kp) { k = *kp ? (unsigned char)*kp++ : 'q'; }
        else k = read_key(250);
        if (!k) continue;
        if (k == 'q' || k == 3) break;
        if (detail) { if (k == '\r' || k == '\n' || k == 0x1b) detail = 0; continue; }
        switch (k) {
        case 'j': if (sel + 1 < st->nmodels) sel++; break;
        case 'k': if (sel > 0) sel--; break;
        case '\t': tab = !tab; break;
        case 'r':
            if (st->refresh) {
                int selected = sel;
                (void)st->refresh(st, st->refresh_context);
                if (!st->nmodels) sel = top = 0;
                else if (selected >= st->nmodels) sel = st->nmodels - 1;
            }
            break;
        case '\r': case '\n':
            if (st->nmodels && sz.w >= 60 && sz.h >= 12) detail = 1;
            break;
        default: break;
        }
        int rows = sz.h - 7; if (rows < 1) rows = 1;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
    cooked();
    return 0;
}
