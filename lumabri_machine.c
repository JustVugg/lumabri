#define _GNU_SOURCE
#include "lumabri_machine.h"
#include "lumabri_proto.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int read_u64_file(const char *path, uint64_t *value) {
    FILE *file = fopen(path, "r");
    unsigned long long parsed = 0;
    if (!file) return -1;
    int ok = fscanf(file, "%llu", &parsed) == 1;
    fclose(file);
    if (!ok) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static uint64_t kib_to_bytes(unsigned long long kib) {
    if (kib > (unsigned long long)(UINT64_MAX >> 10)) return UINT64_MAX;
    return (uint64_t)kib << 10;
}

static unsigned long long add_kib(unsigned long long left,
                                  unsigned long long right) {
    return ULLONG_MAX - left < right ? ULLONG_MAX : left + right;
}

int lmb_machine_read_meminfo(FILE *file, uint64_t *total,
                             uint64_t *available, uint64_t *swap_total,
                             uint64_t *swap_free) {
    if (!file) return -1;
    char line[256];
    unsigned long long mt = 0, ma = 0, mf = 0, buffers = 0;
    unsigned long long cached = 0, reclaimable = 0, shmem = 0;
    unsigned long long st = 0, sf = 0;
    int have_available = 0;
    while (fgets(line, sizeof line, file)) {
        (void)sscanf(line, "MemTotal: %llu kB", &mt);
        if (sscanf(line, "MemAvailable: %llu kB", &ma) == 1)
            have_available = 1;
        (void)sscanf(line, "MemFree: %llu kB", &mf);
        (void)sscanf(line, "Buffers: %llu kB", &buffers);
        (void)sscanf(line, "Cached: %llu kB", &cached);
        (void)sscanf(line, "SReclaimable: %llu kB", &reclaimable);
        (void)sscanf(line, "Shmem: %llu kB", &shmem);
        (void)sscanf(line, "SwapTotal: %llu kB", &st);
        (void)sscanf(line, "SwapFree: %llu kB", &sf);
    }
    if (!have_available) {
        /* Linux added MemAvailable in 3.14, but WSL1's 4.4 compatibility
         * procfs omits it. Match the conservative procps fallback: genuinely
         * free pages plus reclaimable buffer/page/slab cache, excluding
         * shared-memory pages. Never claim more than MemTotal. */
        unsigned long long cache = add_kib(cached, reclaimable);
        cache = cache > shmem ? cache - shmem : 0;
        ma = add_kib(add_kib(mf, buffers), cache);
        if (mt && ma > mt) ma = mt;
    }
    if (total) *total = kib_to_bytes(mt);
    if (available) *available = kib_to_bytes(ma);
    if (swap_total) *swap_total = kib_to_bytes(st);
    if (swap_free) *swap_free = kib_to_bytes(sf);
    return mt ? 0 : -1;
}

static void meminfo(uint64_t *total, uint64_t *available,
                    uint64_t *swap_total, uint64_t *swap_free) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file || lmb_machine_read_meminfo(file, total, available,
                                          swap_total, swap_free)) {
        if (total) *total = 0;
        if (available) *available = 0;
        if (swap_total) *swap_total = 0;
        if (swap_free) *swap_free = 0;
    }
    if (file) fclose(file);
}

uint64_t lmb_machine_available_ram(void) {
    uint64_t value = 0;
    meminfo(NULL, &value, NULL, NULL);
    return value;
}

uint64_t lmb_machine_total_ram(void) {
    uint64_t value = 0;
    meminfo(&value, NULL, NULL, NULL);
    return value;
}

