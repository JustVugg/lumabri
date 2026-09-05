#include "lumabri_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s TRACKER EXPECTED_NAME EXPECTED_MODEL\n", argv[0]);
        return 2;
    }
    LmbMsg message = {0};
    if (lmb_request(argv[1], LMB_SWARM_DETAIL, NULL, 0, &message) ||
        message.op != LMB_SWARM_DETAIL_R || message.pay_len) {
        fprintf(stderr, "cannot read named swarm detail\n");
        return 1;
    }
    LmbCur cursor = { message.body, message.body_len, 0 };
    uint32_t version = 0, count = 0;
    if (lmb_cur_u32(&cursor, &version) || lmb_cur_u32(&cursor, &count) ||
        version != LMB_SWARM_DETAIL_VERSION || count > 64) return 1;
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        char name[64], model[64];
        uint32_t roles, age, files, experts, have_stats, expert_inflight;
        uint32_t estate, residency, resident_experts;
        uint32_t begin, end, active, maximum, queued, segment_inflight, flags;
        uint64_t held, served, reads, calls, resident_ram, resident_vram;
        if (lmb_cur_str(&cursor, name, sizeof name) ||
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
            lmb_cur_u32(&cursor, &active) || lmb_cur_u32(&cursor, &maximum) ||
            lmb_cur_u32(&cursor, &queued) ||
            lmb_cur_u32(&cursor, &segment_inflight) ||
            lmb_cur_u32(&cursor, &flags)) return 1;
        (void)age; (void)served; (void)reads; (void)experts;
        (void)have_stats; (void)calls; (void)expert_inflight;
        (void)estate; (void)residency; (void)resident_experts;
        (void)resident_ram; (void)resident_vram;
        (void)begin; (void)end; (void)active; (void)maximum;
        (void)queued; (void)segment_inflight; (void)flags;
        if (!strcmp(name, argv[2]) && !strcmp(model, argv[3]) &&
            (roles & LMB_SWARM_ROLE_STORAGE) && held && files)
            found = 1;
    }
    int bad = cursor.off != cursor.len || !found;
    lmb_msg_free(&message);
    if (bad) {
        fprintf(stderr, "named storage host not present in swarm detail\n");
        return 1;
    }
    puts("named swarm detail: ok");
    return 0;
}
