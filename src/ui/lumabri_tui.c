/* lumabri_tui.c — see lumabri_tui.h.
 *
 * Written as two screens rather than as a widget system. The roadmap's rule
 * was that the abstraction should come out of having written the same thing
 * twice, and after two screens what actually repeated is small: a frame, a
 * row, a bar, a key hint. Those are functions here. Nothing else was built
 * in advance, and the layout code is straight-line because straight-line is
 * what two views need.
 */
#define _XOPEN_SOURCE 700
#include "lumabri_tui.h"
#include "lumabri_visual.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
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
    if (st->tracker[0]) printf(" %s", st->inventory_ok ? "LAN inventory · plan preview" :
                                                         "TRACKER OFFLINE · plans unavailable");
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
    if (!st->nmodels) {
        at(5, 1); printf("  no checkpoints; switch to Computers to see the LAN");
    }
    for (int i = 0; i < rows && st->nmodels; i++) {
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
        else if (m->planned) {
            uint32_t computers = m->plan.nslices;
            int edge_seen = 0;
            for (uint32_t i = 0; i < m->plan.nslices; i++)
                if (m->plan.slices[i].node == m->plan.edge_node) edge_seen = 1;
            if (!edge_seen) computers++;
            snprintf(detail, sizeof detail, "%u slice%s across %u machine%s",
                     m->plan.nslices, m->plan.nslices == 1 ? "" : "s",
                     computers, computers == 1 ? "" : "s");
        }
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
    printf("%s ↑↓ select   ⏎ details   c request chat   tab switch   r refresh   "
           "q quit%s", c(DIM), c(OFF));
    fflush(stdout);
}