static void cpu_profile(LmbMachineProfile *profile) {
    FILE *file = fopen("/proc/cpuinfo", "r");
    char line[1024], flags[4096] = "";
    unsigned processors = 0;
    struct { int package, core; } seen[1024];
    unsigned nseen = 0;
    int package = -1, core = -1;
    if (file) {
        while (fgets(line, sizeof line, file)) {
            if (!strncmp(line, "processor", 9)) {
                if (package >= 0 && core >= 0 && nseen < 1024) {
                    unsigned i = 0;
                    for (; i < nseen; i++)
                        if (seen[i].package == package && seen[i].core == core) break;
                    if (i == nseen) seen[nseen++] = (typeof(*seen)){package, core};
                }
                processors++; package = core = -1;
            } else if (!strncmp(line, "physical id", 11)) {
                char *colon = strchr(line, ':'); if (colon) package = atoi(colon + 1);
            } else if (!strncmp(line, "core id", 7)) {
                char *colon = strchr(line, ':'); if (colon) core = atoi(colon + 1);
            } else if ((!strncmp(line, "model name", 10) ||
                        !strncmp(line, "Hardware", 8)) && !profile->cpu_model[0]) {
                char *colon = strchr(line, ':');
                if (colon) {
                    while (*++colon == ' ' || *colon == '\t') {}
                    colon[strcspn(colon, "\r\n")] = 0;
                    snprintf(profile->cpu_model, sizeof profile->cpu_model, "%s", colon);
                }
            } else if ((!strncmp(line, "flags", 5) || !strncmp(line, "Features", 8)) &&
                       !flags[0]) {
                char *colon = strchr(line, ':');
                if (colon) snprintf(flags, sizeof flags, " %s ", colon + 1);
            }
        }
        if (package >= 0 && core >= 0 && nseen < 1024) {
            unsigned i = 0;
            for (; i < nseen; i++)
                if (seen[i].package == package && seen[i].core == core) break;
            if (i == nseen) seen[nseen++] = (typeof(*seen)){package, core};
        }
        fclose(file);
    }
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    profile->logical_cpus = online > 0 ? (uint32_t)online : processors;
    profile->physical_cores = nseen ? nseen : profile->logical_cpus;
    const char *isa = "generic";
    if (strstr(flags, " avx512_bf16 ") || strstr(flags, " avx512bf16 ")) isa = "avx512bf16";
    else if (strstr(flags, " avx512_vnni ") || strstr(flags, " avx512vnni ")) isa = "avx512vnni";
    else if (strstr(flags, " avx512f ")) isa = "avx512f";
    else if (strstr(flags, " avx_vnni ") || strstr(flags, " avxvnni ")) isa = "avxvnni";
    else if (strstr(flags, " avx2 ")) isa = "avx2";
    else if (strstr(flags, " asimd ")) isa = "asimd";
    snprintf(profile->isa, sizeof profile->isa, "%s", isa);
}

static void numa_profile(LmbMachineProfile *profile) {
    DIR *dir = opendir("/sys/devices/system/node");
    struct dirent *entry;
    if (!dir) { profile->numa_nodes = 1; return; }
    while ((entry = readdir(dir)))
        if (!strncmp(entry->d_name, "node", 4) &&
            entry->d_name[4] >= '0' && entry->d_name[4] <= '9')
            profile->numa_nodes++;
    closedir(dir);
    if (!profile->numa_nodes) profile->numa_nodes = 1;
}

static void gpu_profile(LmbMachineProfile *profile) {
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *entry;
    if (!dir) return;
    while ((entry = readdir(dir))) {
        unsigned card = 0; char tail = 0;
        if (sscanf(entry->d_name, "card%u%c", &card, &tail) != 1) continue;
        char vendor[256], total[256], used[256];
        snprintf(vendor, sizeof vendor, "/sys/class/drm/card%u/device/vendor", card);
        uint64_t ignored = 0;
        if (read_u64_file(vendor, &ignored)) continue;
        profile->gpu_count++;
        snprintf(total, sizeof total, "/sys/class/drm/card%u/device/mem_info_vram_total", card);
        snprintf(used, sizeof used, "/sys/class/drm/card%u/device/mem_info_vram_used", card);
        uint64_t t = 0, u = 0;
        if (!read_u64_file(total, &t)) {
            (void)read_u64_file(used, &u);
            profile->vram_total_bytes += t;
            profile->vram_available_bytes += t > u ? t - u : 0;
        }
    }
    closedir(dir);
}

static int public_address(uint32_t host_order) {
    return (host_order >> 24) != 0 && (host_order >> 24) != 10 &&
           (host_order >> 24) != 127 && (host_order >> 16) != 0xa9fe &&
           (host_order >> 20) != 0xac1 && (host_order >> 16) != 0xc0a8 &&
           (host_order >> 22) != 0x0191;
}

