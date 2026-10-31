/* Native console boundary for the shared C screens. This does not own any
 * model, network connection or keeper. Closing a screen restores the shell. */
#ifndef LMB_CONSOLE_WINDOWS_H
#define LMB_CONSOLE_WINDOWS_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>

typedef struct {
    HANDLE input, output;
    DWORD input_mode, output_mode;
    UINT input_cp, output_cp;
    int active;
    WORD repeated;
    int repeated_key;
    int rebound, saved_fd[2];
    HANDLE saved_handle[2];
    int std_was_crt[2];
} LmbConsole;

static inline void lmb_console_restore_stdio(LmbConsole *c) {
    if (!c->rebound) return;
    fflush(stdout);
    for (int i=0;i<2;i++) {
        if (c->saved_fd[i]>=0) { _dup2(c->saved_fd[i],i);_close(c->saved_fd[i]); }
        SetStdHandle(i ? STD_OUTPUT_HANDLE : STD_INPUT_HANDLE,
            c->std_was_crt[i] ? (HANDLE)_get_osfhandle(i) : c->saved_handle[i]);
    }
    c->rebound=0;
}

/* A process created inside ConPTY can inherit the launcher's redirected CRT
 * descriptors even though it has its own console. An explicitly opened TUI
 * binds that console and restores the original descriptors when detached. */
static inline int lmb_console_bind_stdio(LmbConsole *c) {
    HANDLE input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    HANDLE output=CreateFileW(L"CONOUT$",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if (input==INVALID_HANDLE_VALUE || output==INVALID_HANDLE_VALUE) {
        if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output!=INVALID_HANDLE_VALUE) CloseHandle(output);
        return -1;
    }
    int fds[2]={_open_osfhandle((intptr_t)input,_O_RDONLY|_O_BINARY),
                _open_osfhandle((intptr_t)output,_O_WRONLY|_O_BINARY)};
    if (fds[0]<0 || fds[1]<0) {
        if (fds[0]>=0) _close(fds[0]);else CloseHandle(input);
        if (fds[1]>=0) _close(fds[1]);else CloseHandle(output);
        return -1;
    }
    c->saved_fd[0]=_dup(0);c->saved_fd[1]=_dup(1);
    c->saved_handle[0]=GetStdHandle(STD_INPUT_HANDLE);c->saved_handle[1]=GetStdHandle(STD_OUTPUT_HANDLE);
    for (int i=0;i<2;i++) c->std_was_crt[i]=c->saved_handle[i]==(HANDLE)_get_osfhandle(i);
    int bad=c->saved_fd[0]<0 || c->saved_fd[1]<0;
    c->rebound=1;
    if (!bad) bad=_dup2(fds[0],0) || _dup2(fds[1],1) ||
        !SetStdHandle(STD_INPUT_HANDLE,(HANDLE)_get_osfhandle(0)) ||
        !SetStdHandle(STD_OUTPUT_HANDLE,(HANDLE)_get_osfhandle(1));
    _close(fds[0]);_close(fds[1]);
    if (bad) {lmb_console_restore_stdio(c);return -1;}
    return 0;
}

