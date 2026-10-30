/* Exercise the real native lumabri.exe entrypoint, not a manager test double.
 * Every process belongs to an owned test job and a temporary local profile. */
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include <assert.h>

static LmbOwnedProcess children[40];
static unsigned count;
static char binary[6144], fixture[6144];

static LmbOwnedProcess *start(const char *program, const char *const *args, size_t nargs, HANDLE output) {
    assert(count < sizeof children / sizeof *children);
    LmbOwnedProcess *p = &children[count++];
    LmbProcessLaunch launch = {.binary = program, .args = args, .nargs = nargs,
                              .output = output, .error = output};
    assert(!lmb_owned_process_start(p, &launch));
    return p;
}

static DWORD exited(LmbOwnedProcess *p) {
    /* Only the launcher must exit. Its detached manager must remain alive. */
    assert(WaitForSingleObject(p->process, 15000) == WAIT_OBJECT_0);
    DWORD code; assert(GetExitCodeProcess(p->process, &code)); return code;
}

static DWORD command(const char *action, char *output, size_t cap) {
    wchar_t dir[1024], name[1200];
    assert(GetTempPathW(1024, dir));
    assert(GetTempFileNameW(dir, L"lmb", 0, name));
    HANDLE file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    const char *args[] = {"service", action, "--json"};
    DWORD code = exited(start(binary, args, !strcmp(action, "status") ? 3 : 2, file));
    LARGE_INTEGER zero = {0};
    assert(SetFilePointerEx(file, zero, NULL, FILE_BEGIN));
    if (output) {
        DWORD got = 0;
        assert(ReadFile(file, output, (DWORD)cap - 1, &got, NULL)); output[got] = 0;
    }
    CloseHandle(file); return code;
}

static HomeServiceSnapshot status(const char *role) {
    HomeServiceSnapshot s;
    ULONGLONG until = GetTickCount64() + 5000;
    while (home_service_query(role, HOME_SVC_STATUS, NULL, &s)) {
        assert(GetTickCount64() < until); Sleep(10);
    }
    return s;
}

