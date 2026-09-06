/* Standalone design study. No Lumabri runtime, sockets, models or settings.
 * The canvas drives both the terminal and SVG previews. The wordmark below
 * is the existing Lumabri ANSI-Shadow logo, not a replacement brand asset. */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

enum { BG, TEXT, MUTED, ACCENT, LINE, SELECTED, GREEN, SAND, CORAL, GOLD, PEACH, NCOLORS };
static const unsigned colors[NCOLORS] = {
    0x191a1b, 0xece6dd, 0xa5a199, 0xff875f, 0x494744,
    0x352823, 0x9dbd9a, 0xffd7af, 0xff5f5f, 0xffaf5f, 0xffaf87
};
typedef struct { wchar_t ch; unsigned char fg, bg; } Cell;
static Cell canvas[70][180];
static int width = 104, height = 38;
enum View { HOME, CHAT, MODELS, COMPUTERS, SHARE };
static enum View view = HOME;
static int choice, command_choice, palette, chosen[3], active_model;
static char input[256], notice[160];
static char prompts[8][256];
static int nprompts;
static volatile sig_atomic_t stopped, resized;
static struct termios saved;
static int raw_active;

static const char *wordmark[] = {
    "██╗     ██╗   ██╗███╗   ███╗ █████╗ ██████╗ ██████╗ ██╗",
    "██║     ██║   ██║████╗ ████║██╔══██╗██╔══██╗██╔══██╗██║",
    "██║     ██║   ██║██╔████╔██║███████║██████╔╝██████╔╝██║",
    "██║     ██║   ██║██║╚██╔╝██║██╔══██║██╔══██╗██╔══██╗██║",
    "███████╗╚██████╔╝██║ ╚═╝ ██║██║  ██║██████╔╝██║  ██║██║",
    "╚══════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝"
};
static const char *actions[] = {
    "Start a conversation", "Explore models", "Your computers", "Share resources"
};
static const char *descriptions[] = {
    "A quiet space to think, ask and build.",
    "Find a model your computers can run together.",
    "See resources. Choose who participates.",
    "Review a request before anything is loaded."
};
static const char *commands[] = { "/home", "/models", "/computers", "/share" };
static const char *command_help[] = {
    "Back to your workspace", "Choose a model", "Choose participating computers", "Review donor requests"
};
static const char *model_names[] = { "OLMoE", "DeepSeek V4 Flash", "GLM", "Kimi", "Qwen" };
static const char *computer_names[] = { "MacBook Pro", "Living-room PC", "GPU workstation" };
static const char *computer_specs[] = {
    "Intel CPU     16 GB RAM", "CPU           32 GB RAM", "GPU           32 GB RAM + 12 GB VRAM"
};

static void put(int y, int x, int fg, const char *s) {
    mbstate_t state = {0};
    while (*s && y >= 0 && y < height && x < width - 2) {
        wchar_t ch;
        size_t n = mbrtowc(&ch, s, strlen(s), &state);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) break;
        int cells = wcwidth(ch);
        if (cells > 0 && x >= 0 && x + cells <= width - 2) {
            canvas[y][x].ch = ch; canvas[y][x].fg = (unsigned char)fg;
            for (int i = 1; i < cells; i++) canvas[y][x + i].ch = 0;
        }
        if (cells > 0) x += cells;
        s += n;
    }
}

static void rule(int y, int fg) {
    for (int x = 4; x < width - 4; x++) put(y, x, fg, "─");
}

static void highlight(int y, int rows) {
    for (int j = y; j < y + rows && j < height; j++)
        for (int x = 3; x < width - 3; x++) canvas[j][x].bg = SELECTED;
}

static void wrapped(int *y, int x, const char *text, int fg, int end) {
    /* Copy for this study is ASCII; UTF-8 input is shown by put() unmodified. */
    int room = width - x - 6;
    while (*text && *y < end) {
        size_t n = strlen(text);
        if (n > (size_t)room) {
            n = (size_t)room;
            while (n && text[n] != ' ') n--;
            if (!n) n = (size_t)room;
        }
        char part[256];
        snprintf(part, sizeof part, "%.*s", (int)n, text);
        put((*y)++, x, fg, part);
        text += n;
        while (*text == ' ') text++;
    }
}

static void header(const char *name) {
    put(1, 4, ACCENT, "✻ lumabri");
    put(1, 17, MUTED, name);
    put(1, width - 23, SAND, "DESIGN PREVIEW");
    rule(3, LINE);
}

static void footer(void) {
    rule(height - 4, LINE);
    put(height - 3, 4, MUTED, notice[0] ? notice : "Sample data only. No network, downloads or inference.");
    put(height - 2, 4, MUTED, view == CHAT ? "Enter send sample   / actions   Esc home   Ctrl-C exit" :
        "↑ ↓ move   Enter select   / actions   Esc back");
}

