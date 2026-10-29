/* Native inventory feeding the same planner and governor as Unix. Hardware
 * detection never asserts that a Colibri GPU backend is operational. */
#ifndef LMB_MACHINE_WINDOWS_H
#define LMB_MACHINE_WINDOWS_H
#include <iphlpapi.h>
#include <psapi.h>
#define COBJMACROS
#include <dxgi.h>
#include "lumabri_private_file_windows.h"

static wchar_t *lmb_machine_wide(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *w = malloc((size_t)n*sizeof *w);
    if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, n)) {
        free(w); w = NULL;
    }
    return w;
}
static uint64_t lmb_machine_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

typedef struct { uint64_t total, free; } LmbWindowsSwap;
static BOOL CALLBACK lmb_machine_pagefile(void *arg, PENUM_PAGE_FILE_INFORMATION p,
                                         LPCWSTR name) {
    (void)name; LmbWindowsSwap *s = arg;
    s->total += p->TotalSize;
    if (p->TotalSize >= p->TotalInUse) s->free += p->TotalSize-p->TotalInUse;
    return TRUE;
}
static void lmb_machine_win_memory(uint64_t *total, uint64_t *available,
                                    uint64_t *swap_total, uint64_t *swap_free) {
    MEMORYSTATUSEX mem = {.dwLength = sizeof mem};
    uint64_t mt = 0, ma = 0;
    if (GlobalMemoryStatusEx(&mem)) {
        /* PageFile fields are commit limits, not pagefile capacity. They bound
         * allocatable physical RAM; they are never added to resident memory. */
        mt = lmb_machine_min(mem.ullTotalPhys, mem.ullTotalPageFile);
        ma = lmb_machine_min(mem.ullAvailPhys, mem.ullAvailPageFile);
        ma = lmb_machine_min(ma, mt);
    }
    BOOL in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in_job)) ma = 0;
    else if (in_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {0};
        if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation,
                &limit, sizeof limit, NULL)) ma = 0;
        else {
            DWORD flags = limit.BasicLimitInformation.LimitFlags;
            if (flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) {
                PROCESS_MEMORY_COUNTERS_EX process = {.cb = sizeof process};
                mt = lmb_machine_min(mt, limit.ProcessMemoryLimit);
                if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&process,
                                           sizeof process)) ma = 0;
                else ma = lmb_machine_min(ma, limit.ProcessMemoryLimit > process.PrivateUsage ?
                    limit.ProcessMemoryLimit-process.PrivateUsage : 0);
            }
            if (flags & JOB_OBJECT_LIMIT_JOB_MEMORY) {
                mt = lmb_machine_min(mt, limit.JobMemoryLimit);
                /* The supported limit snapshot exposes the job's peak, not
                 * current shared commit. This is a conservative lower bound
                 * on free capacity, never treat the whole limit as free RAM. */
                ma = lmb_machine_min(ma, limit.JobMemoryLimit > limit.PeakJobMemoryUsed ?
                    limit.JobMemoryLimit-limit.PeakJobMemoryUsed : 0);
            }
        }
    }
    ma = lmb_machine_min(ma, mt);
    if (total) *total = mt;
    if (available) *available = ma;
    if (swap_total || swap_free) {
        LmbWindowsSwap swap = {0}; SYSTEM_INFO sys; GetSystemInfo(&sys);
        if (!EnumPageFilesW(lmb_machine_pagefile, &swap)) memset(&swap, 0, sizeof swap);
        if (swap_total) *swap_total = swap.total*sys.dwPageSize;
        if (swap_free) *swap_free = swap.free*sys.dwPageSize;
    }
}