static int donor_fixture(void) {
    HomeService s; if (home_service_open(&s, "donor")) return 1;
    s.snapshot.ram = 4096; s.snapshot.reserved_total = 2048; s.snapshot.model_count = 2;
    if (home_service_save(&s)) return 1;
    for (;;) {
        int op = home_service_poll(&s);
        if (op == HOME_SVC_STOP) {
            s.snapshot.state = HOME_SVC_STOPPED;
            s.snapshot.model_count = 0; s.snapshot.reserved_total = 0;
            assert(!home_service_save(&s)); home_service_answer(&s);
            home_service_close(&s); return 0;
        }
        home_service_answer(&s); Sleep(10);
    }
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--donor-fixture")) return donor_fixture();
    assert(argc == 2);
    wchar_t exe[2048], base[1024], temporary[1100];
    assert(GetModuleFileNameW(NULL, exe, 2048));
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, exe, -1, fixture, sizeof fixture, NULL, NULL));
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1, binary, sizeof binary, NULL, NULL));
    assert(GetTempPathW(1024, base));
    assert(swprintf(temporary, 1100, L"%lslumabri-manager-%lu-%llu-\x00e8", base,
        GetCurrentProcessId(), GetTickCount64()) > 0);
    assert(CreateDirectoryW(temporary, NULL));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", temporary));
    assert(SetEnvironmentVariableW(L"LUMABRI_HOME_FOREGROUND", NULL));
    assert(SetEnvironmentVariableW(L"LUMABRI_READY_HANDLE", NULL));

    char json[8192];
    assert(!command("status", json, sizeof json) && strstr(json, "\"live\":false"));
    assert(SetEnvironmentVariableW(L"LUMABRI_HOME_FOREGROUND", L"1"));
    assert(command("start", json, sizeof json) != 0 && strstr(json, "LUMABRI_HOME_FOREGROUND"));
    assert(SetEnvironmentVariableW(L"LUMABRI_HOME_FOREGROUND", NULL));
    const char *internal[] = {"--internal-manager"};
    assert(exited(start(binary, internal, 1, NULL)) == 2);
    const char *args[] = {"service", "start"};
    LmbOwnedProcess *starters[8];
    for (unsigned i = 0; i < 8; i++) starters[i] = start(binary, args, 2, NULL);
    for (unsigned i = 0; i < 8; i++) {
        DWORD code = exited(starters[i]);
        if (code) fprintf(stderr, "starter %u failed: %lu\n", i, code);
        assert(!code);
    }
    HomeServiceSnapshot first = status("manager");
    assert(first.state == HOME_SVC_RUNNING && first.pid);
    assert(!command("status", json, sizeof json));
    assert(strstr(json, "\"role\":\"manager\",\"state\":\"live\""));
    HomeServiceSnapshot same = status("manager");
    assert(!memcmp(first.instance, same.instance, 32));

    const char *donor_args[] = {"--donor-fixture"};
    LmbOwnedProcess *donor = start(fixture, donor_args, 1, NULL);
    HomeServiceSnapshot kept = status("donor");
    assert(kept.model_count == 2 && kept.reserved_total == 2048);
    assert(!command("restart", NULL, 0));
    LmbOwnedProcess *replacement = &children[count - 1];
    HomeServiceSnapshot second = status("manager"), after = status("donor");
    assert(memcmp(first.instance, second.instance, 32));
    assert(!memcmp(kept.instance, after.instance, 32) && after.model_count == 2 && after.reserved_total == 2048);
    assert(home_service_query("manager", HOME_SVC_STOP, &first, &after));

    /* Crash only our newly launched manager's job. Its PID is never reopened.
     * A dead journal cannot recreate models or claim a live reservation. */
    assert(!lmb_owned_process_stop(replacement, 0, 5000));
    assert(!home_service_record("manager", &after) && after.state == HOME_SVC_RUNNING);
    assert(!command("status", json, sizeof json) && strstr(json, "\"state\":\"interrupted\""));
    assert(!command("start", NULL, 0));
    second = status("manager"); after = status("donor");
    assert(!memcmp(kept.instance, after.instance, 32) && after.model_count == 2 && after.reserved_total == 2048);
    assert(strstr(second.detail, "1 live keeper(s)"));
    assert(!command("stop", NULL, 0));
    assert(!exited(donor));
    assert(!command("status", json, sizeof json) && !strstr(json, "\"live\":true"));

    /* An unresponsive but locked singleton must not be reported stopped. */
    HomeService held; assert(!home_service_open(&held, "manager"));
    assert(!home_service_save(&held));
    assert(command("restart", NULL, 0) != 0);
    assert(command("start", NULL, 0) != 0);
    home_service_close(&held);
    assert(!command("stop", NULL, 0));
    for (unsigned i = 0; i < count; i++) {
        assert(!lmb_owned_process_stop(&children[i], 0, 5000));
        assert(!lmb_owned_process_close(&children[i]));
    }
    char path[1200];
    const char *roles[] = {"manager", "donor"};
    const char *suffixes[] = {"lock", "state", "log"};
    for (unsigned r = 0; r < 2; r++) for (unsigned s = 0; s < 3; s++) {
        assert(!home_service_path(roles[r], suffixes[s], path, sizeof path));
        wchar_t *wide = lmb_process_wide(path); assert(wide);
        (void)DeleteFileW(wide); free(wide);
    }
    assert(!home_service_directory(path, sizeof path));
    wchar_t *directory = lmb_process_wide(path); assert(directory);
    assert(RemoveDirectoryW(directory)); free(directory);
    wchar_t own[1100]; assert(GetEnvironmentVariableW(L"LOCALAPPDATA", own, 1100));
    assert(swprintf(temporary, 1100, L"%ls\\Lumabri", own) > 0);
    assert(RemoveDirectoryW(temporary)); assert(RemoveDirectoryW(own));
    puts("WINDOWS MANAGER PASS: real CLI, concurrent start, terminal independence, keeper-preserving restart, crash reconciliation and lock-fenced stop");
    return 0;
}
