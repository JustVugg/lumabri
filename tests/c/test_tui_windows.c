/* Real Windows pseudoconsole: exercise the shipped renderer and input loop,
 * then inspect restored console modes. No terminal or daemon is borrowed. */
#define WIN32_LEAN_AND_MEAN
#define NTDDI_VERSION 0x0A000006 /* ConPTY acceptance harness: Windows 10 1809+ */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "src/ui/lumabri_tui.h"
#include "src/ui/lumabri_console_windows.h"

typedef struct {
    HANDLE pipe;
    CRITICAL_SECTION mutex;
    char text[2*1024*1024];
    size_t used;
} Capture;
static DWORD WINAPI capture(void *arg) {
    Capture *c = arg; char bytes[8192]; DWORD n;
    while (ReadFile(c->pipe, bytes, sizeof bytes, &n, NULL) && n) {
        EnterCriticalSection(&c->mutex);
        size_t take = n < sizeof c->text-c->used-1 ? n : sizeof c->text-c->used-1;
        memcpy(c->text+c->used, bytes, take); c->used += take; c->text[c->used] = 0;
        LeaveCriticalSection(&c->mutex);
    }
    return 0;
}
static int contains(Capture *c, const char *text) {
    EnterCriticalSection(&c->mutex);
    int found = strstr(c->text, text) != NULL;
    LeaveCriticalSection(&c->mutex); return found;
}
static void expect(Capture *c, const char *text) {
    ULONGLONG until = GetTickCount64()+15000;
    while (!contains(c, text) && GetTickCount64() < until) Sleep(10);
    if (!contains(c, text)) {
        fprintf(stderr, "Missing console output: %s\n%s\n", text, c->text); abort();
    }
}
static void send_keys(HANDLE input, const char *keys) {
    DWORD written; size_t n = strlen(keys);
    assert(WriteFile(input, keys, (DWORD)n, &written, NULL) && written == n);
}

static int child(int scenario) {
    /* A parent launched by PowerShell through redirected WSL pipes has CRT
     * standard handles too. Bind this test child to its own ConPTY explicitly. */
    HANDLE ci = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE co = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    assert(ci != INVALID_HANDLE_VALUE && co != INVALID_HANDLE_VALUE);
    int fi = _open_osfhandle((intptr_t)ci, _O_RDONLY | _O_BINARY);
    int fo = _open_osfhandle((intptr_t)co, _O_WRONLY | _O_BINARY);
    assert(fi >= 0 && fo >= 0 && !_dup2(fi, 0) && !_dup2(fo, 1) && !_dup2(fo, 2));
    _close(fi); _close(fo);
    assert(SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(0)));
    assert(SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(1)));
    assert(SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2)));
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD im, om, after_i, after_o;
    assert(GetConsoleMode(in, &im) && GetConsoleMode(out, &om));
    assert(SetConsoleCP(437) && SetConsoleOutputCP(850));
    LmbTuiState *st = calloc(1, sizeof *st); assert(st);
    st->nmodels = 2; st->nnodes = 1; st->inventory_ok = 1;
    strcpy(st->tracker, "127.0.0.1:47300");
    strcpy(st->nodes[0].name, "Native computer");
    strcpy(st->nodes[0].addr, "127.0.0.1:47301");
    strcpy(st->identities[0], "native-peer");
    st->nodes[0].ram_budget_bytes = UINT64_C(16)<<30;
    for (int i=0; i<2; i++) {
        snprintf(st->models[i].name, sizeof st->models[i].name, "Model %d", i);
        snprintf(st->models[i].dir, sizeof st->models[i].dir, "model-%d", i);
        st->models[i].planned = 1; st->models[i].plan.state = LMB_PLAN_RESIDENT;
        st->models[i].shape.layers = 2;
    }
    strcpy(st->models[1].name, "Modèle 日本");
    int action = lmb_tui_run(st, 0, NULL);
    assert(GetConsoleMode(in, &after_i) && GetConsoleMode(out, &after_o));
    assert(after_i == im && after_o == om);
    assert(GetConsoleCP() == 437 && GetConsoleOutputCP() == 850);
    if (scenario == 1) assert(!action && lmb_tui_node_enabled(st, 0));
    if (scenario == 2) assert(action == LMB_TUI_REQUEST_CHAT && st->action_model == 1);
    if (scenario == 3) assert(!action);
    free(st);
    puts("CONSOLE_RESTORED"); fflush(stdout); return 0;
}

