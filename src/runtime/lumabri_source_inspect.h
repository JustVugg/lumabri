/* Shared signed-source inventory inspection. */
#ifndef LMB_SOURCE_INSPECT_H
#define LMB_SOURCE_INSPECT_H
#include "lumabri_proto.h"
#include "lumabri_planner.h"
#include "lumabri_weight_container.h"
typedef struct {
    char peers[8][64]; int npeers;
    uint64_t total_bytes, metadata_bytes; int nfiles;
    char config_peer[64];
    char model_type[64];
} Swarm;

static int swarm_inspect(const char *tracker, const char *model, Swarm *s) {
    memset(s, 0, sizeof *s);
    LmbMsg m = {0};
    LmbBuf fb = {0};
    if (model && model[0]) lmb_buf_str(&fb, model);
    int rc = lmb_request(tracker, LMB_PLACEMENT, fb.p, (uint32_t)fb.len, &m);
    free(fb.p);
    if (rc || m.op != LMB_PLACEMENT_R) {lmb_msg_free(&m);return -1;}
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) { lmb_msg_free(&m); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        char rel[LMB_PATH_MAX], addr[64];
        uint64_t size; uint16_t np;
        if (lmb_cur_str(&c, rel, sizeof rel) || lmb_cur_u64(&c, &size) ||
            lmb_cur_u16(&c, &np)) { lmb_msg_free(&m); return -1; }
        if (UINT64_MAX - s->total_bytes < size) { lmb_msg_free(&m); return -1; }
        s->nfiles++; s->total_bytes += size;
        if (!lmb_weight_container(rel))
            s->metadata_bytes += size;
        int is_cfg = !strcmp(rel, "config.json");
        for (uint16_t p = 0; p < np; p++) {
            if (lmb_cur_str(&c, addr, sizeof addr)) { lmb_msg_free(&m); return -1; }
            if (is_cfg && !s->config_peer[0])
                snprintf(s->config_peer, sizeof s->config_peer, "%s", addr);
            int seen = 0;
            for (int k = 0; k < s->npeers; k++) if (!strcmp(s->peers[k], addr)) seen = 1;
            if (!seen && s->npeers < 8)
                snprintf(s->peers[s->npeers++], 64, "%s", addr);
        }
    }
    lmb_msg_free(&m);
    if (!s->nfiles || !s->config_peer[0]) return -1;

    /* config.json: direct from the peer, else relayed through the tracker
     * (the peer may be behind a NAT and reachable only outbound) */
    LmbMsg r = {0};
    LmbSocket fd = lmb_connect_ms(s->config_peer, 3000);
    if (fd != LMB_INVALID_SOCKET && lmb_auth(fd)) { lmb_close(fd); fd = LMB_INVALID_SOCKET; }
    if (fd != LMB_INVALID_SOCKET) {
        LmbBuf b = {0};
        lmb_buf_str(&b, "config.json"); lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 1 << 20);
        rc = lmb_send(fd, LMB_READ, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        if (rc == 0) rc = lmb_recv(fd, &r);
        lmb_close(fd);
    } else rc = -1;
    if (rc || r.op != LMB_READ_R || !r.pay_len) {
        lmb_msg_free(&r);
        memset(&r, 0, sizeof r);
        LmbBuf b = {0};
        lmb_buf_str(&b, model ? model : "");
        lmb_buf_str(&b, "config.json");
        lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 1 << 20);
        rc = lmb_request(tracker, LMB_RREAD, b.p, (uint32_t)b.len, &r);
        free(b.p);
        if (rc || r.op != LMB_RREAD_R || !r.pay_len) { lmb_msg_free(&r); return -1; }
    }
    /* the payload is not NUL-terminated: parse a terminated copy, so a
     * truncated config can never send strchr past the allocation */
    char *cfg = malloc((size_t)r.pay_len + 1);
    if (cfg) {
        memcpy(cfg, r.pay, r.pay_len);
        cfg[r.pay_len] = 0;
        (void)lmb_json_string(cfg, "model_type", s->model_type,
                              sizeof s->model_type);
        free(cfg);
    }
    lmb_msg_free(&r);
    return 0;
}


#endif
