#include "lumabri_proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_string(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    const char *tracker = NULL, *model_filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tracker") && i + 1 < argc) tracker = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model_filter = argv[++i];
        else {
            fprintf(stderr, "usage: %s --tracker HOST:PORT [--model NAME]\n", argv[0]);
            return 2;
        }
    }
    if (!tracker) {
        fprintf(stderr, "--tracker is required\n");
        return 2;
    }

    LmbMsg message = {0};
    if (lmb_request(tracker, LMB_SWARM_DETAIL, NULL, 0, &message) ||
        message.op != LMB_SWARM_DETAIL_R || message.pay_len) {
        fprintf(stderr, "cannot read swarm detail from %s\n", tracker);
        lmb_msg_free(&message);
        return 1;
    }
    LmbCur cursor = { message.body, message.body_len, 0 };
    uint32_t version = 0, count = 0;
    if (lmb_cur_u32(&cursor, &version) ||
        version != LMB_SWARM_DETAIL_VERSION ||
        lmb_cur_u32(&cursor, &count) || count > 4096) {
        fprintf(stderr, "malformed swarm detail\n");
        lmb_msg_free(&message);
        return 1;
    }

    printf("{\"schema\":1,\"tracker\":");
    json_string(tracker);
    printf(",\"peers\":[");
    unsigned emitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        char name[64], model[64];
        uint32_t roles, age, files, experts, have_stats, expert_inflight;
        uint32_t estate, residency, resident_experts;
        uint32_t begin, end, active, maximum, queued, segment_inflight, flags;
        uint64_t held, served, reads, calls, resident_ram, resident_vram;
        int bad = lmb_cur_str(&cursor, name, sizeof name) ||
                  lmb_cur_str(&cursor, model, sizeof model) ||
                  lmb_cur_u32(&cursor, &roles) || lmb_cur_u32(&cursor, &age) ||
                  lmb_cur_u64(&cursor, &held) || lmb_cur_u64(&cursor, &served) ||
                  lmb_cur_u64(&cursor, &reads) || lmb_cur_u32(&cursor, &files) ||
                  lmb_cur_u32(&cursor, &experts) ||
                  lmb_cur_u32(&cursor, &have_stats) ||
                  lmb_cur_u64(&cursor, &calls) ||
                  lmb_cur_u32(&cursor, &expert_inflight) ||
                  lmb_cur_u32(&cursor, &estate) ||
                  lmb_cur_u32(&cursor, &residency) ||
                  lmb_cur_u32(&cursor, &resident_experts) ||
                  lmb_cur_u64(&cursor, &resident_ram) ||
                  lmb_cur_u64(&cursor, &resident_vram) ||
                  lmb_cur_u32(&cursor, &begin) || lmb_cur_u32(&cursor, &end) ||
                  lmb_cur_u32(&cursor, &active) ||
                  lmb_cur_u32(&cursor, &maximum) ||
                  lmb_cur_u32(&cursor, &queued) ||
                  lmb_cur_u32(&cursor, &segment_inflight) ||
                  lmb_cur_u32(&cursor, &flags);
        if (bad) {
            fprintf(stderr, "malformed peer row\n");
            lmb_msg_free(&message);
            return 1;
        }
        if (model_filter && strcmp(model_filter, model)) continue;
        printf("%s{\"name\":", emitted++ ? "," : ""); json_string(name);
        printf(",\"model\":"); json_string(model);
        printf(",\"roles\":%u,\"age_s\":%u", roles, age);
        printf(",\"storage\":{\"bytes_held\":%" PRIu64
               ",\"bytes_served\":%" PRIu64 ",\"reads\":%" PRIu64
               ",\"files\":%u}", held, served, reads, files);
        printf(",\"expert\":{\"count\":%u,\"stats\":%s,\"calls\":%" PRIu64
               ",\"inflight\":%u,\"state\":%u,\"residency_flags\":%u"
               ",\"resident_count\":%u,\"resident_ram_bytes\":%" PRIu64
               ",\"resident_vram_bytes\":%" PRIu64 "}", experts,
               have_stats ? "true" : "false", calls, expert_inflight,
               estate, residency, resident_experts, resident_ram, resident_vram);
        printf(",\"segment\":{\"begin\":%u,\"end\":%u,\"sessions\":%u"
               ",\"max_sessions\":%u,\"queue\":%u,\"inflight\":%u"
               ",\"flags\":%u}}", begin, end, active, maximum, queued,
               segment_inflight, flags);
    }
    printf("]}\n");
    int malformed = cursor.off != cursor.len;
    lmb_msg_free(&message);
    if (malformed) {
        fprintf(stderr, "trailing bytes in swarm detail\n");
        return 1;
    }
    return 0;
}
