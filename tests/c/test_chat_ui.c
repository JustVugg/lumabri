/* Exercise the actual CLI editor and streaming renderer, without an engine. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    LmbPrepareProgress prep = {0};
    char prep_bar[29], prep_detail[180];
    lmb_prepare_display(&prep.index, 1, 0, prep_bar, prep_detail, sizeof prep_detail);
    assert(strstr(prep_detail, "estimating...") && !strchr(prep_detail, '%'));
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 INDEX 0 64000000 0", 0);
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 INDEX 16000000 64000000 2000", 2);
    lmb_prepare_display(&prep.index, 1, 2, prep_bar, prep_detail, sizeof prep_detail);
    assert(strstr(prep_detail, "25%") && strstr(prep_detail, "6 s remaining"));
    lmb_prepare_display(&prep.index, 1, 20, prep_bar, prep_detail, sizeof prep_detail);
    assert(strstr(prep_detail, "ETA unavailable") && !strstr(prep_detail, "6 s"));
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 INDEX 64000000 64000000 4000", 4);
    lmb_prepare_display(&prep.index, 1, 4, prep_bar, prep_detail, sizeof prep_detail);
    assert(strstr(prep_detail, "100%") && strstr(prep_detail, "identity checks finishing"));
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 INDEX -1 64000000 5000", 5);
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 INDEX 64000001 64000000 5000", 5);
    assert(prep.index.done == 64000000);
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 INDEX 0 32000000 0", 6);
    assert(prep.index.rate == 0 && prep.index.samples == 1);
    lmb_prepare_record(&prep, "LMB_PREPARE_V1 TRANSFER 128000000 0 1000", 7);
    lmb_prepare_display(&prep.transfer, 0, 7, prep_bar, prep_detail, sizeof prep_detail);
    assert(strstr(prep_detail, "128.0 MB served") && strstr(prep_detail, "remaining time unavailable"));
    assert(!strchr(prep_detail, '%')); /* repeated reads cannot produce >100% */
    char prep_path[] = "/tmp/lumabri-preparation-log.XXXXXX";
    int prep_fd = mkstemp(prep_path); assert(prep_fd >= 0);
    FILE *prep_log = fdopen(prep_fd, "w"); assert(prep_log);
    LmbPrepareProgress observed = {0};
    fputs("LMB_PREPARE_V1 INDEX 10 100 ", prep_log); fflush(prep_log);
    lmb_prepare_read(&observed, prep_path, 1); assert(!observed.index.samples);
    fputs("1000\n", prep_log); fflush(prep_log);
    lmb_prepare_read(&observed, prep_path, 2); assert(observed.index.done == 10);
    for (int i = 0; i < 10000; i++) fputc('x', prep_log);
    fputs("LMB_PREPARE_V1 INDEX 100 100 2000\nLMB_PREPARE_V1 INDEX 20 100 3000\n", prep_log);
    fflush(prep_log);
    off_t prev_offset = observed.offset;
    lmb_prepare_read(&observed, prep_path, 3);
    assert(observed.offset - prev_offset <= 8192 && observed.index.done == 10);
    lmb_prepare_read(&observed, prep_path, 4); assert(observed.index.done == 20);
    assert(!ftruncate(prep_fd, 0)); rewind(prep_log);
    fputs("LMB_PREPARE_V1 INDEX 1 2 0\n", prep_log); fflush(prep_log);
    lmb_prepare_read(&observed, prep_path, 5);
    assert(observed.index.done == 1 && observed.index.samples == 1 && observed.index.rate == 0);
    fclose(prep_log); assert(!unlink(prep_path));
    if (argc > 1 && !strcmp(argv[1], "prepare-progress")) {
        puts("PREPARATION PROGRESS: PASS (bounded logs, counters, ETA, unknown totals, stale/reset)");
        return 0;
    }
    /* Hosted inactivity is not an inference deadline. Even an engine which
     * emits no progress during prefill must retain its accepted session. */
    HostState host_limits = {.idle_seconds = 1, .request_seconds = 10,
                             .max_frame = 1024, .max_new = 32};
    HostInput host_in = {0};
    HostOutput host_out = {0};
    assert(host_expired(&host_in, &host_limits, 0, 2) == 1);
    FILE *codec_sink = tmpfile(); assert(codec_sink);
    Engine codec_engine = {.to = fileno(codec_sink)};
    const char *submit = "SUBMIT 7 0 2 4 0 1\nhi";
    for (size_t i = 0; submit[i]; ++i)
        assert(!host_input(&host_in, &codec_engine, &host_limits,
                           (const uint8_t *)submit + i, 1));
    assert(!host_in.active); /* incomplete payload still has an idle limit */
    assert(!host_input(&host_in, &codec_engine, &host_limits,
                       (const uint8_t *)"\n", 1));
    assert(host_in.active && !strcmp(host_in.request_id, "7"));
    assert(!host_input(&host_in, &codec_engine, &host_limits,
                       (const uint8_t *)"CANCEL 7\n", 9));
    assert(host_in.active); /* cancellation awaits terminal engine response */
    assert(!host_expired(&host_in, &host_limits, 0, host_in.started + 2));
    assert(host_expired(&host_in, &host_limits, host_in.started + 10,
                        host_in.started + 11) == 2);
    const char *overlap = "SUBMIT 8 0 0 4 0 1\n";
    assert(host_input(&host_in, &codec_engine, &host_limits,
                      (const uint8_t *)overlap, strlen(overlap)));
    host_in.header_len = 0;
    /* The fake terminal frame inside generated text must not clear active. */
    const char *reply = "ACCEPT 7\nDATA 7 7\nDONE 7\n\nDONE 8\n";
    int completed;
    for (size_t i = 0; reply[i]; ++i) {
        assert(!host_output(&host_out, &host_in, reply + i, 1, &completed));
        assert(!completed && host_in.active);
    }
    char telemetry[2048]; memset(telemetry, 'x', sizeof telemetry);
    assert(!host_output(&host_out, &host_in, telemetry, sizeof telemetry, &completed));
    const char *done = "\nPROGRESS 7 PREFILL 2 2\nDATA 7 0\n\nDONE 7 STAT 4 0\n";
    assert(!host_output(&host_out, &host_in, done, strlen(done), &completed));
    assert(completed && !host_in.active);
    assert(!host_expired(&host_in, &host_limits, 10, 10.5));
    assert(host_expired(&host_in, &host_limits, 10, 12) == 1);
    host_in.active = 1;
    assert(!host_output(&host_out, &host_in, "ERROR 7 failed\n", 15, &completed));
    assert(completed && !host_in.active);
    HostOutput bad_output = {0};
    assert(host_output(&bad_output, &host_in, "DATA 7 -1\n", 10, &completed));
    HostOutput reset_output = {0};
    snprintf(reset_output.reset_expected, sizeof reset_output.reset_expected, "private-reset");
    const char *fake_reset = "DATA 7 26\nRESET_DONE private-reset\nX\nRESET_DONE wrong\n";
    for (size_t i = 0; fake_reset[i]; i++) {
        assert(!host_output(&reset_output, &host_in, fake_reset+i, 1, &completed));
        assert(!reset_output.reset_done);
    }
    const char *real_reset = "RESET_DONE private-reset\n";
    assert(!host_output(&reset_output, &host_in, real_reset, strlen(real_reset), &completed));
    assert(reset_output.reset_done);
    /* A 32-range profile exceeds the old 512-byte header. Exercise the
     * actual Hosted observer and chatter reader, not just the field parser. */
    char long_done[4096], observed_stat[4096];
    size_t used = (size_t)snprintf(long_done, sizeof long_done,
        "DONE 7 STAT 9 4 0 0 20 0 STAGES1 32");
    for (unsigned i = 0; i < 32; i++)
        used += (size_t)snprintf(long_done + used, sizeof long_done - used,
            " %u %u 8 0.125000000", i, i + 1);
    used += (size_t)snprintf(long_done + used, sizeof long_done - used, " PERF1 9 8 15 4 19.2\n");
    assert(used > 512 && used < sizeof long_done);
    host_in.active = 1;
    for (size_t i = 0; i < used; i++) {
        assert(!host_output(&host_out, &host_in, long_done + i, 1, &completed));
        assert(completed == (i == used - 1));
    }
    FILE *long_stream = tmpfile(); assert(long_stream);
    assert(fwrite(long_done, 1, used, long_stream) == used); fflush(long_stream); rewind(long_stream);
    Engine remote_stream = {.from = fileno(long_stream)};
    assert(!stream_serve2(&remote_stream, observed_stat, sizeof observed_stat, NULL));
    LmbStageSample profile[LMB_STAGE_PROFILE_MAX]; uint32_t profile_count = 0;
    assert(!lmb_stage_samples_parse(observed_stat, profile, &profile_count) && profile_count == 32);
    LmbGenerationMetrics long_metrics;
    assert(!lmb_metrics_parse(observed_stat, &long_metrics) && long_metrics.decode_steps == 8);
    fclose(long_stream);
    fclose(codec_sink);
    if (argc > 1 && !strcmp(argv[1], "host-codec")) {
        puts("HOST CODEC: PASS (fragmentation, payload boundaries, request/idle deadlines)");
        return 0;
    }

    /* The real engine stderr consumer must preserve long JSON lines, blank
     * lines, CRLF and a final unterminated fragment in the saved log. */
    char log_path[] = "/tmp/lumabri-engine-log-test.XXXXXX";
    int log_fd = mkstemp(log_path); assert(log_fd >= 0); close(log_fd);
    const char *previous_log = getenv("LUMABRI_ENGINE_LOG");
    char *saved_log = previous_log ? strdup(previous_log) : NULL;
    assert(!previous_log || saved_log);
    assert(!setenv("LUMABRI_ENGINE_LOG", log_path, 1));
    char payload[4096], actual[4096];
    memset(payload, 'x', sizeof payload);
    memcpy(payload, "[segment-stage] ", 16);
    memcpy(payload + sizeof payload - 12, "\n\n\r\nfragment", 12);
    FILE *input_log = tmpfile(); assert(input_log);
    assert(fwrite(payload, 1, sizeof payload, input_log) == sizeof payload);
    rewind(input_log);
    int input_fd = dup(fileno(input_log)); assert(input_fd >= 0);
    (void)stderr_thread((void *)(intptr_t)input_fd);
    fclose(input_log);
    FILE *saved_stream = fopen(log_path, "rb"); assert(saved_stream);
    assert(fread(actual, 1, sizeof actual, saved_stream) == sizeof actual);
    assert(fgetc(saved_stream) == EOF && !memcmp(payload, actual, sizeof payload));
    fclose(saved_stream); assert(!unlink(log_path));
    if (saved_log) { assert(!setenv("LUMABRI_ENGINE_LOG", saved_log, 1)); free(saved_log); }
    else assert(!unsetenv("LUMABRI_ENGINE_LOG"));
    g_eng.ntail = 0;
    int probe_pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, probe_pair));
    LmbProbeDeadline deadline = {0};
    assert(lmb_probe_start(NULL, probe_pair[0], 1));
    assert(lmb_probe_start(&deadline, -1, 1));
    assert(lmb_probe_start(&deadline, probe_pair[0], NAN));
    assert(lmb_probe_start(&deadline, probe_pair[0], 0));
    assert(lmb_probe_start(&deadline, probe_pair[0], 61));
    assert(!lmb_probe_start(&deadline, probe_pair[0], 2));
    assert(lmb_probe_start(&deadline, probe_pair[0], 2));
    assert(!lmb_probe_stop(&deadline));
    assert(!lmb_probe_stop(&deadline));
    assert(write(probe_pair[1], "a", 1) == 1);
    char probe_byte;
    assert(read(probe_pair[0], &probe_byte, 1) == 1 && probe_byte == 'a');
    assert(!lmb_probe_start(&deadline, probe_pair[0], .02));
    struct pollfd probe_wait = {probe_pair[0], POLLIN, 0};
    assert(poll(&probe_wait, 1, 5000) > 0);
    assert(read(probe_pair[0], &probe_byte, 1) == 0);
    assert(lmb_probe_stop(&deadline));
    assert(fcntl(probe_pair[0], F_GETFD) >= 0); /* timer does not own/close it */
    close(probe_pair[0]); close(probe_pair[1]);
    /* A partial real codec frame must not make the bounded probe wait for
     * a payload forever, nor produce a usable STAT result on expiry. */
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, probe_pair));
    Engine stalled = {.from = probe_pair[0], .to = probe_pair[0], .proto = PROTO_SERVE2};
    const char *partial = "DATA 1 100\n";
    assert(write(probe_pair[1], partial, strlen(partial)) == (ssize_t)strlen(partial));
    assert(!lmb_probe_start(&deadline, probe_pair[0], .02));
    char probe_stat[512]; char *probe_reply = NULL;
    assert(stream_serve2(&stalled, probe_stat, sizeof probe_stat, &probe_reply) < 0);
    assert(lmb_probe_stop(&deadline) && !probe_stat[0] && !probe_reply);
    close(probe_pair[0]); close(probe_pair[1]);
    const char *boot[] = {
        "\nLUMABRI_SAMPLING LOGITS\nLUMABRI_NUMERIC 2 cpu-test\n" FRAME_READY "\n",
        "\nLUMABRI_SAMPLING GREEDY\nLUMABRI_NUMERIC 2 cpu-test\n" FRAME_READY "\n",
        "\nLUMABRI_NUMERIC 0 cpu-test\n" FRAME_READY "\n",
        "\n" FRAME_READY "\n",
        "\nLUMABRI_NUMERIC 2 abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\n" FRAME_READY "\n"
    };
    for (unsigned i = 0; i < sizeof boot / sizeof *boot; i++) {
        int ready[2]; assert(!pipe(ready));
        assert(write(ready[1], boot[i], strlen(boot[i])) == (ssize_t)strlen(boot[i]));
        close(ready[1]); Engine engine = {.from = ready[0], .numeric_abi = 99};
        assert(!engine_wait_ready(&engine)); close(ready[0]);
        assert(engine.numeric_abi == (i < 2 ? 2u : 0u));
        assert(engine.greedy_only == (i == 1));
        assert(i < 2 ? !strcmp(engine.numeric_class, "cpu-test") : !engine.numeric_class[0]);
    }
    const char *io_env = getenv("LUMABRI_IO_TIMEOUT_MS");
    char *saved_io = io_env ? strdup(io_env) : NULL;
    unsetenv("LUMABRI_IO_TIMEOUT_MS");
    assert(home_control_io_ms(1000) == 1000);
    setenv("LUMABRI_IO_TIMEOUT_MS", "5000", 1);
    assert(home_control_io_ms(1000) == 5000);
    setenv("LUMABRI_IO_TIMEOUT_MS", "300000", 1);
    assert(home_control_io_ms(1000) == 10000);
    setenv("LUMABRI_IO_TIMEOUT_MS", "invalid", 1);
    assert(home_control_io_ms(1000) == 1000);
    if (saved_io) { setenv("LUMABRI_IO_TIMEOUT_MS", saved_io, 1); free(saved_io); }
    else unsetenv("LUMABRI_IO_TIMEOUT_MS");
    char cache_dir[] = "/tmp/lumabri-weight-lease-XXXXXX", lock_path[160];
    assert(mkdtemp(cache_dir));
    snprintf(lock_path, sizeof lock_path, "%s/weights.lock", cache_dir);
    int weight_lease = home_weight_lease(cache_dir);
    assert(weight_lease >= 0 && home_weight_lease(cache_dir) < 0);
    assert(fcntl(weight_lease, F_GETFD) & FD_CLOEXEC);
    close(weight_lease);
    weight_lease = home_weight_lease(cache_dir);
    assert(weight_lease >= 0); close(weight_lease);
    assert(!unlink(lock_path));
    assert(!symlink("missing", lock_path));
    assert(home_weight_lease(cache_dir) < 0);
    assert(!unlink(lock_path) && !rmdir(cache_dir));
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
    if (argc > 1 && !strcmp(argv[1], "resident-plan")) {
        LmbResidentPlan plan = {.context = 2048, .max_new = 256, .execution = execution}, loaded;
        snprintf(plan.tracker, sizeof plan.tracker, "127.0.0.1:47300");
        snprintf(plan.host, sizeof plan.host, "127.0.0.1:47303");
        snprintf(plan.model, sizeof plan.model, "approved-model");
        memset(plan.root, 'a', 64); memset(plan.host_key, 'b', 64);
        assert(home_resident_plan_valid(&plan));
        plan.execution.nodes[0].edge = 0; assert(!home_resident_plan_valid(&plan));
        plan.execution.nodes[0].edge = 1;
        char test_dir[] = "/tmp/lumabri-resident-plan-XXXXXX", private_dir[160], record[200];
        assert(mkdtemp(test_dir));
        snprintf(private_dir, sizeof private_dir, "%s/.lumabri", test_dir);
        assert(!mkdir(private_dir, 0700));
        const char *previous = getenv("HOME"); char *saved_home = previous ? strdup(previous) : NULL;
        assert(!setenv("HOME", test_dir, 1));
        assert(!home_resident_plan_save(&plan));
        assert(!home_resident_plan_load(plan.tracker, &loaded));
        assert(!strcmp(loaded.root, plan.root) && loaded.execution.count == 2 && loaded.max_new == 256);
        assert(home_resident_plan_load("other-household:47300", &loaded));
        assert(!home_resident_plan_path(record, sizeof record));
        struct stat metadata; assert(!stat(record, &metadata) && !(metadata.st_mode & 077));
        assert(!chmod(record, 0644)); assert(home_resident_plan_load(plan.tracker, &loaded));
        assert(!home_resident_plan_save(&plan)); /* atomically replace, reset private permissions */
        int corrupt = open(record, O_WRONLY | O_APPEND); assert(corrupt >= 0);
        assert(write(corrupt, "x", 1) == 1); close(corrupt);
        assert(home_resident_plan_load(plan.tracker, &loaded));
        assert(!unlink(record)); assert(!symlink("missing", record));
        assert(home_resident_plan_load(plan.tracker, &loaded));
        assert(!unlink(record) && !rmdir(private_dir) && !rmdir(test_dir));
        if (saved_home) { setenv("HOME", saved_home, 1); free(saved_home); } else unsetenv("HOME");
        puts("RESIDENT PLAN: PASS (private atomic persistence, exact household, malformed and linked records rejected)");
        return 0;
    }
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
    selection.nmodels = 1;
    selection.models[0].planned = selection.models[0].calibration_key_valid = 1;
    selection.models[0].advice_flags = LMB_ADVICE_FASTEST_OBSERVED;
    lmb_tui_invalidate_plans(&selection);
    assert(!selection.models[0].planned && !selection.models[0].calibration_key_valid);
    assert(!selection.models[0].advice_flags);
    selection.nnodes = 3;
    for (int i = 0; i < 3; i++) {
        snprintf(selection.identities[i], sizeof selection.identities[i], "peer-%d", i);
        snprintf(selection.nodes[i].addr, sizeof selection.nodes[i].addr, "127.0.0.1:%d", i + 1);
        assert(!lmb_tui_node_enabled(&selection, (uint32_t)i));
    }
    strcpy(selection.selected_nodes[0], selection.identities[1]);
    assert(lmb_tui_node_enabled(&selection, 1));
    assert(!lmb_tui_node_enabled(&selection, 0) && !lmb_tui_node_enabled(&selection, 2));
    selection.nmodels = 2; selection.inventory_ok = 1;
    for (int i = 0; i < 2; i++) {
        LmbTuiModel *m = &selection.models[i];
        snprintf(m->name, sizeof m->name, "advice-fixture-%d", i);
        m->shape.layers = 1; m->plan.edge_node = 1; m->plan.slices[0].layer_end = 1;
        m->planned = m->shape.sizing_verified = m->weights_present = m->checkpoint_inventory_ok = 1;
        m->plan.state = LMB_PLAN_RESIDENT; m->plan.nslices = 1; m->plan.slices[0].node = 1;
        m->plan.slices[0].bytes_resident = 20u * (i + 1); m->checkpoint_bytes = 10u * (i + 1);
    }
    catalog_advice_refresh(&selection);
    assert(selection.models[0].advice_flags == LMB_ADVICE_LOWEST_RAM);
    assert(selection.models[1].advice_flags == LMB_ADVICE_LARGEST_CHECKPOINT);
    if (argc > 1 && !strcmp(argv[1], "advice")) {
        catalog_json(&selection);
        return lmb_tui_run(&selection, 1, "");
    }
    selection.models[1].shape.sizing_verified = 0;
    catalog_advice_refresh(&selection);
    assert(!selection.models[0].advice_flags && !selection.models[1].advice_flags);
    selection.models[1].shape.sizing_verified = 1; selection.selected_nodes[0][0] = 0;
    catalog_advice_refresh(&selection);
    assert(!selection.models[0].advice_flags && !selection.models[1].advice_flags);
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
