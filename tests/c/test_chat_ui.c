/* Exercise the actual CLI editor and streaming renderer, without an engine. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    LmbExecutionView execution = { .count = 2, .layers = 4 };
    for (uint32_t i = 0; i < 2; i++) {
        snprintf(execution.nodes[i].name, 64, "donor-%u", i);
        snprintf(execution.nodes[i].address, 64, "127.0.0.1:%u", 47301 + i);
        execution.nodes[i].begin = i * 2; execution.nodes[i].end = i * 2 + 2;
        execution.nodes[i].reserved_bytes = 128u << 20;
    }
    execution.nodes[0].edge = 1;
    assert(lmb_execution_valid(&execution));
    execution.nodes[1].begin = 1; assert(!lmb_execution_valid(&execution));
    execution.nodes[1].begin = 3; assert(!lmb_execution_valid(&execution));
    execution.nodes[1].begin = 2;
    execution.nodes[1].edge = 1; assert(!lmb_execution_valid(&execution));
    execution.nodes[1].edge = 0;
    execution.count = LMB_CLUSTER_MAX_NODES + 1; assert(!lmb_execution_valid(&execution));
    execution.count = 2;
    assert(!lmb_execution_valid(NULL));
    if (argc > 1 && !strcmp(argv[1], "plan")) {
        snprintf(execution.nodes[1].name, 64, "donor-1\033[2J");
        lmb_execution_print(stdout, &execution);
        lmb_execution_print(stdout, NULL);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "help")) {
        render_help();
        return 0;
    }
    (void)setlocale(LC_CTYPE, "");
    g_tty = 1;
    g_slash_completion = 1;
    signal(SIGPIPE, SIG_IGN);
    static LmbTuiState selection;
    selection.nnodes = 3;
    for (int i = 0; i < 3; i++) {
        snprintf(selection.identities[i], sizeof selection.identities[i], "peer-%d", i);
        snprintf(selection.nodes[i].addr, sizeof selection.nodes[i].addr, "127.0.0.1:%d", i + 1);
        assert(!lmb_tui_node_enabled(&selection, (uint32_t)i));
    }
    strcpy(selection.selected_nodes[0], selection.identities[1]);
    assert(lmb_tui_node_enabled(&selection, 1));
    assert(!lmb_tui_node_enabled(&selection, 0) && !lmb_tui_node_enabled(&selection, 2));
    if (argc > 1 && !strcmp(argv[1], "editor")) {
        char line[128];
        printf("USER-PREVIOUS\nASSISTANT-PREVIOUS\n\n│ › "); fflush(stdout);
        int rc = line_edit(line, sizeof line);
        printf("\nRESULT:%s\n", rc ? "cancelled" : line);
        return rc != 0;
    }
    int pipefd[2]; assert(!pipe(pipefd));
    assert(!engine_write_full(pipefd[1], "pipe", 4));
    char got[4]; assert(read(pipefd[0], got, 4) == 4 && !memcmp(got, "pipe", 4));
    close(pipefd[0]); close(pipefd[1]);
    for (int turn = 0; turn < 2; turn++) {
        printf("\nUSER-%d: keep my message\nASSISTANT-%d:\n", turn, turn);
        assert(live_begin("", "renderer-test", "testing"));
        for (int row = 0; row < 35; row++) {
            char text[100];
            snprintf(text, sizeof text, "answer-%d-line-%02d\n", turn, row);
            /* Deliberately fragmented output, as on a real token stream. */
            for (size_t j = 0; j < strlen(text); j++) live_write(text + j, 1);
        }
        const char *unicode = "caffè 中文\n";
        for (size_t j = 0; j < strlen(unicode); j++) live_write(unicode + j, 1);
        live_end();
        puts("TURN-DONE");
    }
    return 0;
}
