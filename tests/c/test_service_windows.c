/* Actual native keeper/client processes, on an isolated local profile. No
 * installed service, machine setting, or persisted PID is ever modified. */
#include "src/runtime/lumabri_service.h"
#include <assert.h>

static HANDLE family;
static PROCESS_INFORMATION start(const wchar_t *argument) {
    wchar_t exe[2048], command[4096]; PROCESS_INFORMATION child = {0};
    STARTUPINFOW startup = {0}; startup.cb = sizeof startup;
    assert(GetModuleFileNameW(NULL, exe, 2048));
    assert(swprintf(command, 4096, L"\"%ls\" %ls", exe, argument) > 0);
    assert(CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                         NULL, NULL, &startup, &child));
    assert(AssignProcessToJobObject(family, child.hProcess));
    assert(ResumeThread(child.hThread) != MAXDWORD);
    CloseHandle(child.hThread); child.hThread = NULL; return child;
}
static void ended(PROCESS_INFORMATION *child, DWORD expected) {
    assert(WaitForSingleObject(child->hProcess, 5000) == WAIT_OBJECT_0);
    DWORD code;
    assert(GetExitCodeProcess(child->hProcess, &code) && code == expected);
    CloseHandle(child->hProcess); child->hProcess = NULL;
}
static HomeServiceSnapshot status(const char *role) {
    HomeServiceSnapshot current;
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (home_service_query(role, HOME_SVC_STATUS, NULL, &current)) {
        assert(GetTickCount64() < deadline); Sleep(10);
    }
    return current;
}
static void stop(const char *role) {
    HomeServiceSnapshot current = status(role), reply;
    assert(!home_service_query(role, HOME_SVC_STOP, &current, &reply));
    assert(reply.state == HOME_SVC_STOPPED);
}
static int keeper(const char *role) {
    HomeService service;
    if (home_service_open(&service, role)) return 20;
    service.snapshot.revision = UINT64_C(0x100000003);
    service.snapshot.ram = 4096; service.snapshot.reserved_total = 1024;
    service.snapshot.model_count = !strcmp(role, "donor") ? 1 : 0;
    if (home_service_save(&service)) return 21;
    ULONGLONG deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline) {
        int op = home_service_poll(&service);
        if (op < 0) { Sleep(2); continue; }
        if (op == HOME_SVC_STOP) service.snapshot.state = HOME_SVC_STOPPED;
        if (op == HOME_SVC_UNLOAD) {
            service.snapshot.model_count = 0;
            service.snapshot.reserved_total = 0; service.snapshot.revision++;
        }
        if (home_service_save(&service)) return 1000 + (int)GetLastError();
        home_service_answer(&service);
        if (op == HOME_SVC_STOP) { home_service_close(&service); return 0; }
    }
    home_service_close(&service); return 22;
}

static void malformed(const char *role, int mode) {
    HANDLE h = home_service_connect(role); assert(h != INVALID_HANDLE_VALUE);
    uint8_t request[80] = {0}, byte;
    lmb_put32(request, HOME_SVC_VERSION); lmb_put32(request + 4, 99);
    assert(!home_service_io(h, request, mode ? 1 : sizeof request, 1, GetTickCount64() + 2000));
    /* Even an otherwise idle client has a bounded read deadline. */
    ULONGLONG begin = GetTickCount64();
    assert(home_service_io(h, &byte, 1, 0, begin + 2000));
    assert(GetTickCount64() - begin < 1500);
    CloseHandle(h);
}

