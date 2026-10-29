/* Replaceable manager. Keepers are independently locked and authenticated
 * local services. Restarting this process does not unload their engines.
 * A dead keeper is reported as interrupted, never recreated from a journal
 * containing an old approval. */
#ifndef LUMABRI_SERVICE_MANAGER_H
#define LUMABRI_SERVICE_MANAGER_H

static const char *const home_service_roles[] = {"manager", "donor", "tracker", "prepare", "api"};

static void home_service_wait_ms(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    (void)poll(NULL, 0, (int)milliseconds);
#endif
}

/* A successful probe of the singleton lock, not an absent RPC endpoint or a
 * saved PID, is the authority to start a replacement. */
static int home_service_lock_available(const char *role) {
    char path[1200];
    if (home_service_path(role, "lock", path, sizeof path)) return -1;
#ifdef _WIN32
    HANDLE lock = lmb_private_open(path, 0, 0, NULL);
    if (lock == INVALID_HANDLE_VALUE) return errno == ENOENT ? 1 : -1;
    OVERLAPPED at = {0};
    int free_lock = LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                              0, MAXDWORD, MAXDWORD, &at) != 0;
    DWORD error = free_lock ? ERROR_SUCCESS : GetLastError();
    CloseHandle(lock);
    return free_lock ? 1 : error == ERROR_LOCK_VIOLATION ? 0 : -1;
#else
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    int rc = flock(fd, LOCK_EX | LOCK_NB), error = errno;
    close(fd);
    return !rc ? 1 : error == EWOULDBLOCK || error == EAGAIN ? 0 : -1;
#endif
}

static void home_service_reconcile(HomeService *manager) {
    unsigned live = 0, interrupted = 0;
    for (unsigned i = 1; i < sizeof home_service_roles / sizeof *home_service_roles; i++) {
        HomeServiceSnapshot observed;
        if (!home_service_query(home_service_roles[i], HOME_SVC_STATUS, NULL, &observed)) live++;
        else if (!home_service_record(home_service_roles[i], &observed) && observed.state == HOME_SVC_RUNNING) interrupted++;
    }
    snprintf(manager->snapshot.detail, sizeof manager->snapshot.detail,
        "%u live keeper(s), %u interrupted operation(s). Old approvals are never replayed.", live, interrupted);
}

static int home_service_manager_run(HomeService *manager) {
    double reconciled = -2;
    int rc = 0;
    while (!g_stopping) {
        if (nowd() - reconciled >= 2) { home_service_reconcile(manager); reconciled = nowd(); }
        if (home_service_save(manager)) { rc = 1; break; }
        int op = home_service_poll(manager);
        if (op == HOME_SVC_STOP) break;
        home_service_answer(manager);
        home_service_wait_ms(50);
    }
    manager->snapshot.state = rc ? HOME_SVC_FAILED : HOME_SVC_STOPPED;
    if (home_service_save(manager)) rc = 1;
    home_service_answer(manager); home_service_close(manager);
    return rc;
}

#ifdef _WIN32
#include "src/runtime/lumabri_manager_windows.h"
#endif

static int home_service_ensure(void) {
    if (home_service_foreground()) return 0;
    HomeServiceSnapshot existing;
    if (!home_service_query("manager", HOME_SVC_STATUS, NULL, &existing)) return 0;
#ifdef _WIN32
    return home_service_manager_start_windows();
#else
    HomeService manager;
    if (home_service_open(&manager, "manager")) {
        /* Another terminal can be starting the same singleton. */
        for (int i = 0; i < 20; i++) {
            home_service_wait_ms(50);
            if (!home_service_query("manager", HOME_SVC_STATUS, NULL, &existing)) return 0;
        }
        return -1;
    }
    snprintf(manager.snapshot.name, sizeof manager.snapshot.name, "Household manager");
    if (home_service_save(&manager)) { home_service_close(&manager); return -1; }
    int detached = home_service_detach(&manager, -1);
    if (detached < 0) { home_service_close(&manager); return -1; }
    if (detached) {
        for (int i = 0; i < 40; i++) {
            if (!home_service_query("manager", HOME_SVC_STATUS, NULL, &existing)) return 0;
            home_service_wait_ms(50);
        }
        return -1;
    }
    _exit(home_service_manager_run(&manager));
#endif
}

