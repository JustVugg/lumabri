/* Small shared terminal canvas for the household screens. No network/state
 * ownership: callers render an immutable snapshot and handle actions. */
#ifndef LUMABRI_VISUAL_H
#define LUMABRI_VISUAL_H
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#ifdef _WIN32
#include "lumabri_console_windows.h"
#else
#include <sys/ioctl.h>
#endif

enum { UI_BG, UI_TEXT, UI_MUTED, UI_ACCENT, UI_LINE, UI_SELECTED, UI_GREEN, UI_SAND };
static const unsigned ui_colors[] = {
    0x191a1b, 0xece6dd, 0xa5a199, 0xff875f, 0x494744, 0x352823, 0x9dbd9a, 0xffd7af
};
typedef struct { uint32_t ch; unsigned char fg, bg; } UiCell;
static UiCell ui_canvas[100][200];
static int ui_w, ui_h;

/* Apple Terminal can report xterm-256color but does not implement RGB SGR.
 * Sending 48;2;r;g;b there can select unrelated ANSI backgrounds. */
static inline int ui_color_mode(void) {
    const char *override = getenv("LUMABRI_COLOR");
    if (getenv("NO_COLOR") && *getenv("NO_COLOR")) return 0;
    if (override) {
        if (!strcmp(override, "none")) return 0;
        if (!strcmp(override, "16")) return 16;
        if (!strcmp(override, "256")) return 256;
        if (!strcmp(override, "truecolor")) return 24;
    }
    const char *program = getenv("TERM_PROGRAM"), *color = getenv("COLORTERM");
    if (program && !strcmp(program, "Apple_Terminal")) return 256;
    if (color && (!strcmp(color, "truecolor") || !strcmp(color, "24bit"))) return 24;
    const char *term = getenv("TERM");
    if (term && strstr(term, "256color")) return 256;
    return 16;
}

static inline void ui_sgr(int mode, int fg, int bg) {
    static const unsigned palette256[] = {234, 255, 248, 209, 239, 238, 108, 223};
    static const unsigned palette16[] = {30, 37, 90, 33, 90, 30, 32, 93};
    if (mode == 24) {
        unsigned f = ui_colors[fg], b = ui_colors[bg];
        printf("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
               f >> 16, (f >> 8) & 255, f & 255, b >> 16, (b >> 8) & 255, b & 255);
    } else if (mode == 256) {
        printf("\x1b[38;5;%um\x1b[48;5;%um", palette256[fg], palette256[bg]);
    } else if (mode == 16) {
        printf("\x1b[%u;%um", palette16[fg], bg == UI_SELECTED ? 100u : 40u);
    } else {
        fputs("\x1b[0m", stdout);
    }
}

static inline void ui_text(int y, int x, int fg, const char *s) {
#ifndef _WIN32
    mbstate_t state = {0};
#endif
    size_t remaining = strlen(s);
    while (remaining && y >= 0 && y < ui_h && x < ui_w - 2) {
#ifdef _WIN32
        const unsigned char *b = (const unsigned char *)s;
        size_t n = b[0] < 0x80 ? 1 : b[0] >= 0xc2 && b[0] <= 0xdf ? 2 :
            b[0] >= 0xe0 && b[0] <= 0xef ? 3 : b[0] >= 0xf0 && b[0] <= 0xf4 ? 4 : 0;
        if (!n || n > remaining) break;
        uint32_t ch = b[0] & (n == 1 ? 0x7f : n == 2 ? 0x1f : n == 3 ? 0xf : 7);
        for (size_t i = 1; i < n; i++) {
            if ((b[i] & 0xc0) != 0x80) return;
            ch = (ch << 6) | (b[i] & 0x3f);
        }
        if ((n == 2 && ch < 0x80) || (n == 3 && ch < 0x800) ||
            (n == 4 && ch < 0x10000) || ch > 0x10ffff ||
            (ch >= 0xd800 && ch <= 0xdfff)) break;
        int cells = lmb_console_width(ch);
#else
        wchar_t ch;
        size_t n = mbrtowc(&ch, s, remaining, &state);
        if (n == (size_t)-1 || n == (size_t)-2 || !n) break;
        int cells = wcwidth(ch);
#endif
        /* No terminal control bytes from checkpoint names or peer adverts. */
        if (cells > 0 && x >= 0 && x + cells <= ui_w - 2) {
            ui_canvas[y][x].ch = ch; ui_canvas[y][x].fg = (unsigned char)fg;
            for (int i = 1; i < cells; i++) ui_canvas[y][x + i].ch = 0;
        }
        if (cells > 0) x += cells;
        s += n; remaining -= n;
    }
}

