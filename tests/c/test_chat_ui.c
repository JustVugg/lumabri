/* Exercise the actual CLI editor and streaming renderer, without an engine. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "api-hold-allocation")) {
        int access=lmb_api_access_dir(argv[2],0), permit=-1; uint8_t id[32];
        assert(access>=0 && strlen(argv[3])==64 && !lmb_unhex(id,argv[3],32));
        assert(!lmb_replica_admit(access,id,&permit));
        puts("ADMITTED"); fflush(stdout);
        (void)getchar(); close(permit); close(access); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "api-codec")) {
        const char *good="[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":\"hello\"},{\"role\":\"user\",\"content\":\"next\"}]";
        LmbJson j; LmbJsonToken tokens[64]; Cap history={0}; char *prompt=NULL;
        assert(!lmb_json_parse(&j,good,strlen(good),tokens,64));
        assert(!api_messages(&j,0,EK_OLMOE,&history,&prompt));
        assert(strstr(history.p,"hi") && strstr(history.p,"hello") && !strcmp(prompt,"next"));
        free(history.p); free(prompt);
        const char *bad[]={"[]","[{\"role\":\"system\",\"content\":\"x\"}]",
            "[{\"role\":\"user\",\"content\":\"x\"},{\"role\":\"assistant\",\"content\":\"y\"}]",
            "[{\"role\":\"user\",\"content\":\"x\",\"unknown\":0}]"};
        for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
            history=(Cap){0}; prompt=NULL;
            assert(!lmb_json_parse(&j,bad[i],strlen(bad[i]),tokens,64));
            assert(api_messages(&j,0,EK_OLMOE,&history,&prompt)); free(history.p); free(prompt);
        }
        int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
        unsigned char bytes[600]; memset(bytes,'a',sizeof bytes);
        assert(!api_delta(&pair[0],bytes,sizeof bytes));
        char result[2048]; ssize_t got=read(pair[1],result,sizeof result-1); assert(got>0); result[got]=0;
        assert(strstr(result,"event: delta\n") && strstr(result,"YWFhYWFh") && strlen(result)>800);
        close(pair[0]); close(pair[1]);
        Cap large={0}; assert(!api_addf(&large,"%0120u",1) && large.len==120); free(large.p);
        large=(Cap){0}; assert(!api_json_text(&large,"caf\xc3\xa9\n"));
        assert(!strcmp(large.p,"\"caf\xc3\xa9\\u000a\"")); free(large.p);
        puts("API CODEC: PASS (exact existing templates, bounded messages, roles, lossless byte deltas)"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reply-errors")) {
        Cap overflow = {.len = SIZE_MAX - 2};
        assert(cap_add(&overflow, "four", 4) && !overflow.p);
        const char *frames[] = {"DATA 7 3\nabc\nERROR 7 cancelled\n", "DATA 7 3\nabc\nDONE 8\n", "DATA 7 3\nab"};
        for (unsigned i = 0; i < 3; i++) {
            FILE *input = tmpfile(); assert(input);
            assert(fwrite(frames[i], 1, strlen(frames[i]), input) == strlen(frames[i]));
            assert(!fflush(input)); rewind(input);
            Engine e = {.from = fileno(input)}; strcpy(e.request_id, "7");
            char stat[512] = "old metrics", *reply = (char *)1;
            assert(stream_serve2(&e, stat, sizeof stat, &reply) == (i ? -1 : -2));
            assert(!reply && !stat[0]); fclose(input);
        }
        puts("REPLY ERRORS: PASS (no successful capture or calibration on ERROR, wrong request or truncation)");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "cancel-prepare")) {
        HomeServiceSnapshot current, reply;
        assert(!home_service_query("prepare", HOME_SVC_STATUS, NULL, &current));
        return home_service_query("prepare", HOME_SVC_CANCEL, &current, &reply) != 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "portfolio-preview") || !strcmp(argv[1], "joint-guards"))) {
        LmbTuiState *st = calloc(1, sizeof *st); assert(st);
        st->nnodes = st->nmodels = 2; st->inventory_ok = 1; st->context = 128; st->sessions = 2;
        for (unsigned i = 0; i < 2; i++) {
            LmbTuiModel *m = &st->models[i];
            snprintf(m->name, sizeof m->name, "model-%u", i);
            snprintf(m->dir, sizeof m->dir, "/fixture/model-%u", i);
            strcpy(st->selected_models[i], m->dir);
            m->weights_present = m->checkpoint_inventory_ok = 1; m->checkpoint_bytes = 1000000;
            strcpy(m->shape.model_type, "olmoe"); strcpy(m->shape.segment_id, "olmoe");
            m->shape.layers = 4; m->shape.hidden = 64; m->shape.vocab = 128; m->shape.max_context = 4096;
            m->shape.memory_contract = m->shape.sizing_verified = 1; m->shape.edge_resident_bytes = 8000000;
            for (unsigned layer = 0; layer < 4; layer++) m->shape.memory[layer].resident_bytes = 100000000;
            snprintf(st->identities[i], sizeof st->identities[i], "node-%u", i);
            strcpy(st->selected_nodes[i], st->identities[i]);
            strcpy(st->runtime_ids[i], "fixture-runtime");
            strcpy(st->nodes[i].addr, "test-only"); st->nodes[i].threads = 2;
            st->nodes[i].ram_budget_bytes = 3000000000;
            st->workloads[i] = (LmbWorkloadFacts){.known=1,.compute_policy=LMB_COMPUTE_LOCAL_FIFO,.allocation_set={1}};
            st->facts[i] = (LmbResourceFacts){.known=LMB_FACT_PRICE,.price_micro_per_hour=(i+1)*100,.currency="EUR"};
        }
        const char *names[] = {"model-0", "model-1"};
        if (!strcmp(argv[1], "joint-guards")) {
            catalog_joint_refresh(st); assert(st->joint.ready && st->joint.plan.model_count == 2);
            assert(lmb_tui_model_enabled(st, 0) && lmb_tui_model_enabled(st, 1));
            LmbTuiState *now = malloc(sizeof *now); assert(now); memcpy(now, st, sizeof *now);
            uint32_t map[LMB_CLUSTER_MAX_NODES]; char why[256];
            assert(!home_joint_validate(st, now, 0, map, why, sizeof why));
            now->runtime_ids[0][0] = 0; assert(home_joint_validate(st, now, 0, map, why, sizeof why));
            memcpy(now, st, sizeof *now); now->workloads[0].known = 0;
            assert(home_joint_validate(st, now, 0, map, why, sizeof why));
            memcpy(now, st, sizeof *now); now->workloads[0].allocation_set[1] = 1;
            assert(home_joint_validate(st, now, 0, map, why, sizeof why));
            /* The first model changes allocation state, not the reviewed
             * remaining ranges. Refresh facts without inventing a new plan. */
            now->workloads[0].allocations = 1;
            assert(!home_joint_validate(st, now, 1, map, why, sizeof why));
            now->nodes[0].ram_budget_bytes = 1;
            assert(home_joint_validate(st, now, 1, map, why, sizeof why));
            memcpy(now, st, sizeof *now); now->workloads[0].allocations = 4;
            assert(home_joint_validate(st, now, 1, map, why, sizeof why));
            memcpy(now, st, sizeof *now); strcpy(now->identities[0], "different-peer");
            assert(home_joint_validate(st, now, 0, map, why, sizeof why));
            memcpy(now, st, sizeof *now); now->nodes[0].threads++;
            assert(home_joint_validate(st, now, 0, map, why, sizeof why));
            memcpy(now, st, sizeof *now);
            assert(home_joint_prepare_next(st, now, 0, why, sizeof why) == 0);
            assert(lmb_tui_node_enabled(now, 0) && !lmb_tui_node_enabled(now, 1));
            assert(now->models[0].plan.edge_node == 0 && now->models[0].planned);
            memcpy(now, st, sizeof *now);
            strcpy(now->identities[0], "unused-node"); strcpy(now->identities[1], st->identities[0]);
            assert(home_joint_prepare_next(st, now, 0, why, sizeof why) == 0);
            assert(now->models[0].plan.edge_node == 1 && now->models[0].plan.slices[0].node == 1);
            assert(lmb_tui_node_enabled(now, 1) && !lmb_tui_node_enabled(now, 0));
            lmb_tui_invalidate_plans(st); assert(!st->joint.ready && lmb_tui_model_enabled(st, 0));
            strcpy(st->selected_models[0], "/missing"); catalog_joint_refresh(st);
            assert(!st->joint.ready && strstr(st->joint.reason, "disappeared"));
            free(now); free(st); puts("JOINT GUARDS: PASS"); return 0;
        }
        int rc = catalog_portfolio_json(st, names, 2); free(st); return rc;
    }
    if (argc == 2 && !strcmp(argv[1], "service-codec")) {
        HomeServiceSnapshot input = {.state=HOME_SVC_RUNNING, .ram=1000,
            .model_count=2, .reserved_total=900, .compute_enabled=1,
            .compute_active=1, .compute_queued=3, .compute_grants=1234};
        strcpy(input.role, "donor");
        LmbBuf b = {0}; HomeServiceSnapshot output;
        strcpy(input.detail, "invalid\033[2J"); assert(home_service_pack(&b, &input));
        strcpy(input.detail, "invalid · delimiter"); assert(home_service_pack(&b, &input));
        input.detail[0] = 0;
        assert(!home_service_pack(&b, &input));
        assert(!home_service_unpack(b.p, b.len, &output));
        assert(output.compute_active == 1 && output.compute_queued == 3 && output.compute_grants == 1234);
        for (size_t n = 0; n < b.len; n++) assert(home_service_unpack(b.p, n, &output));
        lmb_put32(b.p, 3);
        assert(!home_service_unpack(b.p, b.len-32, &output) && output.compute_grants == 1234);
        lmb_put32(b.p, 2); /* v2 had allocations but no compute observation */
        assert(!home_service_unpack(b.p, b.len-52, &output));
        assert(output.model_count == 2 && output.reserved_total == 900 && !output.compute_enabled);
        lmb_put32(b.p, 1);
        assert(!home_service_unpack(b.p, b.len-64, &output));
        assert(!output.model_count && !output.compute_enabled);
        lmb_put32(b.p, 5); assert(home_service_unpack(b.p, b.len, &output));
        free(b.p); b = (LmbBuf){0};
        input.compute_active = 2; assert(home_service_pack(&b, &input));
        input.compute_active = 1; input.compute_queued = 33; assert(home_service_pack(&b, &input));
        input.compute_queued = 3; input.compute_enabled = 0; assert(home_service_pack(&b, &input));
        input.compute_enabled = 1;
        char directory[] = "/tmp/lmb-service-codec.XXXXXX"; assert(mkdtemp(directory));
        HomeService service = {.snapshot=input};
        snprintf(service.directory, sizeof service.directory, "%s", directory);
        snprintf(service.journal, sizeof service.journal, "%s/state", directory);
        assert(!home_service_save(&service));
        assert(!home_service_unpack(service.last, service.last_size, &output));
        assert(output.compute_enabled && !output.compute_active && !output.compute_queued && !output.compute_grants);
        struct stat before, after; assert(!stat(service.journal, &before));
        service.snapshot.compute_active = 0; service.snapshot.compute_queued = 9;
        service.snapshot.compute_grants++;
        assert(!home_service_save(&service)); assert(!stat(service.journal, &after));
        assert(before.st_ino == after.st_ino); /* no replace/fsync for kernel counters */
        free(service.last); assert(!unlink(service.journal)); assert(!rmdir(directory));
        puts("SERVICE CODEC: PASS (v1/v2/v3/v4, strict bounds, counters stay out of durable state)"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "model-pool")) {
        HomeDonor parked[3] = {0}, d = {.parked = parked, .park_capacity = 3, .pool_budget = 1000, .offer_revision = 3};
        for (unsigned i = 0; i < 3; i++) { parked[i] = d; parked[i].client = -1; }
        d.transaction.reservation_held = 1; d.transaction.offer.ram_bytes = 400; d.retained = 1;
        assert(home_donor_reserved(&d) == 400 && home_donor_room(&d, 900) == 600);
        home_donor_swap(&d, 0);
        assert(!d.transaction.reservation_held && d.offer_revision == 4 && home_donor_reserved(&d) == 400);
        d.transaction.reservation_held = 1; d.transaction.offer.ram_bytes = 300;
        d.transaction.offer.id[0] = 2; parked[0].transaction.offer.id[0] = 1;
        HomeService first = {0}, second = {0};
        home_service_donor_snapshot(&first, &d);
        home_donor_swap(&d, 0); home_service_donor_snapshot(&second, &d);
        assert(!memcmp(first.snapshot.allocation_set, second.snapshot.allocation_set, 32));
        home_donor_swap(&d, 0); d.transaction.offer.id[0]++;
        home_service_donor_snapshot(&second, &d);
        assert(memcmp(first.snapshot.allocation_set, second.snapshot.allocation_set, 32));
        assert(home_donor_room(&d, 400) == 100); /* reserve the loading allocation not yet in RSS */
        d.retained = 1; assert(home_donor_room(&d, 400) == 300);
        parked[1].transaction.reservation_held = 1; parked[1].transaction.offer.ram_bytes = 300;
        assert(!home_donor_room(&d, 10000));
        parked[1].transaction.offer.ram_bytes = UINT64_MAX;
        assert(home_donor_reserved(&d) == UINT64_MAX && !home_donor_room(&d, UINT64_MAX));
        puts("MODEL POOL: PASS (aggregate reservations, pending load, view swap, overflow)"); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "resident-release")) {
        assert(!lmb_secure_init()); LmbResidentPlan plan;
        assert(!home_resident_plan_read(argv[2], argv[3], &plan));
        for (uint32_t i = 0; i < plan.execution.count; i++) assert(!home_resident_peer(&plan, i, 1));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "resident-chat")) {
        assert(!lmb_secure_init()); LmbResidentPlan plan;
        assert(!home_resident_plan_read(argv[2], argv[3], &plan));
        return home_resident_plan_chat(&plan);
    }
    if (argc == 4 && !strcmp(argv[1], "resident-calibration-state")) {
        assert(!lmb_secure_init()); LmbResidentPlan plan; LmbCalKey key; LmbCalibration record;
        char directory[1200], why[200];
        assert(!home_resident_plan_read(argv[2], argv[3], &plan));
        assert(!home_resident_observation_key(&plan, &key));
        assert(!catalog_calibration_dir(directory) && !lmb_cal_load(directory, plan.content_id, &record));
        key.adapter_abi = record.key.adapter_abi;
        snprintf(key.numeric_class, sizeof key.numeric_class, "%s", record.key.numeric_class);
        assert(!catalog_workload_capture(argv[3], &key, why, sizeof why));
        printf("%s\n", lmb_cal_matches(&record.key, &key) ? "current" : "stale"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "planner-evidence")) {
        LmbTuiState *st = calloc(1, sizeof *st); assert(st);
        st->nnodes = 1; st->nmodels = 2; st->context = 128; st->sessions = 1;
        LmbTuiModel *m = &st->models[0];
        strcpy(m->name, "observed"); strcpy(m->shape.model_type, "olmoe"); strcpy(m->shape.segment_id, "olmoe");
        m->shape.layers = 4; m->shape.hidden = 64; m->shape.vocab = 256;
        m->shape.sizing_verified = m->shape.memory_contract = 1; m->shape.max_context = 128;
        m->shape.edge_resident_bytes = 1000;
        for (unsigned i = 0; i < 4; i++) m->shape.memory[i].resident_bytes = 1000;
        m->planned = m->has_calibration = m->calibration_key_valid = 1;
        m->plan.nslices = 1; m->plan.slices[0].layer_end = 4; m->plan.slices[0].bytes_resident = 1000000;
        LmbCalKey *k = &m->calibration.key;
        strcpy(k->model_root, "test-only"); strcpy(k->adapter, "olmoe");
        strcpy(k->numeric_class, "test-only"); strcpy(k->build_id, "fixture"); strcpy(k->plan_kind, "segment");
        k->adapter_abi = k->nodes = k->sessions = 1; k->context = 128; k->layer_end[0] = 4; k->threads[0] = 2;
        strcpy(k->node_id[0], "fixture"); strcpy(k->node_hardware_id[0], "fixture");
        strcpy(k->node_build_id[0], "fixture"); strcpy(k->node_backend[0], "cpu");
        m->calibration.decode_tok_s = 10; m->calibration.ttft_seconds = .1;
        m->calibration.measured_at = (double)time(NULL); m->calibration.samples = 3;
        m->calibration.prompt_tokens = 4; m->calibration.generated_tokens = 8;
        m->calibration.stage_count = 1; m->calibration.stage_decode_seconds[0] = .08;
        m->calibration.source = LMB_CAL_SOURCE_SESSION;
        m->calibration.preparation_seconds = 1.25; m->calibration.prepared_at = m->calibration.measured_at;
        m->calibration.link_count = 1;
        double link_times[] = {.001, .002, .003};
        assert(!lmb_link_observed(&m->calibration.links[0], link_times, .01, m->calibration.measured_at));
        assert(lmb_cal_valid(&m->calibration)); m->calibration_key = *k;
        st->models[1] = *m; strcpy(st->models[1].name, "changed-threads");
        st->models[1].calibration_key.threads[0]++;
        catalog_json(st); free(st); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "resident-deny-stale")) {
        assert(!lmb_secure_init());
        LmbResidentPlan plan, changed;
        assert(!home_resident_plan_read(argv[2], argv[3], &plan));
        changed = plan; changed.allocation[0] ^= 1;
        assert(home_resident_peer(&changed, 0, 1));
        changed = plan; changed.root[0] = changed.root[0] == 'a' ? 'b' : 'a';
        assert(home_resident_peer(&changed, 0, 1));
        assert(!home_resident_peer(&plan, 0, 0));
        puts("RESIDENT AUTHORITY: PASS (stale allocation/root cannot unload the live model)");
        return 0;
    }
    LmbMachineReport inventory = {.machine = {.logical_cpus = 4, .physical_cores = 2,
        .ram_total_bytes = 8ull << 30}, .runtime_threads = 4};
    inventory.identity[0] = 1;
    snprintf(inventory.control_addr, sizeof inventory.control_addr, "192.168.1.12:47301");
    memset(inventory.runtime_id, 'a', 64);
    LmbCalKey approved = {.nodes = 1}; char reason[200];
    lmb_hex(approved.node_id[0], inventory.identity, 32);
    memcpy(approved.node_build_id[0], inventory.runtime_id, 65);
    approved.threads[0] = 4;
    catalog_hardware_id(&inventory.machine, inventory.control_addr, approved.node_hardware_id[0]);
    assert(!catalog_runtime_match(&approved, &inventory, 1, reason, sizeof reason));
    /* RAM consumed by the approved model is not a hardware change. */
    inventory.machine.ram_available_bytes = 1ull << 30;
    inventory.ram_budget_bytes = 0;
    assert(!catalog_runtime_match(&approved, &inventory, 1, reason, sizeof reason));
    assert(catalog_runtime_match(&approved, &inventory, 0, reason, sizeof reason) == 1);
    assert(strstr(reason, "temporarily unavailable"));
    assert(!catalog_runtime_match(&approved, &inventory, 1, reason, sizeof reason));
    inventory.runtime_id[0] = 'b';
    assert(catalog_runtime_match(&approved, &inventory, 1, reason, sizeof reason) == -1);
    assert(strstr(reason, "runtime identity"));
    inventory.runtime_id[0] = 'a'; inventory.runtime_threads = 1;
    assert(catalog_runtime_match(&approved, &inventory, 1, reason, sizeof reason) == -1);
    assert(strstr(reason, "thread capacity"));
    inventory.runtime_threads = 4; inventory.machine.logical_cpus = 8;
    assert(catalog_runtime_match(&approved, &inventory, 1, reason, sizeof reason) == -1);
    assert(strstr(reason, "hardware or endpoint"));
    if (argc > 1 && !strcmp(argv[1], "calibration-inventory")) {
        puts("CALIBRATION INVENTORY: PASS (absence is retryable; runtime, hardware and thread changes rejected)");
        return 0;
    }
    LmbPrepareWatchdog watchdog = {.started = 0, .advanced = 0};
    /* Same progressing transfer that used to be killed at 900 seconds. */
    for (unsigned t = 300; t <= 3600; t += 300) {
        lmb_prepare_watchdog_advance(&watchdog, t, t * 1024u, 2);
        assert(!lmb_prepare_watchdog_expired(&watchdog, t));
    }
    /* Connected/status repeats do not renew a stalled download. */
    lmb_prepare_watchdog_advance(&watchdog, 4499, 3600 * 1024u, 2);
    assert(!lmb_prepare_watchdog_expired(&watchdog, 4499));
    assert(lmb_prepare_watchdog_expired(&watchdog, 4500) == 1);
    /* Local loading / host startup can advance without network traffic. */
    lmb_prepare_watchdog_advance(&watchdog, 4499, 3600 * 1024u, 3);
    assert(!lmb_prepare_watchdog_expired(&watchdog, 4500));
    lmb_prepare_watchdog_advance(&watchdog, 86400, 999999999, 4);
    assert(lmb_prepare_watchdog_expired(&watchdog, 86400) == 2);
    LmbPrepareWatchdog approval = {.started = 10, .advanced = 10};
    assert(lmb_prepare_watchdog_expired(&approval, 910) == 1);
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
    /* Client slot selection is never authority. Rewrite slot zero using the
     * assigned conversation, before any buffered bytes reach the shared Edge. */
    LmbBuf captured = {0}; HostInput routed = {.capture=&captured};
    HostState multiplex = {.slots=4, .routed_slot=3, .max_frame=1024, .max_new=8};
    const char *request = "SUBMIT 17 0 2 8 0 1\nhi\n";
    assert(!host_input(&routed, &codec_engine, &multiplex, (const uint8_t *)request, strlen(request)));
    assert(routed.active && captured.len == strlen("SUBMIT 17 3 2 8 0 1\nhi\n"));
    assert(!memcmp(captured.p, "SUBMIT 17 3 2 8 0 1\nhi\n", captured.len));
    assert(host_input(&routed, &codec_engine, &multiplex, (const uint8_t *)"CANCEL 18\n", 10));
    free(captured.p); captured = (LmbBuf){0}; routed = (HostInput){.capture=&captured};
    request = "SUBMIT 1 1 0 8 0 1\n\n";
    assert(host_input(&routed, &codec_engine, &multiplex, (const uint8_t *)request, strlen(request)));
    assert(!captured.len);
    routed = (HostInput){.capture=&captured}; request = "SUBMIT -1 0 0 8 0 1\n\n";
    assert(host_input(&routed, &codec_engine, &multiplex, (const uint8_t *)request, strlen(request)));
    assert(!captured.len);
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
        plan.execution.hybrid = 1;
        plan.execution.nodes[0].end = plan.execution.layers;
        assert(!home_resident_plan_save(&plan));
        assert(!home_resident_plan_load(plan.tracker, &loaded));
        assert(loaded.execution.hybrid && lmb_execution_valid(&loaded.execution));
        plan.allocation[0] = 1;
        plan.peer_keys[0][0] = 2; plan.peer_keys[1][0] = 3;
        plan.preparation_seconds = 2.25; plan.prepared_at = 1000;
        assert(!home_resident_plan_save(&plan));
        assert(!home_resident_plan_load(plan.tracker, &loaded));
        assert(!memcmp(loaded.allocation, plan.allocation, 32));
        assert(loaded.preparation_seconds == 2.25 && loaded.prepared_at == 1000);
        assert(!memcmp(loaded.peer_keys, plan.peer_keys, sizeof plan.peer_keys));
        for (uint32_t slots = 1; slots <= LMB_HOST_MAX_SESSIONS; slots *= 2) {
            plan.sessions = slots;
            assert(!home_resident_plan_save(&plan));
            assert(!home_resident_plan_load(plan.tracker, &loaded) && loaded.sessions == slots);
        }
        plan.sessions = 9; assert(!home_resident_plan_valid(&plan)); plan.sessions = 8;
        memset(plan.content_id, 'c', 64);
        memset(plan.observation.build_id, 'd', 64);
        snprintf(plan.observation.adapter, sizeof plan.observation.adapter, "olmoe");
        for (uint32_t i = 0; i < plan.execution.count; i++) {
            memset(plan.observation.hardware[i], 'e', 64);
            memset(plan.observation.runtime[i], 'f', 64);
            plan.observation.threads[i] = i + 1;
        }
        assert(!home_resident_plan_save(&plan));
        assert(!home_resident_plan_load(plan.tracker, &loaded));
        assert(!memcmp(&loaded.observation, &plan.observation, sizeof plan.observation));
        LmbCalKey seed;
        assert(!home_resident_observation_key(&loaded, &seed));
        assert(seed.nodes == 2 && seed.sessions == 8 && seed.threads[1] == 2);
        assert(!strcmp(seed.model_root, plan.content_id) && !strcmp(seed.adapter, "olmoe"));
        assert(!seed.adapter_abi && !seed.numeric_class[0] && !lmb_cal_key_valid(&seed));
        assert(!seed.workload[0].known); /* a saved plan never fabricates a current workload */
        loaded.observation.threads[0] = 257; assert(!home_resident_plan_valid(&loaded));
        loaded = plan; loaded.observation.runtime[0][0] = 'z'; assert(!home_resident_plan_valid(&loaded));
        loaded = plan; loaded.observation.build_id[64] = 'd'; assert(!home_resident_plan_valid(&loaded));
        LmbResidentPlan library[4]; char library_path_a[1200], library_path_b[1200], library_directory[1200];
        assert(!home_resident_library_path(&plan, library_path_a, sizeof library_path_a));
        assert(!home_resident_library_save(&plan));
        assert(!home_resident_library_save(&plan));
        assert(home_resident_library_list(plan.tracker, library, 4) == 1);
        plan.allocation[0] = 4;
        assert(!home_resident_library_path(&plan, library_path_b, sizeof library_path_b));
        assert(!home_resident_library_save(&plan));
        assert(home_resident_library_list(plan.tracker, library, 4) == 2);
        assert(!home_resident_library_list("other:47300", library, 4));
        assert(!chmod(library_path_a, 0644));
        assert(home_resident_library_list(plan.tracker, library, 4) == 1);
        assert(!unlink(library_path_a)); assert(!symlink(library_path_b, library_path_a));
        assert(home_resident_library_list(plan.tracker, library, 4) == 1);
        assert(!unlink(library_path_a)); assert(!unlink(library_path_b));
        assert(!home_resident_library_directory(library_directory, sizeof library_directory));
        assert(!rmdir(library_directory));
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
        execution.hybrid = 1;
        execution.nodes[0].end = execution.layers;
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