static inline int lmb_console_open(LmbConsole *c) {
    if (c->active) return -1;
    c->input = GetStdHandle(STD_INPUT_HANDLE);
    c->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(c->input, &c->input_mode) ||
        !GetConsoleMode(c->output, &c->output_mode)) {
        if (lmb_console_bind_stdio(c)) return -1;
        c->input=GetStdHandle(STD_INPUT_HANDLE);c->output=GetStdHandle(STD_OUTPUT_HANDLE);
        if (!GetConsoleMode(c->input,&c->input_mode) || !GetConsoleMode(c->output,&c->output_mode)) {
            lmb_console_restore_stdio(c);return -1;
        }
    }
    c->input_cp = GetConsoleCP(); c->output_cp = GetConsoleOutputCP();
    /* Quick Edit freezes the entire process during selection. ReadConsoleInputW
     * supplies keys and resize events without a locale-dependent byte stream. */
    DWORD input = (c->input_mode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
        ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE |
          ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    if (!SetConsoleMode(c->input, input)) {lmb_console_restore_stdio(c);return -1;}
    if (!SetConsoleMode(c->output, c->output_mode | ENABLE_PROCESSED_OUTPUT |
                        ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
        !SetConsoleCP(CP_UTF8) || !SetConsoleOutputCP(CP_UTF8)) {
        SetConsoleMode(c->input, c->input_mode);
        SetConsoleMode(c->output, c->output_mode);
        SetConsoleCP(c->input_cp); SetConsoleOutputCP(c->output_cp);
        lmb_console_restore_stdio(c);
        return -1;
    }
    c->repeated = 0; c->active = 1;
    return 0;
}

static inline void lmb_console_close(LmbConsole *c) {
    if (!c->active) return;
    fflush(stdout);
    SetConsoleMode(c->input, c->input_mode);
    SetConsoleMode(c->output, c->output_mode);
    SetConsoleCP(c->input_cp); SetConsoleOutputCP(c->output_cp);
    c->active = 0; c->repeated = 0;
    lmb_console_restore_stdio(c);
}

static inline void lmb_console_size(int *width, int *height) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        int h = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (w > 0) *width = w;
        if (h > 0) *height = h;
    }
}

/* The catalogue's shortcuts are ASCII. Unicode model names are rendered as
 * UTF-8; future text editors must consume Unicode input separately rather
 * than truncate UTF-16 characters into shortcut bytes. */
static inline int lmb_console_key(LmbConsole *c, int timeout_ms, int *resized) {
    if (c->repeated) { c->repeated--; return c->repeated_key; }
    ULONGLONG end = GetTickCount64() + (unsigned)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD wait = now < end ? (DWORD)(end-now) : 0;
        DWORD ready = WaitForSingleObject(c->input, wait);
        if (ready != WAIT_OBJECT_0) return ready == WAIT_TIMEOUT ? 0 : 3;
        INPUT_RECORD event; DWORD count = 0;
        if (!ReadConsoleInputW(c->input, &event, 1, &count) || count != 1) return 3;
        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) *resized = 1;
        if (event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown) {
            KEY_EVENT_RECORD *key = &event.Event.KeyEvent;
            int value = 0;
            if (key->wVirtualKeyCode == VK_UP) value = 'k';
            else if (key->wVirtualKeyCode == VK_DOWN) value = 'j';
            else if (key->uChar.UnicodeChar > 0 && key->uChar.UnicodeChar < 128)
                value = key->uChar.UnicodeChar;
            if (value) {
                c->repeated_key = value;
                c->repeated = key->wRepeatCount ? key->wRepeatCount-1 : 0;
                return value;
            }
        }
        if (GetTickCount64() >= end) return 0;
    }
}

/* Windows wchar_t is UTF-16; mbrtowc/%lc depend on the CRT locale and cannot
 * represent supplementary code points. Keep the canvas in Unicode scalars
 * and emit UTF-8, which both conhost VT and Windows Terminal understand. */
static inline int lmb_console_width(uint32_t ch) {
    if (!ch || ch < 32 || (ch >= 0x7f && ch < 0xa0)) return -1;
    if ((ch >= 0x300 && ch <= 0x36f) || (ch >= 0xfe00 && ch <= 0xfe0f)) return 0;
    if (ch <= 0xffff) {
        WCHAR w = (WCHAR)ch; WORD type;
        if (GetStringTypeW(CT_CTYPE3, &w, 1, &type)) {
            if (type & (C3_FULLWIDTH | C3_IDEOGRAPH)) return 2;
            /* NLS also marks precomposed letters such as è NONSPACING.
             * Those letters still occupy one terminal cell. */
            if ((type & C3_NONSPACING) && !(type & C3_ALPHA)) return 0;
        }
    }
    if ((ch >= 0x1f300 && ch <= 0x1faff) || (ch >= 0x20000 && ch <= 0x3fffd)) return 2;
    return 1;
}
#endif
