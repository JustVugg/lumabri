/* Native Windows filesystem and TCP acceptance; never touches installed keys. */
#include "lumabri_secure.h"
#include <assert.h>
#include <winioctl.h>

static void utf8(char *out, size_t cap, const wchar_t *wide) {
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                              out, (int)cap, NULL, NULL));
}
static void file_text(const char *path, const char *text) {
    FILE *fp = lmb_private_stream(path, 1, 1); assert(fp);
    assert(!_chsize(_fileno(fp), 0));
    assert(fputs(text, fp) >= 0 && !lmb_private_stream_flush(fp));
    lmb_private_stream_close(fp);
}

static void process_race(const wchar_t *path) {
    enum { N = 4 }; PROCESS_INFORMATION children[N]; HANDLE pipes[N];
    wchar_t exe[1024]; assert(GetModuleFileNameW(NULL, exe, 1024));
    for (int i = 0; i < N; i++) {
        HANDLE write; SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
        assert(CreatePipe(&pipes[i], &write, &sa, 0));
        assert(SetHandleInformation(pipes[i], HANDLE_FLAG_INHERIT, 0));
        STARTUPINFOW startup = {0}; startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write; startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        wchar_t command[2200];
        assert(swprintf(command, 2200, L"\"%ls\" --identity \"%ls\"", exe, path) > 0);
        assert(CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_SUSPENDED,
                              NULL, NULL, &startup, &children[i]));
        CloseHandle(write);
    }
    for (int i = 0; i < N; i++) { assert(ResumeThread(children[i].hThread) != MAXDWORD); CloseHandle(children[i].hThread); }
    char expected[65] = {0};
    for (int i = 0; i < N; i++) {
        assert(WaitForSingleObject(children[i].hProcess, 15000) == WAIT_OBJECT_0);
        DWORD code = 1; assert(GetExitCodeProcess(children[i].hProcess, &code) && !code);
        CloseHandle(children[i].hProcess);
        char actual[65] = {0}; DWORD got = 0;
        assert(ReadFile(pipes[i], actual, 64, &got, NULL) && got == 64);
        CloseHandle(pipes[i]);
        if (!i) memcpy(expected, actual, sizeof expected);
        else assert(!memcmp(expected, actual, 64));
    }
}

typedef struct { LmbSocket listener; int refused; } Echo;
static void *echo(void *arg) {
    Echo *e = arg;
    LmbSocket fd = lmb_socket_accept(e->listener, NULL, NULL); assert(fd != LMB_INVALID_SOCKET);
    assert(!lmb_secure_server(fd)); assert(lmb_secure_peer_matches(fd, g_sec_pk));
    LmbMsg m = {0}; int rc = lmb_recv(fd, &m);
    if (e->refused) assert(rc != 0);
    else {
        assert(!rc && m.op == LMB_PING);
        assert(!lmb_send(fd, LMB_OK, m.body, m.body_len, NULL, 0));
        lmb_msg_free(&m);
    }
    lmb_close(fd); return NULL;
}

static void network_test(const char *identity, const char *known) {
    assert(!_putenv_s("LUMABRI_PEER_KEY", identity));
    assert(!_putenv_s("LUMABRI_KNOWN_HOSTS", known));
    assert(!_putenv_s("LUMABRI_ENCRYPT", "1"));
    assert(!lmb_secure_init());
    Echo e = {lmb_listen(0), 0}; assert(e.listener != LMB_INVALID_SOCKET);
    struct sockaddr_in address; LmbSocketLength size = sizeof address;
    assert(!getsockname(e.listener, (struct sockaddr *)&address, &size));
    char endpoint[64]; snprintf(endpoint, sizeof endpoint, "127.0.0.1:%u", ntohs(address.sin_port));
    for (int pass = 0; pass < 3; pass++) {
        e.refused = pass == 2;
        if (e.refused) {
            uint8_t different[32]; memset(different, 42, sizeof different);
            char key[65], line[160]; lmb_hex(key, different, 32);
            snprintf(line, sizeof line, "%s %s\n", endpoint, key); file_text(known, line);
        }
        pthread_t worker; assert(!pthread_create(&worker, NULL, echo, &e));
        LmbSocket fd = lmb_connect_ms_io(endpoint, 2000, 2000);
        if (e.refused) assert(fd == LMB_INVALID_SOCKET);
        else {
            assert(fd != LMB_INVALID_SOCKET && lmb_secure_peer_matches(fd, g_sec_pk));
            assert(!lmb_send(fd, LMB_PING, "secret", 6, NULL, 0));
            LmbMsg m = {0}; assert(!lmb_recv(fd, &m));
            assert(m.op == LMB_OK && m.body_len == 6 && !memcmp(m.body, "secret", 6));
            lmb_msg_free(&m); lmb_close(fd);
        }
        assert(!pthread_join(worker, NULL)); assert(!g_sec_count);
    }
    lmb_close(e.listener);
}

