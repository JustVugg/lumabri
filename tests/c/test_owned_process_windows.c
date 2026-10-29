/* Real Windows engine ownership: no process-name or saved-PID signalling. */
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_ready.h"
#include <assert.h>

static void utf8(char *out, int cap, const wchar_t *wide) {
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, cap, NULL, NULL));
}
static HANDLE inherited(const char *name) {
    char value[64], *end; assert(GetEnvironmentVariableA(name, value, sizeof value));
    unsigned long long n = strtoull(value, &end, 10); assert(n && !*end);
    HANDLE handle = (HANDLE)(uintptr_t)n; DWORD flags;
    assert(GetHandleInformation(handle, &flags)); return handle;
}
static void launch_leaf(const wchar_t *exe) {
    wchar_t line[4096]; assert(swprintf(line, 4096, L"\"%ls\" --leaf", exe) > 0);
    STARTUPINFOW startup = {0}; PROCESS_INFORMATION child = {0}; startup.cb = sizeof startup;
    assert(CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child));
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
}
static void warm_native_process_runtime(const wchar_t *exe) {
    /* Establish the handle baseline after Windows' first local-image launch.
     * A separate raw Win32 probe reproduces the one-time retained handles;
     * this setup deliberately does not call the Lumabri launch helper. */
    wchar_t line[4096]; assert(swprintf(line,4096,L"\"%ls\" --exit",exe)>0);
    STARTUPINFOW startup={0}; PROCESS_INFORMATION child={0}; startup.cb=sizeof startup;
    assert(CreateProcessW(exe,line,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&child));
    assert(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0);
    DWORD code; assert(GetExitCodeProcess(child.hProcess,&code) && code==37);
    assert(CloseHandle(child.hThread) && CloseHandle(child.hProcess));
}
static void supervise_engine(const wchar_t *exe, const wchar_t *model) {
    char binary[4096], checkpoint[4096]; utf8(binary, sizeof binary, exe); utf8(checkpoint, sizeof checkpoint, model);
    const char *args[] = {checkpoint};
    LmbProcessLaunch launch = {.binary=binary, .args=args, .nargs=1,
        .output=GetStdHandle(STD_OUTPUT_HANDLE), .error=GetStdHandle(STD_ERROR_HANDLE)};
    LmbOwnedProcess engine = {0}; assert(!lmb_owned_process_start(&engine, &launch));
    DWORD status = MAXDWORD;
    int result = lmb_owned_process_wait(&engine, 60000, &status);
    if (result) (void)lmb_owned_process_stop(&engine, 0, 5000);
    assert(!result && !status && !lmb_owned_process_close(&engine));
    puts("OWNED OLMOE: PASS (real resident Edge and split Segment under native process-tree ownership)");
}

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wchar_t exe[2048]; assert(GetModuleFileNameW(NULL, exe, 2048));
    if (argc == 4 && !wcscmp(argv[1], L"--engine")) { supervise_engine(argv[2], argv[3]); return 0; }
    if (argc == 2 && !wcscmp(argv[1], L"--leaf")) { Sleep(INFINITE); return 0; }
    if (argc == 2 && !wcscmp(argv[1], L"--exit")) return 37;
    if (argc == 2 && !wcscmp(argv[1], L"--tree")) {
        launch_leaf(exe); assert(!lmb_ready_notify()); return 7;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--ready-leaf")) { assert(!lmb_ready_notify()); Sleep(INFINITE); return 0; }
    if (argc == 2 && !wcscmp(argv[1], L"--owner")) {
        char binary[4096]; utf8(binary, sizeof binary, exe);
        const char *args[] = {"--ready-leaf"};
        LmbChildHandle ready = {"LUMABRI_READY_HANDLE", inherited("LUMABRI_READY_HANDLE")};
        LmbProcessLaunch launch = {.binary=binary, .args=args, .nargs=1, .handles=&ready, .nhandles=1};
        LmbOwnedProcess child = {0}; assert(!lmb_owned_process_start(&child, &launch));
        Sleep(INFINITE); return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--cooperate")) {
        wchar_t value[100]; assert(GetEnvironmentVariableW(L"LMB_PROCESS_PARENT", value, 100));
        assert(!wcscmp(value, L"child \x00e8\x03a9"));
        assert(GetEnvironmentVariableW(L"LMB_PROCESS_SPECIAL", value, 100));
        assert(!wcscmp(value, L"&;$(no-shell)"));
        /* An unrelated inheritable event must not escape the whitelist. */
        (void)SetEvent((HANDLE)(uintptr_t)wcstoull(argv[2], NULL, 10));
        HANDLE stop = inherited("LUMABRI_STOP_HANDLE");
        assert(!lmb_ready_notify() && !lmb_ready_notify());
        assert(WaitForSingleObject(stop, 10000) == WAIT_OBJECT_0); CloseHandle(stop); return 37;
    }
    assert(argc == 1);
    wchar_t base[512], directory[700], copied[750];
    assert(GetTempPathW(512, base));
    assert(swprintf(directory, 700, L"%lslumabri owned \x00e8 %lu", base, GetCurrentProcessId()) > 0);
    assert(CreateDirectoryW(directory, NULL));
    assert(swprintf(copied, 750, L"%ls\\worker.exe", directory) > 0 && CopyFileW(exe, copied, TRUE));
    char binary[4096]; utf8(binary, sizeof binary, copied);
    assert(SetEnvironmentVariableW(L"LMB_PROCESS_PARENT", L"parent unchanged"));
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL), stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(ready && stop);
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    HANDLE unrelated = CreateEventW(&inherit, TRUE, FALSE, NULL); assert(unrelated);
    char value[32]; snprintf(value, sizeof value, "%llu", (unsigned long long)(uintptr_t)unrelated);
    const char *args[] = {"--cooperate", value};
    const char *env[] = {"lmb_process_parent=child \xc3\xa8\xce\xa9", "LMB_PROCESS_SPECIAL=&;$(no-shell)"};
    LmbChildHandle bindings[] = {{"LUMABRI_READY_HANDLE", ready}, {"LUMABRI_STOP_HANDLE", stop}};
    LmbProcessLaunch launch = {.binary=binary, .args=args, .nargs=2, .environment=env, .nenvironment=2,
        .handles=bindings, .nhandles=2, .stop=stop};
    LmbOwnedProcess child = {0};
    warm_native_process_runtime(copied);
    DWORD before = 0, after = 0; assert(GetProcessHandleCount(GetCurrentProcess(), &before));
    assert(!lmb_owned_process_start(&child, &launch));
    assert(lmb_owned_process_start(&child, &launch) && errno == EINVAL); /* Never overwrite live ownership. */
    assert(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(unrelated, 0) == WAIT_TIMEOUT);
    wchar_t parent[100]; assert(GetEnvironmentVariableW(L"LMB_PROCESS_PARENT", parent, 100));
    assert(!wcscmp(parent, L"parent unchanged"));
    DWORD flags; assert(GetHandleInformation(ready, &flags) && !(flags & HANDLE_FLAG_INHERIT));
    assert(GetHandleInformation(stop, &flags) && !(flags & HANDLE_FLAG_INHERIT));
    assert(lmb_owned_process_alive(&child) == 1 && lmb_owned_process_close(&child) && errno == EBUSY);
    assert(!lmb_owned_process_stop(&child, 3000, 3000));
    DWORD status = 0; assert(!lmb_owned_process_wait(&child, 0, &status) && status == 37);
    assert(!lmb_owned_process_close(&child));
    assert(GetProcessHandleCount(GetCurrentProcess(), &after));
    assert(after == before);

    ResetEvent(ready); ResetEvent(stop); const char *tree[] = {"--tree"};
    launch.args=tree; launch.nargs=1; launch.environment=NULL; launch.nenvironment=0;
    launch.stop=NULL; launch.nhandles=1;
    assert(!lmb_owned_process_start(&child, &launch));
    assert(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0);
    assert(lmb_owned_process_alive(&child) == 1); /* Immediate parent exited, grandchild did not. */
    assert(lmb_owned_process_wait(&child, 30, NULL) == 1);
    assert(lmb_owned_process_close(&child) && errno == EBUSY);
    assert(!lmb_owned_process_stop(&child, 0, 3000));
    assert(!lmb_owned_process_wait(&child, 0, &status) && status == 7);
    assert(!lmb_owned_process_close(&child));

    assert(GetProcessHandleCount(GetCurrentProcess(), &after));
    assert(after == before);

    ResetEvent(ready); const char *owner[] = {"--owner"}; launch.args=owner;
    assert(!lmb_owned_process_start(&child, &launch));
    assert(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    /* Crash only our owner handle. Its private job must clean descendants. */
    assert(TerminateProcess(child.process, 93));
    assert(!lmb_owned_process_wait(&child, 5000, &status) && status == 93);
    assert(!lmb_owned_process_close(&child));

    assert(GetProcessHandleCount(GetCurrentProcess(), &after));
    assert(after == before);

    const char *exit_args[] = {"--exit"}; launch.args=exit_args; launch.handles=NULL; launch.nhandles=0;
    for (unsigned i = 0; i < 16; i++) {
        assert(!lmb_owned_process_start(&child, &launch));
        assert(!lmb_owned_process_wait(&child, 5000, &status) && status == 37);
        assert(!lmb_owned_process_close(&child));
        assert(GetProcessHandleCount(GetCurrentProcess(), &after));
        assert(after == before);
    }
    const char *duplicates[] = {"LMB_A=1", "lmb_a=2"}; launch.environment=duplicates; launch.nenvironment=2;
    assert(lmb_owned_process_start(&child, &launch) && !child.process && !child.job);
    launch.environment=NULL; launch.nenvironment=0;
    LmbChildHandle invalid = {"LUMABRI_READY_HANDLE", INVALID_HANDLE_VALUE}; launch.handles=&invalid; launch.nhandles=1;
    assert(lmb_owned_process_start(&child, &launch) && !child.process && !child.job);
    assert(GetProcessHandleCount(GetCurrentProcess(), &after));
    assert(after == before);
    CloseHandle(unrelated); CloseHandle(stop); CloseHandle(ready);
    assert(SetEnvironmentVariableW(L"LMB_PROCESS_PARENT", NULL));
    assert(DeleteFileW(copied) && RemoveDirectoryW(directory));
    puts("OWNED PROCESS: PASS (atomic job ownership, Unicode env/argv, READY, cooperative stop, descendant exit, owner crash, private handles, no leaks)");
    return 0;
}