static void draw_nodes(const LmbTuiState *st, Size sz, int sel, int top) {
    at(4, 1);
    printf("  %s%-20s %-12s %-12s %-12s %s%s", c(DIM), "COMPUTER", "RAM OFFERED",
           "VRAM", "CORES/THREADS", "GPU DETECTED", c(OFF));
    if (!g_snapshot) fputc('\n', stdout);
    for (int row = 0; row < (sz.h - 7) / 2 && top + row < (int)st->nnodes; row++) {
        int i = top + row;
        const LmbClusterNode *n = &st->nodes[i];
        const LmbMachineProfile *p = &st->profiles[i];
        char ram[32], vram[32], cores[32], hardware[256];
        human_bytes(n->ram_budget_bytes, ram, sizeof ram);
        human_bytes(n->vram_budget_bytes, vram, sizeof vram);
        snprintf(cores, sizeof cores, "%u/%u", p->physical_cores, n->threads);
        snprintf(hardware, sizeof hardware, "%s · %s/%s", p->cpu_model, p->os, p->arch);
        at(5 + row * 2, 1);
        if (!g_snapshot) fputs("\x1b[K", stdout);
        printf("%c%c%-20.20s %-12s %-12s %-12s %u", i == sel ? '>' : ' ',
               lmb_tui_node_enabled(st, (uint32_t)i) ? '+' : '-', n->name, ram,
               n->vram_budget_bytes ? vram : "—", cores, p->gpu_count);
        at(6 + row * 2, 1);
        printf("    %.*s", sz.w > 8 ? sz.w - 8 : 1, hardware);
        if (!g_snapshot) fputc('\n', stdout);
    }
    at(sz.h - 1, 1);
    rule(sz.w, NULL);
    at(sz.h, 1);
    printf("%s ↑↓ select · space include/exclude · tab switch · q quit%s", c(DIM), c(OFF));
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
    printf("%s ↵ back   c request chat   q quit%s", c(DIM), c(OFF));
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

typedef struct {
    LmbTuiState *next;
    pthread_t thread;
    _Atomic int done;
} RefreshJob;

static void *refresh_thread(void *arg) {
    RefreshJob *job = arg;
    job->next->refresh(job->next, job->next->refresh_context);
    atomic_store(&job->done, 1);
    return NULL;
}

static void refresh_start(RefreshJob *job, const LmbTuiState *st) {
    if (job->next || !st->refresh) return;
    job->next = malloc(sizeof *st);
    if (!job->next) return;
    memcpy(job->next, st, sizeof *st);
    atomic_store(&job->done, 0);
    if (pthread_create(&job->thread, NULL, refresh_thread, job)) {
        free(job->next); job->next = NULL;
    }
}

static double refresh_clock(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Live catalogue uses the approved open canvas; text snapshots retain their
 * stable diagnostic format. Both read the same planner state. */
static void draw_workspace(const LmbTuiState *st, int tab, int sel, int detail,
                           int palette, int action_sel) {
    ui_begin(detail ? "model plan" : tab ? "your computers" : "models");
    uint32_t selected = 0;
    uint64_t ram = 0;
    for (uint32_t i = 0; i < st->nnodes; i++) if (lmb_tui_node_enabled(st, i)) {
        selected++; ram += st->nodes[i].ram_budget_bytes;
    }
    ui_printf(5, 5, UI_TEXT, "%u computers visible · %u selected · %.1f GB offered RAM selected",
              st->nnodes, selected, ram / 1e9);
    ui_text(7, 5, UI_MUTED, "Models     /     Computers     ·     Tab switches views");
    const char *status = st->inventory_ok ? "Donors must approve the allocation before anything is loaded." :
        "TRACKER OFFLINE · No requests can start; check the household address.";
    if (detail && sel < st->nmodels) {
        const LmbTuiModel *m = &st->models[sel];
        char speed[96]; speed_text(m, speed, sizeof speed);
        ui_text(9, 5, UI_SAND, m->name);
        ui_printf(11, 5, UI_MUTED, "%s · %u layers · %u context · %u session(s)",
                  m->shape.model_type, m->shape.layers, st->context, st->sessions);
        ui_printf(13, 5, UI_TEXT, "Plan: %s    Speed: %s", state_word(m), speed);
        if (m->planned && m->plan.state != LMB_PLAN_UNRUNNABLE) {
            for (uint32_t i = 0; i < m->plan.nslices && 16 + (int)i * 2 < ui_h - 7; i++) {
                const LmbSlice *s = &m->plan.slices[i];
                ui_printf(16 + (int)i * 2, 5, UI_TEXT, "%s%s · layers %u–%u · %.2f GB resident",
                    st->nodes[s->node].name, s->node == m->plan.edge_node ? " (chat host)" : "",
                    s->layer_begin, s->layer_end - 1, s->bytes_resident / 1e9);
            }
            ui_text(ui_h - 6, 5, UI_SAND, "Enter requests this plan. Every participating donor must accept.");
        } else {
            ui_printf(16, 5, UI_TEXT, "Missing: %.2f GB · sizing %s · local source weights %s",
                m->plan.missing_bytes / 1e9, m->planned ? "available" : "unavailable",
                m->weights_present ? "present" : "missing");
            ui_text(19, 5, UI_MUTED, "Select computers in Share resources, then review the updated plan.");
        }
    } else if (tab) {
        int rows = (ui_h - 15) / 3; if (rows < 1) rows = 1;
        int top = sel >= rows ? sel - rows + 1 : 0;
        if (!st->nnodes) ui_text(11, 5, UI_MUTED, "No computers reporting. Open Share resources on your other computers.");
        for (int i = top; i < (int)st->nnodes && i < top + rows; i++) {
            const LmbClusterNode *n = &st->nodes[i];
            const LmbMachineProfile *p = &st->profiles[i];
            char title[256], description[512];
            snprintf(title, sizeof title, "[%s] %s", lmb_tui_node_enabled(st, (uint32_t)i) ? "✓" : " ", n->name);
            snprintf(description, sizeof description, "%s · %.1f GB RAM · %u threads · %u GPU detected / %.1f GB VRAM",
                p->cpu_model, n->ram_budget_bytes / 1e9, n->threads, p->gpu_count, p->vram_available_bytes / 1e9);
            ui_item(10 + (i - top) * 3, i == sel, title, description);
        }
        ui_text(ui_h - 6, 5, UI_MUTED, "Nothing is selected automatically. GPU detected does not mean GPU execution.");
    } else {
        int rows = (ui_h - 15) / 3; if (rows < 1) rows = 1;
        int top = sel >= rows ? sel - rows + 1 : 0;
        if (!st->nmodels) {
            ui_text(11, 5, UI_TEXT, "No checkpoints in your model folder yet.");
            ui_text(13, 5, UI_MUTED, st->root);
            ui_text(16, 5, UI_MUTED, "Set your existing model folder in workspace /settings.");
        }
        for (int i = top; i < st->nmodels && i < top + rows; i++) {
            char speed[96], description[256]; speed_text(&st->models[i], speed, sizeof speed);
            snprintf(description, sizeof description, "%s · %s · %u layers", state_word(&st->models[i]), speed, st->models[i].shape.layers);
            ui_item(10 + (i - top) * 3, i == sel, st->models[i].name, description);
        }
        ui_text(ui_h - 6, 5, UI_MUTED, "A plan before a download. Speed appears only with a matching calibration.");
    }
    if (palette) {
        ui_begin("actions");
        static const char *names[] = {"/models", "/computers", "/refresh", "/request", "/back"};
        static const char *helps[] = {"Browse this model folder", "Choose participating donors", "Refresh inventory and plans",
            "Review the selected model before requesting chat", "Return to the workspace"};
        for (int i = 0; i < 5; i++) ui_item(6 + i * 3, action_sel == i, names[i], helps[i]);
    }
    ui_footer(status, "↑ ↓ move   Enter select / confirm   Tab switch   / actions   Esc back");
    if (ui_w < 60 || ui_h < 28) {
        ui_begin("workspace"); ui_text(5, 4, UI_SAND, "Resize to at least 60 × 28. Esc returns.");
    }
    ui_present();
}

int lmb_tui_run(LmbTuiState *st, int snapshot, const char *keys) {
    (void)setlocale(LC_CTYPE, "");
    g_quit = 0;
    g_snapshot = snapshot; g_color = !snapshot;
    int sel = 0, top = 0, tab = st->initial_tab != 0, detail = 0;
    int palette = 0, action_sel = 0;
    int action = 0;
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
            case 'j': if (sel + 1 < (tab ? (int)st->nnodes : st->nmodels)) sel++; break;
            case 'k': if (sel > 0) sel--; break;
            case '\t': tab = !tab; sel = top = 0; break;
            case 'r':
                if (st->refresh) {
                    (void)st->refresh(st, st->refresh_context);
                    int count = tab ? (int)st->nnodes : st->nmodels;
                    if (!count) sel = top = 0;
                    else if (sel >= count) sel = count - 1;
                }
                break;
            case '\r': case '\n':
                if (!tab && st->nmodels && sz.w >= 60 && sz.h >= 12) detail = 1;
                break;
            default: break;
            }
        }
        int rows = (sz.h - 7) / (tab ? 2 : 1); if (rows < 1) rows = 1;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        if (sz.w < 60 || sz.h < 12) draw_compact(st, sz, tab, sel);
        else if (detail) draw_detail(st, sz, sel);
        else {
            draw_header(st, sz, tab);
            if (tab == 0) draw_models(st, sz, sel, top);
            else draw_nodes(st, sz, sel, top);
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
    signal(SIGTERM, on_int);
    signal(SIGHUP, on_int);

    const char *kp = keys;
    RefreshJob job = {0};
    double refreshed = refresh_clock();
    for (;;) {
        if (g_quit) break;
        if (job.next && atomic_load(&job.done)) {
            pthread_join(job.thread, NULL);
            int selection_changed = memcmp(job.next->selected_nodes, st->selected_nodes,
                                             sizeof st->selected_nodes) != 0;
            memcpy(job.next->selected_nodes, st->selected_nodes, sizeof st->selected_nodes);
            if (selection_changed)
                for (int i = 0; i < job.next->nmodels; i++) job.next->models[i].planned = 0;
            char selected_dir[512] = "";
            if (!tab && sel < st->nmodels)
                snprintf(selected_dir, sizeof selected_dir, "%s", st->models[sel].dir);
            /* Swap complete snapshots on the UI thread. The renderer never
             * races a network update or observes half a plan. */
            memcpy(st, job.next, sizeof *st);
            free(job.next); job.next = NULL;
            if (selected_dir[0]) {
                int found = 0;
                for (int i = 0; i < st->nmodels; i++)
                    if (!strcmp(st->models[i].dir, selected_dir)) {
                        sel = i; found = 1; break;
                    }
                if (!found) detail = 0;
            }
            int count = tab ? (int)st->nnodes : st->nmodels;
            if (!count) { sel = top = detail = 0; }
            else if (sel >= count) { sel = count - 1; detail = 0; }
            if (top > sel) top = sel;
            refreshed = selection_changed ? 0 : refresh_clock();
        }
        if (st->tracker[0] && refresh_clock() - refreshed >= 5.0)
            refresh_start(&job, st);
        if (g_resized) { g_resized = 0; sz = term_size(); clear_screen(); }
        draw_workspace(st, tab, sel, detail, palette, action_sel);
        int k;
        if (kp) { k = *kp ? (unsigned char)*kp++ : 'q'; }
        else k = read_key(250);
        if (!k) continue;
        if (k == 'q' || k == 3) break;
        if (k == '/') { palette = !palette; action_sel = 0; continue; }
        if (palette) {
            if (k == 27) { palette = 0; continue; }
            if (k == 'j') action_sel = (action_sel + 1) % 5;
            if (k == 'k') action_sel = (action_sel + 4) % 5;
            if (k != '\r' && k != '\n') continue;
            palette = 0;
            if (action_sel == 4) break;
            if (action_sel < 2) { tab = action_sel; sel = top = detail = 0; continue; }
            if (action_sel == 2) { refresh_start(&job, st); continue; }
            if (!tab && st->nmodels) detail = 1;
            continue;
        }
        if (k == 27 && !detail) break;
        if (ui_w < 60 || ui_h < 28) continue;
        if ((k == 'c' || (detail && (k == '\r' || k == '\n'))) && !tab && st->nmodels && st->tracker[0]) {
            st->action_model = sel;
            action = LMB_TUI_REQUEST_CHAT;
            break;
        }
        if (detail) { if (k == 0x1b) detail = 0; continue; }
        if (tab && (k == '\r' || k == '\n')) k = ' ';
        switch (k) {
        case ' ':
            if (tab && sel < (int)st->nnodes && st->identities[sel][0] && st->nodes[sel].addr[0]) {
                int removed = 0;
                for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++)
                    if (!strcmp(st->selected_nodes[i], st->identities[sel])) {
                        st->selected_nodes[i][0] = 0; removed = 1; break;
                    }
                if (!removed)
                    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++)
                        if (!st->selected_nodes[i][0]) {
                            memmove(st->selected_nodes[i], st->identities[sel], sizeof st->selected_nodes[i]); break;
                        }
                refresh_start(&job, st);
            }
            break;
        case 'j': if (sel + 1 < (tab ? (int)st->nnodes : st->nmodels)) sel++; break;
        case 'k': if (sel > 0) sel--; break;
        case '\t': tab = !tab; sel = top = 0; break;
        case 'r':
            refresh_start(&job, st);
            break;
        case '\r': case '\n':
            if (!tab && st->nmodels && sz.w >= 60 && sz.h >= 12) detail = 1;
            break;
        default: break;
        }
        int rows = (sz.h - 7) / (tab ? 2 : 1); if (rows < 1) rows = 1;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
    cooked();
    if (job.next) { pthread_join(job.thread, NULL); free(job.next); }
    return action;
}