static void anonymous_refused(void) {
    char path[1200]; wchar_t pipe[256];
    assert(!home_service_socket_path("donor", path, sizeof path));
    assert(MultiByteToWideChar(CP_UTF8, 0, path, -1, pipe, 256));
    HANDLE h;
    ULONGLONG deadline = GetTickCount64() + 2000;
    do {
        h = CreateFileW(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        assert(GetLastError() == ERROR_PIPE_BUSY && GetTickCount64() < deadline);
        Sleep(5);
    } while (1);
    uint8_t request[80], response;
    assert(!home_service_request_pack(request, HOME_SVC_STATUS, NULL));
    assert(!home_service_io(h, request, sizeof request, 1, GetTickCount64() + 2000));
    assert(home_service_io(h, &response, 1, 0, GetTickCount64() + 2000));
    CloseHandle(h);
}

static void acl_and_handles(void) {
    /* A separate, owned role exposes the live listener for ACL inspection. */
    HomeService service; assert(!home_service_open(&service, "tracker"));
    DWORD flags;
    assert(GetHandleInformation(service.listener, &flags) && !(flags & HANDLE_FLAG_INHERIT));
    assert(GetHandleInformation(service.lock, &flags) && !(flags & HANDLE_FLAG_INHERIT));
    PSECURITY_DESCRIPTOR sd = NULL; PACL acl = NULL; PSID owner = NULL;
    assert(GetSecurityInfo(service.listener, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, NULL, &acl, NULL, &sd) == ERROR_SUCCESS);
    LmbPrivateSecurity own; assert(!lmb_private_security(&own));
    assert(owner && EqualSid(owner, own.user->User.Sid) && acl && acl->AceCount == 2);
    for (DWORD i = 0; i < acl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace; assert(GetAce(acl, i, (void **)&ace));
        assert(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        PSID sid = &ace->SidStart;
        assert(EqualSid(sid, own.user->User.Sid) || IsWellKnownSid(sid, WinLocalSystemSid));
    }
    LocalFree(sd); lmb_private_security_free(&own);
    /* Close while ConnectNamedPipe is pending: no dangling OVERLAPPED. */
    assert(home_service_poll(&service) < 0 && service.connecting);
    home_service_close(&service);
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--keeper"))
        return keeper(!wcscmp(argv[2], L"donor") ? "donor" : "manager");
    if (argc == 2 && !wcscmp(argv[1], L"--client")) {
        HomeServiceSnapshot donor = status("donor");
        return donor.model_count == 1 ? 0 : 25;
    }
    wchar_t base[1024], temporary[1100];
    DWORD n = GetTempPathW(1024, base); assert(n && n < 1024);
    assert(swprintf(temporary, 1100, L"%lslumabri-service-%lu-%llu-\x00e8", base,
                    GetCurrentProcessId(), GetTickCount64()) > 0);
    assert(CreateDirectoryW(temporary, NULL));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", temporary));
    family = CreateJobObjectW(NULL, NULL); assert(family);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    assert(SetInformationJobObject(family, JobObjectExtendedLimitInformation, &limits, sizeof limits));

    acl_and_handles();
    PROCESS_INFORMATION donor = start(L"--keeper donor"), manager = start(L"--keeper manager");
    HomeServiceSnapshot first = status("donor"), old_manager = status("manager"), snapshot;
    assert(first.model_count == 1 && first.reserved_total == 1024);
    assert(!home_service_record("donor", &snapshot) && !memcmp(first.instance, snapshot.instance, 32));
    PROCESS_INFORMATION duplicate = start(L"--keeper donor"); ended(&duplicate, 20);
    /* Independent interfaces can come and go without owning the keeper. */
    for (int i = 0; i < 4; i++) { PROCESS_INFORMATION client = start(L"--client"); ended(&client, 0); }
    HomeServiceSnapshot stale = first;
    stale.instance[0] ^= 1;
    assert(home_service_query("donor", HOME_SVC_STOP, &stale, &snapshot));
    stale = first; stale.revision ^= UINT64_C(1) << 32;
    assert(home_service_query("donor", HOME_SVC_STOP, &stale, &snapshot));
    stale = first; stale.offer.id[0] ^= 1;
    assert(home_service_query("donor", HOME_SVC_STOP, &stale, &snapshot));
    malformed("donor", 0); malformed("donor", 1); anonymous_refused();
    snapshot = status("donor"); assert(!memcmp(snapshot.instance, first.instance, 32));
    DWORD handles_before, handles_after;
    assert(GetProcessHandleCount(donor.hProcess, &handles_before));
    for (int i = 0; i < 40; i++) snapshot = status("donor");
    Sleep(50); assert(GetProcessHandleCount(donor.hProcess, &handles_after));
    assert(handles_after <= handles_before + 1);

    stop("manager"); ended(&manager, 0);
    manager = start(L"--keeper manager"); snapshot = status("manager");
    assert(memcmp(snapshot.instance, old_manager.instance, 32));
    snapshot = status("donor"); assert(!memcmp(snapshot.instance, first.instance, 32));
    assert(snapshot.model_count == 1 && snapshot.reserved_total == first.reserved_total);
    char record_path[1200]; wchar_t record_wide[1200];
    assert(!home_service_path("donor", "state", record_path, sizeof record_path));
    assert(MultiByteToWideChar(CP_UTF8, 0, record_path, -1, record_wide, 1200));
    HANDLE old_record = CreateFileW(record_wide, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    assert(old_record != INVALID_HANDLE_VALUE);
    int unload = home_service_query("donor", HOME_SVC_UNLOAD, &snapshot, &snapshot);
    if (unload) { DWORD code = 0; GetExitCodeProcess(donor.hProcess, &code);
        fprintf(stderr, "UNLOAD failed: child_exit=%lu win_error=%lu errno=%d\n", code, GetLastError(), errno); }
    assert(!unload);
    assert(!snapshot.model_count && !snapshot.reserved_total && snapshot.revision == first.revision + 1);
    uint8_t bytes[HOME_SVC_MAX]; DWORD got;
    assert(ReadFile(old_record, bytes, sizeof bytes, &got, NULL)); CloseHandle(old_record);
    HomeServiceSnapshot previous;
    assert(!home_service_unpack(bytes, got, &previous) && previous.model_count == 1);
    assert(!home_service_record("donor", &previous) && !previous.model_count);
    stop("donor"); ended(&donor, 0);
    assert(!home_service_record("donor", &snapshot) && snapshot.state == HOME_SVC_STOPPED);
    assert(home_service_query("donor", HOME_SVC_STATUS, NULL, &snapshot));

    donor = start(L"--keeper donor"); snapshot = status("donor");
    assert(memcmp(snapshot.instance, first.instance, 32));
    /* Kill only the exact process handle created by this test. The stale
     * journal must not be mistaken for an active keeper or a new approval. */
    assert(TerminateProcess(donor.hProcess, 23)); ended(&donor, 23);
    assert(!home_service_record("donor", &snapshot) && snapshot.state == HOME_SVC_RUNNING);
    assert(home_service_query("donor", HOME_SVC_STATUS, NULL, &snapshot));
    HomeServiceSnapshot crashed = snapshot;
    donor = start(L"--keeper donor"); snapshot = status("donor");
    assert(memcmp(snapshot.instance, crashed.instance, 32));
    assert(home_service_query("donor", HOME_SVC_STOP, &crashed, &first));
    stop("donor"); ended(&donor, 0); stop("manager"); ended(&manager, 0);
    CloseHandle(family);

    HomeService preparation; assert(!home_service_open(&preparation, "prepare"));
    preparation.snapshot.preparation_results_known = 1;
    uint8_t allocation[32] = {42};
    assert(!home_service_preparation_assign(&preparation, "model-a", allocation));
    assert(home_service_preparation_assign(&preparation, "model-a", allocation));
    assert(!home_service_preparation_ready(&preparation, allocation));
    assert(!home_service_record("prepare", &snapshot));
    assert(snapshot.preparation_result_count == 1 && snapshot.preparation_results[0].ready_observed);
    /* Unsafe state is refused, never inherited by the next atomic write. */
    wchar_t unsafe[1200];
    assert(MultiByteToWideChar(CP_UTF8, 0, preparation.journal, -1, unsafe, 1200));
    HANDLE state = CreateFileW(unsafe, READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    assert(state != INVALID_HANDLE_VALUE);
    PSECURITY_DESCRIPTOR public_sd = NULL; PACL public_acl = NULL; BOOL present, defaulted;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &public_sd, NULL));
    assert(GetSecurityDescriptorDacl(public_sd, &present, &public_acl, &defaulted));
    assert(SetSecurityInfo(state, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
                           PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, public_acl, NULL) == ERROR_SUCCESS);
    CloseHandle(state); LocalFree(public_sd);
    preparation.snapshot.revision++;
    assert(home_service_save(&preparation));
    assert(home_service_record("prepare", &snapshot));
    home_service_close(&preparation);

    char file[1200]; wchar_t wide[1200];
    const char *roles[] = {"donor", "manager", "tracker", "prepare"};
    const char *suffixes[] = {"state", "lock"};
    for (size_t i = 0; i < 4; i++) for (size_t j = 0; j < 2; j++) {
        assert(!home_service_path(roles[i], suffixes[j], file, sizeof file));
        assert(MultiByteToWideChar(CP_UTF8, 0, file, -1, wide, 1200));
        if (!DeleteFileW(wide)) assert(GetLastError() == ERROR_FILE_NOT_FOUND);
    }
    assert(!home_service_directory(file, sizeof file));
    assert(MultiByteToWideChar(CP_UTF8, 0, file, -1, wide, 1200));
    assert(RemoveDirectoryW(wide));
    assert(swprintf(wide, 1200, L"%ls\\Lumabri", temporary) > 0 && RemoveDirectoryW(wide));
    assert(RemoveDirectoryW(temporary));
    puts("NATIVE SERVICE CONTROL: PASS (private IPC, singleton, reconnect, fences, bounded input, atomic records, manager independence, crash restart)");
    return 0;
}