static void scenario(const wchar_t *binary, int mode) {
    HANDLE input_read, input_write, output_read, output_write;
    assert(CreatePipe(&input_read, &input_write, NULL, 0));
    assert(CreatePipe(&output_read, &output_write, NULL, 0));
    HPCON console;
    assert(SUCCEEDED(CreatePseudoConsole((COORD){104,38}, input_read, output_write, 0, &console)));
    CloseHandle(input_read); CloseHandle(output_write);
    Capture *c = calloc(1, sizeof *c); assert(c); c->pipe = output_read;
    InitializeCriticalSection(&c->mutex);
    HANDLE reader = CreateThread(NULL, 0, capture, c, 0, NULL); assert(reader);
    HANDLE job = CreateJobObjectW(NULL, NULL); assert(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    assert(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits));
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(NULL, 2, 0, &bytes);
    STARTUPINFOEXW start = {0}; start.StartupInfo.cb = sizeof start;
    start.lpAttributeList = malloc(bytes); assert(start.lpAttributeList);
    assert(InitializeProcThreadAttributeList(start.lpAttributeList, 2, 0, &bytes));
    assert(UpdateProcThreadAttribute(start.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof console, NULL, NULL));
    assert(UpdateProcThreadAttribute(start.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof job, NULL, NULL));
    wchar_t args[32768];
    assert(swprintf(args, 32768, L"\"%ls\" --child %d", binary, mode) > 0);
    PROCESS_INFORMATION pi;
    assert(CreateProcessW(binary, args, NULL, NULL, FALSE,
        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &start.StartupInfo, &pi));
    DeleteProcThreadAttributeList(start.lpAttributeList); free(start.lpAttributeList);
    CloseHandle(pi.hThread);
    expect(c, "Esc back");
    expect(c, "Modèle 日本");
    if (mode == 1) {
        /* Resize and navigate to a real computer selection, then exit. */
        assert(SUCCEEDED(ResizePseudoConsole(console, (COORD){50,20})));
        expect(c, "Resize to at least");
        assert(SUCCEEDED(ResizePseudoConsole(console, (COORD){104,38})));
        Sleep(300);
        send_keys(input_write, "\t \x1b");
    } else if (mode == 2) send_keys(input_write, "\x1b[B\r\r");
    else send_keys(input_write, "\x03");
    expect(c, "CONSOLE_RESTORED");
    assert(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code; assert(GetExitCodeProcess(pi.hProcess, &code) && code == 0);
    CloseHandle(pi.hProcess); CloseHandle(input_write); CloseHandle(job);
    ClosePseudoConsole(console);
    assert(WaitForSingleObject(reader, 10000) == WAIT_OBJECT_0);
    CloseHandle(reader); CloseHandle(output_read);
    DeleteCriticalSection(&c->mutex); free(c);
}

int wmain(int argc, wchar_t **argv) {
    assert(lmb_console_width(0x00e8) == 1);
    assert(lmb_console_width(0x00b7) == 1);
    assert(lmb_console_width(0x65e5) == 2);
    if (argc == 3 && !wcscmp(argv[1], L"--child")) return child(_wtoi(argv[2]));
    assert(argc == 1);
    wchar_t binary[32768];
    DWORD n = GetModuleFileNameW(NULL, binary, 32768); assert(n && n < 32768);
    for (int mode=1; mode<=3; mode++) scenario(binary, mode);
    puts("WINDOWS TUI PASS: native keys, selection, Unicode, resize, Ctrl-C, console restoration");
    return 0;
}