static void home_screen(void) {
    header("your workspace");
    int top = height >= 34 ? 5 : 4;
    if (height >= 32 && width >= 64) {
        static const int gradient[] = { CORAL, ACCENT, ACCENT, GOLD, PEACH, SAND };
        for (int i = 0; i < 6; i++) put(top + i, 5, gradient[i], wordmark[i]);
        put(top + 7, 5, MUTED, "tiny engine, immense swarm");
        if (width >= 100) {
            put(top + 1, 67, TEXT, "Your computers.");
            put(top + 2, 67, TEXT, "One shared possibility.");
            put(top + 4, 67, MUTED, "A private cluster, at home.");
            put(top + 5, 67, GREEN, "3 computers in this sample");
        }
        top += 10;
    } else {
        put(top, 5, SAND, "Your computers. One shared possibility.");
        top += 3;
    }
    put(top, 5, TEXT, "What would you like to do?");
    for (int i = 0; i < 4; i++) {
        int y = top + 2 + i * 3;
        if (i == choice) { highlight(y, 2); put(y, 4, ACCENT, "›"); }
        put(y, 7, i == choice ? SAND : TEXT, actions[i]);
        put(y + 1, 7, MUTED, descriptions[i]);
    }
}

static void chat_screen(void) {
    header("new conversation");
    put(5, 5, TEXT, model_names[active_model]);
    put(6, 5, MUTED, "sample model  ·  no active session");
    put(8, 5, MUTED, "You");
    put(9, 5, TEXT, "What could I run with the computers I already have?");
    put(12, 5, ACCENT, "✻ Lumabri");
    int y = 14;
    wrapped(&y, 5, "Start with your computers, then choose a model. You stay in control of who joins the plan.", TEXT, height - 12);
    y++;
    wrapped(&y, 5, "The real catalogue will show available memory and supported plans. Speed stays unestimated until we have enough evidence.", MUTED, height - 12);
    /* A finite, in-memory demo transcript: no generated model response. */
    for (int i = nprompts > 2 ? nprompts - 2 : 0; i < nprompts && y < height - 10; i++) {
        y++;
        put(y++, 5, MUTED, "You");
        wrapped(&y, 5, prompts[i], TEXT, height - 10);
        wrapped(&y, 5, "Preview only: your message was not sent to a model.", SAND, height - 10);
    }
    rule(height - 8, ACCENT);
    put(height - 6, 4, ACCENT, "›");
    put(height - 6, 7, input[0] ? TEXT : MUTED, input[0] ? input : "Ask something, or / for actions");
    rule(height - 5, ACCENT);
}

static void models_screen(void) {
    header("models");
    put(5, 5, TEXT, "Choose what you want to run.");
    put(7, 5, MUTED, "Illustrative catalogue, not a compatibility or capacity report.");
    for (int i = 0; i < 5; i++) {
        int y = 10 + i * 2;
        if (choice == i) { highlight(y, 2); put(y, 4, ACCENT, "›"); }
        put(y, 7, choice == i ? SAND : TEXT, model_names[i]);
        put(y + 1, 7, MUTED, "Plan not checked");
        put(y, width - 25, MUTED, "Speed not estimated");
    }
    if (height > 31) {
        rule(23, LINE);
        put(25, 5, TEXT, "A plan before a download.");
        put(27, 5, MUTED, "Memory needs, selected computers and missing resources belong here.");
        put(28, 5, MUTED, "Enter opens the sample conversation. It does not load this model.");
    }
}

static void computers_screen(void) {
    header("computers");
    put(5, 5, TEXT, "Your cluster. Your choice.");
    put(7, 5, MUTED, "Example machines. Nothing is selected automatically.");
    for (int i = 0; i < 3; i++) {
        int y = 10 + i * 3;
        if (choice == i) highlight(y, 2);
        put(y, 5, i == choice ? ACCENT : MUTED, i == choice ? "›" : " ");
        put(y, 8, chosen[i] ? GREEN : MUTED, chosen[i] ? "[✓]" : "[ ]");
        put(y, 13, TEXT, computer_names[i]);
        put(y + 1, 13, MUTED, computer_specs[i]);
    }
    put(21, 5, SAND, "Enter toggles a sample selection; it sends no request.");
    if (height > 31) {
        rule(24, LINE);
        put(26, 5, TEXT, "Being visible is not permission to load a model.");
        put(28, 5, MUTED, "Each donor must approve the real allocation before it starts.");
    }
}