static void wide_registry_test(void) {
    LmbSocket a = (LmbSocket)UINT64_C(0x100000004), b = (LmbSocket)UINT64_C(0x200000004);
    while (lmb_sec_bucket(b) != lmb_sec_bucket(a)) b++;
    LmbSocket handles[] = {a, b};
    for (unsigned i = 0; i < 2; i++) {
        LmbSecEntry *e = calloc(1, sizeof *e); assert(e);
        assert(!pthread_mutex_init(&e->tx_lk, NULL) && !pthread_mutex_init(&e->rx_lk, NULL));
        e->fd = handles[i]; size_t bucket = lmb_sec_bucket(e->fd);
        e->next = g_sec_reg[bucket]; g_sec_reg[bucket] = e; g_sec_count++;
    }
    LmbSecEntry *held = lmb_sec_acquire(a); assert(held && held->fd == a);
    lmb_sec_forget_hook(a); assert(!lmb_sec_acquire(a) && held->closing);
    LmbSecEntry *other = lmb_sec_acquire(b); assert(other && other->fd == b);
    lmb_sec_release(held); lmb_sec_release(other); lmb_sec_forget_hook(b); assert(!g_sec_count);
}

static void reparse_test(const wchar_t *root) {
    wchar_t alias[700], key[750]; char utf[2200];
    assert(swprintf(alias, 700, L"%ls\\junction", root) > 0); utf8(utf, sizeof utf, alias);
    assert(!lmb_private_mkdir(utf));
    /* Mount-point reparse layout from the Windows filesystem interface. */
    typedef struct {
        DWORD tag; WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t names[1500];
    } TestMountPoint;
    TestMountPoint record = {0};
    _Static_assert(offsetof(TestMountPoint, names) == 16, "mount-point layout");
    record.tag = IO_REPARSE_TAG_MOUNT_POINT;
    int substitute = swprintf(record.names, 1500, L"\\??\\%ls", root); assert(substitute > 0);
    size_t visible = wcslen(root);
    memcpy(record.names + substitute + 1, root, (visible + 1) * sizeof(wchar_t));
    record.substitute_length = (WORD)(substitute * sizeof(wchar_t));
    record.print_offset = record.substitute_length + sizeof(wchar_t);
    record.print_length = (WORD)(visible * sizeof(wchar_t));
    record.length = 8 + record.print_offset + record.print_length + sizeof(wchar_t);
    HANDLE h = CreateFileW(alias, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    assert(h != INVALID_HANDLE_VALUE); DWORD returned = 0;
    assert(DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &record, 8 + record.length,
                           NULL, 0, &returned, NULL));
    CloseHandle(h);
    assert(swprintf(key, 750, L"%ls\\peer.key", alias) > 0); utf8(utf, sizeof utf, key);
    uint8_t sk[64], pk[32]; assert(lmb_peer_identity(utf, sk, pk) && errno == EACCES);
    assert(RemoveDirectoryW(alias));
}

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 3 && !wcscmp(argv[1], L"--identity")) {
        char path[2048], hex[129]; uint8_t sk[64], pk[32]; utf8(path, sizeof path, argv[2]);
        if (lmb_peer_identity(path, sk, pk)) return 1;
        lmb_hex(hex, pk, 32); fputs(hex, stdout); fflush(stdout);
        SecureZeroMemory(sk, sizeof sk); return 0;
    }
    wchar_t base[512], root[640], key_w[700], link_w[700];
    assert(GetTempPathW(512, base));
    assert(swprintf(root, 640, L"%lslumabri-identity-%lu-\x00e8", base, GetCurrentProcessId()) > 0);
    char dir[2048], key[2048], pins[2048], known[2048]; utf8(dir, sizeof dir, root);
    assert(!lmb_private_mkdir(dir));
    assert(swprintf(key_w, 700, L"%ls\\peer.key", root) > 0); utf8(key, sizeof key, key_w);
    process_race(key_w);
    uint8_t sk[64], pk[32], saved[32];
    assert(!lmb_peer_identity(key, sk, pk)); memcpy(saved, pk, 32);
    assert(!lmb_peer_identity(key, sk, pk) && !memcmp(saved, pk, 32));
    char alternate[2100]; snprintf(alternate, sizeof alternate, "%s:hidden", key);
    assert(lmb_peer_identity(alternate, sk, pk) && errno == EINVAL);
    assert(!lmb_peer_identity(key, sk, pk) && !memcmp(saved, pk, 32));
    HANDLE h = lmb_private_open(key, 0, 0, NULL); DWORD flags = 0;
    assert(h != INVALID_HANDLE_VALUE && GetHandleInformation(h, &flags) && !(flags & HANDLE_FLAG_INHERIT)); CloseHandle(h);
    snprintf(pins, sizeof pins, "%s\\pins", dir); snprintf(known, sizeof known, "%s\\known_hosts", dir);
    assert(!lmb_sec_pin_file(pins, "localhost:1234", pk, 1));
    assert(!lmb_sec_pin_file(pins, "localhost:1234", pk, 0));
    pk[0] ^= 1; assert(lmb_sec_pin_file(pins, "localhost:1234", pk, 1)); pk[0] ^= 1;
    assert(lmb_sec_pin_file(pins, "localhost:9999", pk, 0));
    file_text(pins, "localhost:1234 a\n"); assert(lmb_sec_pin_file(pins, "localhost:1234", pk, 0));
    network_test(key, known); wide_registry_test(); reparse_test(root);

    /* Alternate paths to the same credential are refused. */
    assert(swprintf(link_w, 700, L"%ls\\alias", root) > 0);
    assert(CreateHardLinkW(link_w, key_w, NULL));
    assert(lmb_peer_identity(key, sk, pk)); assert(DeleteFileW(link_w));
    assert(!lmb_peer_identity(key, sk, pk) && !memcmp(saved, pk, 32));

    /* Protected DACL with Everyone read access is still not a private key. */
    PSECURITY_DESCRIPTOR sd = NULL; PACL acl = NULL; BOOL present, defaulted;
    LmbPrivateSecurity security; assert(!lmb_private_security(&security));
    wchar_t *owner_sid = NULL, permissive[512];
    assert(ConvertSidToStringSidW(security.user->User.Sid, &owner_sid));
    assert(swprintf(permissive, 512, L"D:P(A;;FA;;;%ls)(A;;FA;;;SY)(A;;GR;;;WD)", owner_sid) > 0);
    LocalFree(owner_sid);
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        permissive, SDDL_REVISION_1, &sd, NULL));
    assert(GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted));
    h = CreateFileW(key_w, READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    DWORD changed = SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, acl, NULL);
    if (changed) fprintf(stderr, "Changing disposable ACL failed: %lu\n", changed);
    assert(changed == ERROR_SUCCESS);
    CloseHandle(h); LocalFree(sd);
    assert(lmb_peer_identity(key, sk, pk) && errno == EACCES);
    /* Restore only this disposable test file so corruption can be tested. */
    assert(GetSecurityDescriptorDacl(security.descriptor, &present, &acl, &defaulted));
    h = CreateFileW(key_w, READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    assert(SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           NULL, NULL, acl, NULL) == ERROR_SUCCESS);
    CloseHandle(h); lmb_private_security_free(&security);
    file_text(key, "broken\n"); assert(lmb_peer_identity(key, sk, pk));
    FILE *fp = lmb_private_stream(key, 0, 0); char text[16] = {0}; assert(fp);
    assert(fread(text, 1, 7, fp) == 7 && !strcmp(text, "broken\n")); lmb_private_stream_close(fp);

    wchar_t path[700];
    const wchar_t *names[] = {L"peer.key", L"pins", L"known_hosts"};
    for (unsigned i = 0; i < 3; i++) { assert(swprintf(path, 700, L"%ls\\%ls", root, names[i]) > 0); assert(DeleteFileW(path)); }
    assert(RemoveDirectoryW(root));
    puts("NATIVE PEER IDENTITY: PASS (concurrent processes, durable key, Unicode path, private ACL, hardlink/reparse refusal, corrupt-key preservation, TOFU and encrypted hooks)");
    return 0;
}