static void network_profile(LmbMachineProfile *profile) {
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return;
    for (struct ifaddrs *it = all; it; it = it->ifa_next) {
        if (!it->ifa_addr || !(it->ifa_flags & IFF_UP) ||
            (it->ifa_flags & IFF_LOOPBACK)) continue;
        if (it->ifa_addr->sa_family == AF_INET) {
            profile->network_interfaces++;
            uint32_t ip = ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr);
            if (public_address(ip)) profile->public_ipv4 = 1;
        }
    }
    freeifaddrs(all);
}

static double monotonic_seconds(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

int lmb_machine_probe(LmbMachineProfile *profile, const char *disk_path,
                      const char *tracker) {
    if (!profile) return -1;
    memset(profile, 0, sizeof *profile);
    struct utsname system;
    if (!uname(&system)) {
        snprintf(profile->hostname, sizeof profile->hostname, "%.63s", system.nodename);
        snprintf(profile->os, sizeof profile->os, "%.20s %.42s",
                 system.sysname, system.release);
        snprintf(profile->arch, sizeof profile->arch, "%.31s", system.machine);
    } else {
        (void)gethostname(profile->hostname, sizeof profile->hostname);
    }
    cpu_profile(profile);
    numa_profile(profile);
    meminfo(&profile->ram_total_bytes, &profile->ram_available_bytes,
            &profile->swap_total_bytes, &profile->swap_free_bytes);
    gpu_profile(profile);
    network_profile(profile);
    struct statvfs disk;
    if (!statvfs(disk_path && *disk_path ? disk_path : ".", &disk))
        profile->disk_available_bytes = (uint64_t)disk.f_bavail * disk.f_frsize;
    double loads[1];
    if (getloadavg(loads, 1) == 1) profile->load_one = loads[0];
    profile->tracker_rtt_ms = -1.0;
    if (tracker && *tracker) {
        double start = monotonic_seconds();
        int fd = lmb_connect(tracker);
        if (fd >= 0) {
            profile->tracker_rtt_ms = (monotonic_seconds() - start) * 1000.0;
            close(fd);
        }
    }
    return 0;
}

static void json_string(FILE *out, const char *value) {
    fputc('"', out);
    for (; value && *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

void lmb_machine_print(FILE *out, const LmbMachineProfile *p, int json) {
    if (json) {
        fputs("{\"schema\":1,\"hostname\":", out); json_string(out, p->hostname);
        fputs(",\"os\":", out); json_string(out, p->os);
        fputs(",\"arch\":", out); json_string(out, p->arch);
        fputs(",\"cpu\":{\"model\":", out); json_string(out, p->cpu_model);
        fprintf(out, ",\"isa\":"); json_string(out, p->isa);
        fprintf(out, ",\"logical\":%u,\"physical\":%u,\"numa\":%u}",
                p->logical_cpus, p->physical_cores, p->numa_nodes);
        fprintf(out, ",\"memory\":{\"total\":%llu,\"available\":%llu,"
                "\"swap_total\":%llu,\"swap_free\":%llu}",
                (unsigned long long)p->ram_total_bytes,
                (unsigned long long)p->ram_available_bytes,
                (unsigned long long)p->swap_total_bytes,
                (unsigned long long)p->swap_free_bytes);
        fprintf(out, ",\"gpu\":{\"count\":%u,\"vram_total\":%llu,"
                "\"vram_available\":%llu}", p->gpu_count,
                (unsigned long long)p->vram_total_bytes,
                (unsigned long long)p->vram_available_bytes);
        fprintf(out, ",\"disk_available\":%llu,\"network\":{\"interfaces\":%u,"
                "\"public_ipv4\":%s,\"tracker_rtt_ms\":%.3f},\"load1\":%.3f}\n",
                (unsigned long long)p->disk_available_bytes,
                p->network_interfaces, p->public_ipv4 ? "true" : "false",
                p->tracker_rtt_ms, p->load_one);
        return;
    }
    fprintf(out, "machine %s · %s · %s\n", p->hostname, p->os, p->arch);
    fprintf(out, "CPU     %s · %u physical/%u logical · %s · %u NUMA\n",
            p->cpu_model[0] ? p->cpu_model : "unknown", p->physical_cores,
            p->logical_cpus, p->isa, p->numa_nodes);
    fprintf(out, "RAM     %.1f/%.1f GB available · swap %.1f/%.1f GB free\n",
            p->ram_available_bytes / 1e9, p->ram_total_bytes / 1e9,
            p->swap_free_bytes / 1e9, p->swap_total_bytes / 1e9);
    fprintf(out, "GPU     %u device(s) · %.1f/%.1f GB VRAM visible\n",
            p->gpu_count, p->vram_available_bytes / 1e9, p->vram_total_bytes / 1e9);
    fprintf(out, "disk    %.1f GB available · network %u interface(s) · public IPv4 %s",
            p->disk_available_bytes / 1e9, p->network_interfaces,
            p->public_ipv4 ? "yes" : "no");
    if (p->tracker_rtt_ms >= 0) fprintf(out, " · tracker %.2f ms", p->tracker_rtt_ms);
    fprintf(out, "\nload    %.2f (one minute)\n", p->load_one);
}

static int state_path(char *path, size_t cap) {
    const char *forced = getenv("LUMABRI_GOVERNOR_FILE");
    if (forced && *forced) return snprintf(path, cap, "%s", forced) >= (int)cap ? -1 : 0;
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    return snprintf(path, cap, "%s/.lumabri/governor.state", home) >= (int)cap ? -1 : 0;
}

int lmb_governor_manual_paused(void) {
    char path[1024], value[32] = "";
    if (state_path(path, sizeof path)) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    if (!fgets(value, sizeof value, file)) value[0] = 0;
    fclose(file);
    return !strncmp(value, "paused", 6);
}

int lmb_governor_set_manual(int paused) {
    char path[1024], dir[1024], temporary[1100];
    if (state_path(path, sizeof path)) return -1;
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    if (snprintf(temporary, sizeof temporary, "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof temporary) return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    const char *text = paused ? "paused\n" : "active\n";
    size_t length = strlen(text), offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, text + offset, length - offset);
        if (n <= 0) { int e = errno; close(fd); unlink(temporary); errno = e; return -1; }
        offset += (size_t)n;
    }
    int bad = fsync(fd);
    int saved = errno;
    if (close(fd) && !bad) { bad = 1; saved = errno; }
    if (!bad && rename(temporary, path)) { bad = 1; saved = errno; }
    if (bad) { unlink(temporary); errno = saved; return -1; }
    return 0;
}

const char *lmb_governor_state_name(LmbGovernorState state) {
    switch (state) {
        case LMB_GOV_ACTIVE: return "ACTIVE";
        case LMB_GOV_PRESSURE: return "PRESSURE";
        case LMB_GOV_PAUSED: return "PAUSED";
        case LMB_GOV_RECOVERY: return "RECOVERY";
    }
    return "UNKNOWN";
}

void lmb_governor_init(LmbGovernor *governor, uint64_t reserve) {
    memset(governor, 0, sizeof *governor);
    governor->ram_reserve_bytes = reserve;
    atomic_store(&governor->state,
                 lmb_governor_manual_paused() ? LMB_GOV_PAUSED : LMB_GOV_ACTIVE);
}

LmbGovernorState lmb_governor_state(const LmbGovernor *governor) {
    return (LmbGovernorState)atomic_load(&governor->state);
}

int lmb_governor_accepting(const LmbGovernor *governor) {
    return lmb_governor_state(governor) == LMB_GOV_ACTIVE;
}

LmbGovernorState lmb_governor_poll(LmbGovernor *governor) {
    LmbGovernorState previous = lmb_governor_state(governor), next;
    if (lmb_governor_manual_paused()) {
        next = LMB_GOV_PAUSED;
        governor->recovery_ticks = 0;
    } else {
        uint64_t available = 0, swap_total = 0, swap_free = 0;
        meminfo(NULL, &available, &swap_total, &swap_free);
        int critical = available && available < governor->ram_reserve_bytes / 2;
        if (swap_total && swap_free < swap_total / 20 &&
            available < governor->ram_reserve_bytes) critical = 1;
        if (critical) {
            next = LMB_GOV_PAUSED;
            governor->recovery_ticks = 0;
        } else if (available && available < governor->ram_reserve_bytes) {
            next = LMB_GOV_PRESSURE;
            governor->recovery_ticks = 0;
        } else if (previous != LMB_GOV_ACTIVE) {
            next = ++governor->recovery_ticks >= 3 ?
                   LMB_GOV_ACTIVE : LMB_GOV_RECOVERY;
        } else {
            next = LMB_GOV_ACTIVE;
        }
    }
    atomic_store(&governor->state, next);
    return next;
}