static void share_screen(void) {
    header("share resources");
    put(5, 5, TEXT, "Nothing loads without your approval.");
    put(7, 5, MUTED, "Sample request from Living-room PC");
    rule(9, LINE);
    put(11, 5, MUTED, "Model"); put(11, 24, TEXT, "OLMoE (example only)");
    put(13, 5, MUTED, "Allocation"); put(13, 24, TEXT, "A layer range, including its state");
    put(15, 5, MUTED, "RAM limit"); put(15, 24, SAND, "4 GB (illustrative, not reserved)");
    put(17, 5, MUTED, "Privacy"); put(17, 24, TEXT, "This node processes model activations");
    for (int i = 0; i < 2; i++) {
        int y = 20 + i * 2;
        if (choice == i) { highlight(y, 1); put(y, 4, ACCENT, "›"); }
        put(y, 7, choice == i ? SAND : TEXT, i ? "Decline sample request" : "Approve sample request");
    }
}

static void draw(void) {
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) canvas[y][x] = (Cell){ L' ', TEXT, BG };
    switch (view) {
    case HOME: home_screen(); break;
    case CHAT: chat_screen(); break;
    case MODELS: models_screen(); break;
    case COMPUTERS: computers_screen(); break;
    case SHARE: share_screen(); break;
    }
    footer();
    if (palette) {
        int y = height - 15;
        for (int j = y; j < height - 5; j++)
            for (int x = 3; x < width - 3; x++) canvas[j][x] = (Cell){ L' ', TEXT, BG };
        put(y, 5, SAND, "Actions");
        for (int i = 0; i < 4; i++) {
            int row = y + 2 + i * 2;
            if (i == command_choice) { highlight(row, 1); put(row, 4, ACCENT, "›"); }
            put(row, 7, TEXT, commands[i]);
            put(row, 22, MUTED, command_help[i]);
        }
    }
}

static void glyph(FILE *f, wchar_t ch, int xml) {
    if (!ch) return;
    if (xml && ch == L'&') fputs("&amp;", f);
    else if (xml && ch == L'<') fputs("&lt;", f);
    else if (xml && ch == L'>') fputs("&gt;", f);
    else {
        char bytes[MB_LEN_MAX]; mbstate_t s = {0};
        size_t n = wcrtomb(bytes, ch, &s);
        if (n != (size_t)-1) fwrite(bytes, 1, n, f);
    }
}

static void output(int svg, int plain) {
    if (svg) {
        printf("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\"><title>Lumabri design preview — sample data only</title><rect width=\"100%%\" height=\"100%%\" fill=\"#%06x\"/><g font-family=\"DejaVu Sans Mono,monospace\" font-size=\"15\">\n", width * 10 + 24, height * 21 + 24, width * 10 + 24, height * 21 + 24, colors[BG]);
    } else if (!plain) fputs("\x1b[H", stdout);
    int fg = -1, bg = -1;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            Cell c = canvas[y][x];
            if (svg) {
                if (c.bg != BG) printf("<rect x=\"%d\" y=\"%d\" width=\"10\" height=\"21\" fill=\"#%06x\"/>", 12 + x * 10, 12 + y * 21, colors[c.bg]);
                if (c.ch && c.ch != L' ') {
                    printf("<text x=\"%d\" y=\"%d\" fill=\"#%06x\">", 12 + x * 10, 28 + y * 21, colors[c.fg]);
                    glyph(stdout, c.ch, 1); fputs("</text>", stdout);
                }
            } else {
                if (!plain && (fg != c.fg || bg != c.bg)) {
                    unsigned f = colors[c.fg], b = colors[c.bg];
                    printf("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um", f >> 16, (f >> 8) & 255, f & 255, b >> 16, (b >> 8) & 255, b & 255);
                    fg = c.fg; bg = c.bg;
                }
                glyph(stdout, c.ch, 0);
            }
        }
        if (plain) putchar('\n');
        else if (!svg && y < height - 1) printf("\x1b[%d;1H", y + 2);
    }
    if (svg) puts("</g></svg>");
    fflush(stdout);
}