static inline void ui_printf(int y, int x, int fg, const char *fmt, ...) {
    char line[2048]; va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    ui_text(y, x, fg, line);
}

static inline void ui_rule(int y, int fg) {
    for (int x = 4; x < ui_w - 4; x++) ui_text(y, x, fg, "─");
}

static inline void ui_highlight(int y, int rows) {
    for (int j = y; j >= 0 && j < y + rows && j < ui_h - 4; j++)
        for (int x = 3; x < ui_w - 3; x++) ui_canvas[j][x].bg = UI_SELECTED;
}

static inline void ui_begin(const char *title) {
#ifdef _WIN32
    ui_w = 104; ui_h = 38;
    lmb_console_size(&ui_w, &ui_h);
#else
    struct winsize ws = {0};
    (void)ioctl(1, TIOCGWINSZ, &ws);
    ui_w = ws.ws_col ? ws.ws_col : 104; ui_h = ws.ws_row ? ws.ws_row : 38;
#endif
    if (ui_w > 200) ui_w = 200;
    if (ui_h > 100) ui_h = 100;
    for (int y = 0; y < ui_h; y++) for (int x = 0; x < ui_w; x++)
        ui_canvas[y][x] = (UiCell){L' ', UI_TEXT, UI_BG};
    ui_text(1, 4, UI_ACCENT, "✻ lumabri");
    ui_text(1, 17, UI_MUTED, title); ui_rule(3, UI_LINE);
}

static inline void ui_footer(const char *status, const char *keys) {
    ui_rule(ui_h - 4, UI_LINE);
    ui_text(ui_h - 3, 4, UI_MUTED, status);
    ui_text(ui_h - 2, 4, UI_MUTED, keys);
}

static inline void ui_present(void) {
    int fg = -1, bg = -1;
    int mode = ui_color_mode();
    fputs("\x1b[H", stdout);
    for (int y = 0; y < ui_h; y++) {
        printf("\x1b[%d;1H", y + 1);
        /* Do not write the lower-right cell: several terminals scroll. */
        for (int x = 0; x < ui_w - 1; x++) {
            UiCell cell = ui_canvas[y][x];
            if (!cell.ch) continue;
            if (fg != cell.fg || bg != cell.bg) {
                fg = cell.fg; bg = cell.bg;
                ui_sgr(mode, fg, bg);
            }
#ifdef _WIN32
            uint32_t ch = cell.ch;
            unsigned char utf8[4]; size_t n;
            if (ch < 0x80) { utf8[0] = (unsigned char)ch; n = 1; }
            else if (ch < 0x800) {
                utf8[0] = 0xc0 | (ch >> 6); utf8[1] = 0x80 | (ch & 63); n = 2;
            } else if (ch < 0x10000) {
                utf8[0] = 0xe0 | (ch >> 12); utf8[1] = 0x80 | ((ch >> 6) & 63);
                utf8[2] = 0x80 | (ch & 63); n = 3;
            } else {
                utf8[0] = 0xf0 | (ch >> 18); utf8[1] = 0x80 | ((ch >> 12) & 63);
                utf8[2] = 0x80 | ((ch >> 6) & 63); utf8[3] = 0x80 | (ch & 63); n = 4;
            }
            fwrite(utf8, 1, n, stdout);
#else
            printf("%lc", (wint_t)cell.ch);
#endif
        }
    }
    fputs("\x1b[0m", stdout); fflush(stdout);
}

static inline void ui_item(int y, int selected, const char *title, const char *description) {
    if (selected) { ui_highlight(y, 2); ui_text(y, 4, UI_ACCENT, "›"); }
    ui_text(y, 7, selected ? UI_SAND : UI_TEXT, title);
    ui_text(y + 1, 7, UI_MUTED, description);
}
#endif
