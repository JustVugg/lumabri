/* Native entrypoint. Add commands here only when their complete path is
 * available; unsupported household operations must not fall back to WSL. */
#ifndef LUMABRI_WINDOWS_MAIN_H
#define LUMABRI_WINDOWS_MAIN_H
#include "src/runtime/lumabri_service.h"
#include "lumabri_machine.h"
#include <signal.h>

static volatile sig_atomic_t g_stopping;
static double nowd(void) { return (double)GetTickCount64() / 1000.; }
static int home_fail(const char *message) { fprintf(stderr, "%s\n", message); return 1; }
static void doctor_json_string(const char *value) {
    fputc('"', stdout);
    for (; value && *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else fputc(c, stdout);
    }
    fputc('"', stdout);
}
#include "src/runtime/lumabri_service_manager.h"

static int lmb_windows_main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--internal-manager"))
        return home_service_manager_entry_windows();
    if (argc >= 2 && !strcmp(argv[1], "service")) {
        if (argc >= 3 && home_service_foreground() &&
            (!strcmp(argv[2], "start") || !strcmp(argv[2], "restart")))
            return home_fail("Remove LUMABRI_HOME_FOREGROUND before starting the native background manager.");
        return cmd_service(argc - 2, argv + 2);
    }
    if (argc == 3 && !strcmp(argv[1], "machine") && !strcmp(argv[2], "--json")) {
        LmbMachineProfile machine;
        if (lmb_machine_probe(&machine, ".", NULL)) return 1;
        lmb_machine_print(stdout, &machine, 1);
        return 0;
    }
    fputs("Lumabri native Windows control\n\n"
          "  lumabri service [start|status|restart|stop] [--json]\n"
          "  lumabri service live ROLE\n"
          "  lumabri machine --json\n\n"
          "Household TUI, donor and model preparation are not connected in this native build yet.\n", stderr);
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}

int wmain(int argc, wchar_t **wide) {
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return 1;
    int rc = 1;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(argv[i] = malloc((size_t)n)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], n, NULL, NULL)) goto done;
    }
    rc = lmb_windows_main(argc, argv);
done:
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv); return rc;
}
#endif