static unsigned lmb_machine_bits(ULONG_PTR bits) {
    unsigned n = 0; while (bits) { bits &= bits-1; n++; } return n;
}
static void lmb_machine_win_cpu(LmbMachineProfile *p) {
    SYSTEM_INFO sys; GetNativeSystemInfo(&sys);
    snprintf(p->arch, sizeof p->arch, "%s", sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ?
        "x86_64" : sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "unknown");
    DWORD_PTR allowed = 0, system = 0;
    GROUP_AFFINITY group = {0};
    int known = GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) && allowed &&
        GetThreadGroupAffinity(GetCurrentThread(), &group);
    p->logical_cpus = known ? lmb_machine_bits(allowed) : 1;
    p->physical_cores = 0;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &bytes);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info = bytes && bytes < (1u<<20) ? malloc(bytes) : NULL;
    if (known && info && GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) {
        DWORD offset = 0;
        while (offset < bytes) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *core = (void *)((char *)info+offset);
            size_t masks = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor.GroupMask);
            if (core->Size < masks || core->Size > bytes-offset ||
                core->Processor.GroupCount > (core->Size-masks)/sizeof(GROUP_AFFINITY)) break;
            for (WORD i=0; i<core->Processor.GroupCount; i++)
                if (core->Processor.GroupMask[i].Group == group.Group &&
                    (core->Processor.GroupMask[i].Mask & allowed)) { p->physical_cores++; break; }
            offset += core->Size;
        }
    }
    free(info);
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate = {0};
    DWORD maximum = 0;
    if (QueryInformationJobObject(NULL, JobObjectCpuRateControlInformation, &rate, sizeof rate, NULL) &&
        (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE)) {
        if (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) maximum = rate.CpuRate;
        else if (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_MIN_MAX_RATE)
            maximum = HIWORD(rate.CpuRate); /* MinRate/MaxRate share the DWORD union. */
    }
    if (maximum && maximum <= 10000) {
        uint32_t capped = (uint32_t)(((uint64_t)p->logical_cpus*maximum+9999)/10000);
        if (p->logical_cpus > capped) p->logical_cpus = capped;
    }
    if (!p->physical_cores) p->physical_cores = 1;
    if (p->physical_cores > p->logical_cpus) p->physical_cores = p->logical_cpus;
    wchar_t model[128] = {0}; bytes = sizeof model;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString", RRF_RT_REG_SZ, NULL, model, &bytes) == ERROR_SUCCESS)
        (void)WideCharToMultiByte(CP_UTF8, 0, model, -1, p->cpu_model, sizeof p->cpu_model, NULL, NULL);
    const char *isa = IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE) ? "avx512f" :
        IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) ? "avx2" :
        IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE) ? "asimd" : "generic";
    snprintf(p->isa, sizeof p->isa, "%s", isa);
    USHORT nodes[64]; unsigned count = 0;
    if (known) for (BYTE i=0; i<sizeof allowed*8; i++) if (allowed & ((ULONG_PTR)1<<i)) {
        PROCESSOR_NUMBER processor = {group.Group, i, 0}; USHORT node;
        if (GetNumaProcessorNodeEx(&processor, &node) && node != 0xffff) {
            unsigned j = 0; while (j<count && nodes[j] != node) j++;
            if (j == count) nodes[count++] = node;
        }
    }
    p->numa_nodes = count ? count : 1;
}

static void lmb_machine_win_gpu(LmbMachineProfile *p) {
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return;
    for (UINT i=0; i<64; i++) {
        IDXGIAdapter1 *adapter = NULL;
        if (FAILED(IDXGIFactory1_EnumAdapters1(factory, i, &adapter))) break;
        DXGI_ADAPTER_DESC1 info;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &info)) && !(info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            p->gpu_count++; p->vram_total_bytes += info.DedicatedVideoMemory;
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    /* Shared system RAM is not extra VRAM. Free VRAM/backend capacity requires
     * a successful engine/backend probe; detection leaves both unclaimed. */
}

static uint64_t lmb_machine_win_disk_free(const char *path) {
    wchar_t *w = lmb_machine_wide(path && *path ? path : ".");
    ULARGE_INTEGER space = {0};
    if (w) (void)GetDiskFreeSpaceExW(w, &space, NULL, NULL);
    free(w); return space.QuadPart;
}
static uint64_t lmb_machine_win_disk_speed(const char *path) {
    wchar_t *root = lmb_machine_wide(path && *path ? path : ".");
    if (!root) return 0;
    wchar_t name[2048];
    int n = swprintf(name, sizeof name/sizeof *name, L"%ls\\.lumabri-probe-%lu-%llu",
        root, GetCurrentProcessId(), (unsigned long long)GetTickCount64()); free(root);
    if (n < 0) return 0;
    HANDLE file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_NO_BUFFERING |
        FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    enum { CHUNK = 1<<20, ROUNDS = 8 };
    void *data = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint64_t speed = 0; DWORD got;
    if (!data) goto done;
    memset(data, 0xa5, CHUNK);
    for (int i=0; i<ROUNDS; i++)
        if (!WriteFile(file, data, CHUNK, &got, NULL) || got != CHUNK) goto done;
    LARGE_INTEGER zero = {0}, start, end, freq;
    if (!FlushFileBuffers(file) || !SetFilePointerEx(file, zero, NULL, FILE_BEGIN) ||
        !QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&start)) goto done;
    for (int i=0; i<ROUNDS; i++)
        if (!ReadFile(file, data, CHUNK, &got, NULL) || got != CHUNK) goto done;
    if (QueryPerformanceCounter(&end) && end.QuadPart > start.QuadPart)
        speed = (uint64_t)((double)(CHUNK*ROUNDS)*freq.QuadPart/(end.QuadPart-start.QuadPart));
done:
    if (data) VirtualFree(data, 0, MEM_RELEASE);
    CloseHandle(file); return speed;
}

static int public_address(uint32_t host_order);
static void lmb_machine_win_network(LmbMachineProfile *p) {
    ULONG bytes = 15000; IP_ADAPTER_ADDRESSES *all = NULL;
    for (unsigned retry=0; retry<3 && bytes <= (1u<<20); retry++) {
        free(all); all = malloc(bytes); if (!all) return;
        ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, NULL, all, &bytes);
        if (rc == ERROR_BUFFER_OVERFLOW) continue;
        if (!rc) for (IP_ADAPTER_ADDRESSES *a=all; a; a=a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            int found = 0;
            for (IP_ADAPTER_UNICAST_ADDRESS *u=a->FirstUnicastAddress; u; u=u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                found = 1;
                uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                if (public_address(ip)) p->public_ipv4 = 1;
            }
            if (found) p->network_interfaces++;
        }
        break;
    }
    free(all);
}
#endif
