#include "lumabri_proto.h"
#include "lumabri_segment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int request(const char *tracker, const void *body, uint32_t body_len,
                   const char *expected) {
    LmbMsg message = {0};
    int request_failed = lmb_request(tracker, LMB_RSEG, body, body_len, &message);
    LmbCur cursor = { message.body, message.body_len, 0 };
    char error[128] = "";
    int malformed = lmb_cur_str(&cursor, error, sizeof error) ||
                    cursor.off != cursor.len;
    if (request_failed || message.op != LMB_ERR || message.pay_len || malformed ||
        strcmp(error, expected)) {
        fprintf(stderr, "expected '%s', got op=%u error='%s' request=%d\n",
                expected, message.op, error, request_failed);
        lmb_msg_free(&message);
        return -1;
    }
    lmb_msg_free(&message);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    LmbBuf body = {0};
    if (lmb_buf_str(&body, "absent-segment-peer") ||
        lmb_buf_u32(&body, LMB_SEG_HEALTH) || lmb_buf_u32(&body, 0)) return 1;
    int bad = request(argv[1], body.p, (uint32_t)body.len,
                      "Segment relay peer is unavailable") ||
              request(argv[1], body.p, (uint32_t)body.len,
                      "Segment relay source rate/concurrency limit");
    free(body.p);
    if (bad) {
        fprintf(stderr, "relay token bucket did not enforce its burst\n");
        return 1;
    }
    puts("Segment relay rate limit: ok");
    return 0;
}