static int home_service_stop_role(const char *role) {
    HomeServiceSnapshot before, after;
    if (home_service_query(role, HOME_SVC_STATUS, NULL, &before)) {
        /* No socket is not proof of no live keeper. Consult its lock. */
        return home_service_lock_available(role) == 1 ? 0 : -1;
    }
    int rc = home_service_query(role, HOME_SVC_STOP, &before, &after);
    /* Cleanup can take longer than one RPC deadline. Do not kill a saved PID
     * or start its replacement until the real singleton lock is released. */
    for (int i = 0; i < 60; i++) {
        int available = home_service_lock_available(role);
        if (available == 1) return 0;
        if (available < 0) return -1;
        home_service_wait_ms(100);
    }
    (void)rc; return -1;
}

static int cmd_service(int argc, char **argv) {
    const char *action = argc ? argv[0] : "status";
    if (argc==2 && !strcmp(action,"live")) {
        HomeServiceSnapshot s;
        if (!home_service_role(argv[1])) return 2;
        /* Supervisor probe only. A live keeper is not model readiness. */
        return home_service_query(argv[1],HOME_SVC_STATUS,NULL,&s) || s.state!=HOME_SVC_RUNNING ||
            (!strcmp(argv[1],"donor") && !s.compute_enabled) ? 1 : 0;
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json"))) return 2;
    if (!strcmp(action, "start")) return home_service_ensure() ? 1 : 0;
    if (!strcmp(action, "restart")) {
        if (home_service_stop_role("manager")) return home_fail("Manager did not stop. Existing keepers were not changed.");
        return home_service_ensure() ? 1 : 0;
    }
    if (!strcmp(action, "stop")) {
        int rc = 0;
        const char *roles[] = {"prepare", "api", "donor", "tracker", "manager"};
        for (unsigned i = 0; i < sizeof roles / sizeof *roles; i++)
            if (home_service_stop_role(roles[i])) { fprintf(stderr, "Cannot confirm %s stopped.\n", roles[i]); rc = 1; }
        return rc;
    }
    if (strcmp(action, "status")) return 2;
    int json = argc == 2;
    if (json) fputs("{\"schema\":1,\"services\":[", stdout);
    for (unsigned i = 0; i < sizeof home_service_roles / sizeof *home_service_roles; i++) {
        HomeServiceSnapshot s = {0};
        int live = !home_service_query(home_service_roles[i], HOME_SVC_STATUS, NULL, &s);
        int recorded = live || !home_service_record(home_service_roles[i], &s);
        if (!recorded) memset(&s, 0, sizeof s); /* reject partially decoded records */
        const char *state = live ? "live" : recorded && s.state == HOME_SVC_RUNNING ? "interrupted" : "stopped";
        if (json) {
            char instance[65]; lmb_hex(instance, s.instance, 32);
            printf("%s{\"role\":\"%s\",\"state\":\"%s\",\"live\":%s,\"instance\":\"%s\","
                   "\"pid\":%llu,\"segment_pid\":%llu,\"host_pid\":%llu,\"phase\":%u,\"reserved_bytes\":%llu,\"model\":",
                i ? "," : "", home_service_roles[i], state, live ? "true" : "false", instance,
                (unsigned long long)s.pid, (unsigned long long)s.segment_pid, (unsigned long long)s.host_pid,
                s.phase, (unsigned long long)(live && s.has_offer && s.phase >= LMB_HOME_ACCEPTED && s.phase <= LMB_HOME_READY ? s.offer.ram_bytes : 0));
            doctor_json_string(s.has_offer ? s.offer.model : "");
            printf(",\"model_count\":%u,\"reserved_total_bytes\":%llu", live ? s.model_count : 0,
                   (unsigned long long)(live ? s.reserved_total : 0));
            printf(",\"compute\":{\"enabled\":%s,\"active\":%u,\"queued\":%u,\"grants\":%llu}",
                live && s.compute_enabled ? "true" : "false", live ? s.compute_active : 0,
                live ? s.compute_queued : 0, (unsigned long long)(live ? s.compute_grants : 0));
            fputs(",\"detail\":", stdout);
            doctor_json_string(s.detail);
            fputs(",\"operation_state\":", stdout);
            doctor_json_string(!recorded ? "unknown" : s.state == HOME_SVC_RUNNING ? "running" :
                s.state == HOME_SVC_DONE ? "done" : s.state == HOME_SVC_FAILED ? "failed" : "stopped");
            fputc('}', stdout);
        } else printf("%-8s %-12s %s%s\n", home_service_roles[i], state,
            live && s.has_offer ? s.offer.model : "", recorded && !live ? " (saved state is not a live allocation)" : "");
    }
    if (json) puts("]}");
    return 0;
}
#endif