static void restore(void) {
    if (raw_active) {
        tcsetattr(0, TCSANOW, &saved);
        fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout); fflush(stdout);
        raw_active = 0;
    }
}
static void stop_signal(int sig) { (void)sig; stopped = 1; }
static void resize_signal(int sig) { (void)sig; resized = 1; }
static int limits(int value, int low, int high) { return value < low ? low : value > high ? high : value; }
static void size_terminal(void) {
    struct winsize ws;
    if (!ioctl(1, TIOCGWINSZ, &ws)) {
        width = limits(ws.ws_col, 1, 180); height = limits(ws.ws_row, 1, 70);
    }
}
static void go(enum View next) { view = next; choice = 0; palette = 0; input[0] = 0; notice[0] = 0; }
static int key(void) {
    unsigned char c;
    if (read(0, &c, 1) != 1) { stopped = 1; return 0; }
    if (c != 27) return c;
    struct pollfd p = {0, POLLIN, 0};
    if (poll(&p, 1, 60) <= 0) return 27;
    unsigned char seq[2];
    if (read(0, seq, 1) != 1 || (seq[0] != '[' && seq[0] != 'O')) return 27;
    if (poll(&p, 1, 60) <= 0 || read(0, seq + 1, 1) != 1) return 27;
    if (seq[1] == 'A') return 1001;
    if (seq[1] == 'B') return 1002;
    return 0;
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    int svg = 0, plain = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--svg")) svg = 1;
        else if (!strcmp(argv[i], "--snapshot")) plain = 1;
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = limits(atoi(argv[++i]), 60, 180);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = limits(atoi(argv[++i]), 28, 70);
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "home")) view = HOME;
            else if (!strcmp(v, "chat")) view = CHAT;
            else if (!strcmp(v, "models")) view = MODELS;
            else if (!strcmp(v, "computers")) view = COMPUTERS;
            else if (!strcmp(v, "share")) view = SHARE;
            else { fprintf(stderr, "Unknown preview view: %s\n", v); return 2; }
        } else {
            fprintf(stderr, "Usage: tui-preview [--view home|chat|models|computers|share] [--svg|--snapshot] [--width N] [--height N]\n");
            return 2;
        }
    }
    if (svg || plain) { draw(); output(svg, plain); return 0; }
    if (!isatty(0) || !isatty(1)) { fprintf(stderr, "Open a terminal, or use --snapshot.\n"); return 1; }
    if (tcgetattr(0, &saved)) return 1;
    struct termios raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); raw.c_cc[VMIN] = 1;
    if (tcsetattr(0, TCSANOW, &raw)) return 1;
    raw_active = 1; atexit(restore);
    signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal); signal(SIGWINCH, resize_signal);
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
    int dirty = 1;
    while (!stopped) {
        if (dirty || resized) {
            size_terminal(); resized = 0;
            if (width < 60 || height < 28) {
                fputs("\x1b[H\x1b[2J\x1b[0mLumabri preview: resize to at least 60 x 28. Esc to exit.", stdout); fflush(stdout);
            } else { draw(); output(0, 0); }
            dirty = 0;
        }
        struct pollfd p = {0, POLLIN, 0};
        int ready = poll(&p, 1, 100);
        if (ready <= 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(p.revents & POLLIN)) continue;
        int k = key(); dirty = 1;
        if (width < 60 || height < 28) { if (k == 27) break; continue; }
        if (k == 27) {
            if (palette) { palette = 0; input[0] = 0; }
            else if (view != HOME) go(HOME);
            else break;
        } else if (k == '/') { palette = 1; command_choice = 0; }
        else if (k == 1001 || k == 1002) {
            int count = palette ? 4 : view == HOME ? 4 : view == MODELS ? 5 : view == COMPUTERS ? 3 : view == SHARE ? 2 : 1;
            int *at = palette ? &command_choice : &choice;
            *at = (*at + (k == 1001 ? count - 1 : 1)) % count;
        } else if ((k == 9 || k == '\r' || k == '\n') && palette) {
            static const enum View targets[] = {HOME, MODELS, COMPUTERS, SHARE};
            go(targets[command_choice]);
        } else if (k == '\r' || k == '\n') {
            if (view == HOME) {
                static const enum View targets[] = {CHAT, MODELS, COMPUTERS, SHARE};
                go(targets[choice]);
            } else if (view == COMPUTERS) chosen[choice] = !chosen[choice];
            else if (view == MODELS) { active_model = choice; go(CHAT); snprintf(notice, sizeof notice, "Sample conversation only. Model selection is not connected."); }
            else if (view == SHARE) snprintf(notice, sizeof notice, "Sample request %s. No resources were reserved.", choice ? "declined" : "approved");
            else if (view == CHAT && input[0]) {
                if (nprompts == 8) { memmove(prompts, prompts + 1, sizeof prompts - sizeof prompts[0]); nprompts--; }
                snprintf(prompts[nprompts++], sizeof prompts[0], "%s", input); input[0] = 0;
            }
        } else if (view == CHAT && !palette) {
            size_t n = strlen(input);
            if (k == 127 || k == 8) {
                if (n) { do { n--; } while (n && ((unsigned char)input[n] & 0xc0) == 0x80); input[n] = 0; }
            } else if (k >= 32 && k <= 255 && n + 1 < sizeof input) { input[n] = (char)k; input[n + 1] = 0; }
        }
    }
    restore(); return 0;
}
