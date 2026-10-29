#include "src/runtime/lumabri_process_probe_windows.h"
#include <assert.h>
#include <stdio.h>

static void utf8(char *out, int cap, const wchar_t *wide) {
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, cap, NULL, NULL));
}
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc >= 2 && !wcscmp(argv[1], L"--echo")) {
        for (int i = 2; i < argc; i++) { char text[4096]; utf8(text, sizeof text, argv[i]); puts(text); }
        return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--handle")) {
        (void)SetEvent((HANDLE)(uintptr_t)wcstoull(argv[2], NULL, 10)); puts("done"); return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--flood")) {
        for (unsigned i = 0; i < 80000; i++) putchar('x');
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--fail")) { puts("8"); return 17; }
    if (argc == 3 && !wcscmp(argv[1], L"--descendant")) {
        HANDLE event = CreateEventW(NULL, TRUE, TRUE, argv[2]); assert(event);
        Sleep(INFINITE); return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--tree")) {
        wchar_t exe[2048], command[4096]; assert(GetModuleFileNameW(NULL, exe, 2048));
        assert(swprintf(command, 4096, L"\"%ls\" --descendant \"%ls\"", exe, argv[2]) > 0);
        STARTUPINFOW startup = {0}; PROCESS_INFORMATION child = {0}; startup.cb = sizeof startup;
        assert(CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child));
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        HANDLE event = NULL; ULONGLONG until = GetTickCount64() + 1500;
        while (!event && GetTickCount64() < until) { event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]); if (!event) Sleep(5); }
        assert(event); CloseHandle(event); puts("ready"); fflush(stdout); Sleep(INFINITE); return 0;
    }
    wchar_t exe[2048], base[512], directory[700], copied[750];
    assert(GetModuleFileNameW(NULL, exe, 2048)); assert(GetTempPathW(512, base));
    assert(swprintf(directory, 700, L"%lslumabri probe \x00e8 %lu", base, GetCurrentProcessId()) > 0);
    assert(CreateDirectoryW(directory, NULL));
    assert(swprintf(copied, 750, L"%ls\\probe.exe", directory) > 0); assert(CopyFileW(exe, copied, TRUE));
    char binary[2300], reply[1024]; utf8(binary, sizeof binary, copied);
    const char *arguments[] = {"--echo", "a b", "back\\", "a\\\"b", "", "\xc3\xa8\xce\xa9", "&;$(no-shell)"};
    const char *want = "a b\r\nback\\\r\na\\\"b\r\n\r\n\xc3\xa8\xce\xa9\r\n&;$(no-shell)\r\n";
    size_t length = 0; DWORD status = MAXDWORD;
    assert(!lmb_windows_probe_capture(binary, arguments, 7, reply, sizeof reply, &length, &status, 2000));
    assert(!status && length == strlen(want) && !memcmp(reply, want, length));
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE private_event = CreateEventW(&sa, TRUE, FALSE, NULL); assert(private_event);
    char value[32]; snprintf(value, sizeof value, "%llu", (unsigned long long)(uintptr_t)private_event);
    const char *handle[] = {"--handle", value};
    assert(!lmb_windows_probe_capture(binary, handle, 2, reply, sizeof reply, &length, &status, 2000) && !status);
    assert(WaitForSingleObject(private_event, 0) == WAIT_TIMEOUT); CloseHandle(private_event);
    const char *flood[] = {"--flood"}, *fail[] = {"--fail"};
    assert(lmb_windows_probe_capture(binary, flood, 1, reply, sizeof reply, &length, &status, 2000) && errno == EMSGSIZE);
    assert(!lmb_windows_probe_capture(binary, fail, 1, reply, sizeof reply, &length, &status, 2000) && status == 17);
    char event_name[128]; wchar_t wide_event[128];
    snprintf(event_name, sizeof event_name, "Local\\lumabri-probe-tree-%lu", GetCurrentProcessId());
    assert(MultiByteToWideChar(CP_UTF8, 0, event_name, -1, wide_event, 128));
    const char *tree[] = {"--tree", event_name};
    assert(lmb_windows_probe_capture(binary, tree, 2, reply, sizeof reply, &length, &status, 2000) && errno == ETIMEDOUT);
    assert(!strcmp(reply, "ready\r\n"));
    ULONGLONG until = GetTickCount64() + 3000; HANDLE survivor = NULL;
    do {
        survivor = OpenEventW(SYNCHRONIZE, FALSE, wide_event);
        if (!survivor) break;
        CloseHandle(survivor); Sleep(5);
    } while (GetTickCount64() < until);
    assert(!survivor && GetLastError() == ERROR_FILE_NOT_FOUND);
    assert(DeleteFileW(copied)); assert(RemoveDirectoryW(directory));
    puts("NATIVE PROCESS PROBE: PASS (Unicode/spaced executable, exact arguments, handle isolation, bounds, exit status, hung process-tree cleanup)");
    return 0;
}
