#define WIN32_LEAN_AND_MEAN
#include "lumabri_proto.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <io.h>
#include "lumabri_machine.h"

int main(void) {
    wchar_t temp[MAX_PATH], root[MAX_PATH], child[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp); assert(n && n < MAX_PATH);
    assert(swprintf(root, MAX_PATH, L"%lslumabri-machine-%lu-%llu-\u00e9", temp,
        GetCurrentProcessId(), (unsigned long long)GetTickCount64()) > 0);
    assert(CreateDirectoryW(root, NULL));
    /* Only this disposable process uses this private configuration root. */
    assert(!_wputenv_s(L"LOCALAPPDATA", root));
    assert(!_putenv_s("LUMABRI_GOVERNOR_FILE", ""));
    assert(!_putenv_s("LUMABRI_NO_DISK_PROBE", "1"));
    char utf8[1024];
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root, -1,
        utf8, sizeof utf8, NULL, NULL));
    LmbMachineProfile profile;
    assert(!lmb_machine_probe(&profile, utf8, NULL));
    assert(!strcmp(profile.os, "Windows") && profile.hostname[0] && profile.cpu_model[0]);
    assert(profile.logical_cpus && profile.physical_cores && profile.numa_nodes);
    assert(profile.physical_cores <= profile.logical_cpus);
    MEMORYSTATUSEX memory = {.dwLength = sizeof memory}; assert(GlobalMemoryStatusEx(&memory));
    assert(profile.ram_total_bytes && profile.ram_total_bytes <= memory.ullTotalPhys);
    assert(profile.ram_available_bytes && profile.ram_available_bytes <= profile.ram_total_bytes);
    assert(profile.disk_available_bytes && !profile.disk_read_bps && !profile.lan_bps);
    assert(!profile.gpu_backends && !profile.vram_available_bytes && profile.load_one == -1.0);
    lmb_machine_print(stdout, &profile, 1);
    LmbSocket listener = lmb_socket_open(AF_INET, SOCK_STREAM, 0);
    assert(listener != LMB_INVALID_SOCKET);
    struct sockaddr_in endpoint = {0}; endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(listener, (struct sockaddr *)&endpoint, sizeof endpoint) && !listen(listener, 1));
    LmbSocketLength len = sizeof endpoint;
    assert(!getsockname(listener, (struct sockaddr *)&endpoint, &len));
    char address[64]; snprintf(address, sizeof address, "127.0.0.1:%u", ntohs(endpoint.sin_port));
    assert(!lmb_machine_probe(&profile, utf8, address) && profile.tracker_rtt_ms > 0);
    LmbSocket peer = accept(listener, NULL, NULL); assert(peer != LMB_INVALID_SOCKET);
    DWORD timeout = 2000;
    assert(!setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout));
    char byte; assert(recv(peer, &byte, 1, 0) == 0); /* actual Winsock close, not CRT close */
    lmb_close(peer); lmb_close(listener);

    DWORD_PTR original, system;
    assert(GetProcessAffinityMask(GetCurrentProcess(), &original, &system) && original);
    DWORD_PTR single = original & (~original+1);
    assert(SetProcessAffinityMask(GetCurrentProcess(), single));
    lmb_machine_refresh_resources(&profile, utf8);
    assert(profile.logical_cpus == 1 && profile.physical_cores == 1);
    assert(SetProcessAffinityMask(GetCurrentProcess(), original));
    assert(!_putenv_s("LUMABRI_RAM_RESERVE_MB", "3072"));
    assert(lmb_machine_ram_reserve() == (UINT64_C(3072)<<20));
    assert(!_putenv_s("LUMABRI_RAM_RESERVE_MB", ""));
    assert(!lmb_governor_set_manual(0));
    LmbGovernor governor; lmb_governor_init(&governor, 1);
    assert(lmb_governor_poll(&governor) == LMB_GOV_ACTIVE);
    assert(!lmb_governor_set_manual(1));
    assert(lmb_governor_poll(&governor) == LMB_GOV_PAUSED);
    assert(!lmb_governor_accepting(&governor) && !lmb_governor_abort_inflight(&governor));
    assert(!lmb_governor_set_manual(0));
    assert(lmb_governor_poll(&governor) == LMB_GOV_RECOVERY);
    assert(lmb_governor_poll(&governor) == LMB_GOV_RECOVERY);
    assert(lmb_governor_poll(&governor) == LMB_GOV_ACTIVE);
    lmb_governor_init(&governor, UINT64_MAX);
    assert(lmb_governor_poll(&governor) == LMB_GOV_PAUSED);
    assert(lmb_governor_abort_inflight(&governor));

    char owner[128];
    int lease = lmb_machine_compute_lease_acquire("fixture", "local", owner, sizeof owner);
    assert(lease >= 0);
    assert(lmb_machine_compute_lease_acquire("other", "local", owner, sizeof owner) < 0 && owner[0]);
    close(lease);
    lease = lmb_machine_compute_lease_acquire("second", "local", owner, sizeof owner);
    assert(lease >= 0); close(lease);
    assert(swprintf(child, MAX_PATH, L"%ls\\Lumabri\\compute-donor.lock", root) > 0);
    assert(DeleteFileW(child));
    assert(swprintf(child, MAX_PATH, L"%ls\\Lumabri\\governor.state", root) > 0);
    FILE *bad = _wfopen(child, L"wb"); assert(bad); fputs("broken", bad); fclose(bad);
    assert(lmb_governor_manual_paused());
    assert(DeleteFileW(child));
    assert(swprintf(child, MAX_PATH, L"%ls\\Lumabri", root) > 0); assert(RemoveDirectoryW(child));
    assert(!_putenv_s("LUMABRI_NO_DISK_PROBE", "0"));
    assert(!lmb_machine_probe(&profile, utf8, NULL));
    printf("Native disk probe: %llu bytes/s (zero means unavailable)\n",
        (unsigned long long)profile.disk_read_bps);
    assert(RemoveDirectoryW(root)); /* probe cannot leave a file behind */

    /* Apply a disposable process limit, then check that the inventory cannot
     * advertise the host's entire RAM from inside the constrained process. */
    HANDLE job = CreateJobObjectW(NULL, NULL); assert(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {0};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = 256u<<20;
    assert(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit));
    assert(AssignProcessToJobObject(job, GetCurrentProcess()));
    uint64_t total = lmb_machine_total_ram(), available = lmb_machine_available_ram();
    assert(total > 0 && total <= (256u<<20) && available <= total);
    void *held = VirtualAlloc(NULL, 16u<<20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); assert(held);
    uint64_t reduced = lmb_machine_available_ram();
    assert(reduced+(16u<<20) <= available);
    assert(VirtualFree(held, 0, MEM_RELEASE));
    assert(lmb_machine_available_ram() >= reduced+(16u<<20));
    CloseHandle(job);
    puts("WINDOWS MACHINE PASS: native inventory, affinity, commit limit, TCP probe/close, private lease, pause/drain/recovery and temporary disk probe");
    return 0;
}
