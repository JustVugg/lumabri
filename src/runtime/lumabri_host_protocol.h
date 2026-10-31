/* Hosted framing, authentication and session boundaries shared by native
 * gateways. The including process supplies its stop flag and monotonic clock. */
#ifndef LMB_HOST_PROTOCOL_H
#define LMB_HOST_PROTOCOL_H
#include "src/runtime/lumabri_host_engine.h"
typedef struct {
    LmbHostEngine *engine;
    const char *model_type;
    const char *engine_kind;
    uint32_t speed_milli;      /* 0 until this host has been measured */
    uint32_t max_frame;
    uint32_t max_new;
    uint32_t idle_seconds;
    uint32_t request_seconds; /* absolute bound for an active inference */
    const uint8_t *client_key; /* optional identity bound by an accepted home plan */
    const char *model_root; /* authenticated accepted checkpoint, not a peer's label */
    uint32_t slots, routed_slot, sessions_free;
    int pooled; /* return from bridge after one turn, including a one-slot pool */
} HostState;

#include "src/runtime/lumabri_host_control.h"

static inline int host_read_request(LmbSocket fd, const HostState *h, LmbMsg *request) {
    lmb_set_io_timeout(fd, 2000);
    uint64_t prior_deadline = lmb_read_deadline_ms, now = lmb_io_monotonic_ms();
    if (!now) return -1;
    lmb_read_deadline_ms = prior_deadline && prior_deadline < now + 2000 ? prior_deadline : now + 2000;
    int handshake = lmb_secure_server(fd);
    lmb_read_deadline_ms = prior_deadline;
    if (handshake ||
        (h->client_key && !lmb_secure_peer_matches(fd, h->client_key))) return -1;
    LmbMsg m = {0};
    int rc = lmb_recv_bounded(fd, &m, LMB_TOKEN_MAX + 4, 2000);
    const char *token = getenv("LUMABRI_TOKEN");
    if (!rc && m.op == LMB_AUTH) {
        char supplied[LMB_TOKEN_MAX + 1] = "", expected[LMB_TOKEN_MAX + 1] = "";
        LmbCur c = {m.body, m.body_len, 0};
        rc = m.pay_len || lmb_cur_str(&c, supplied, sizeof supplied) || c.off != c.len ||
             !token || strlen(token) > LMB_TOKEN_MAX;
        if (!rc) {
            snprintf(expected, sizeof expected, "%s", token);
            rc = !lmb_token_equal(supplied, expected);
        }
        lmb_msg_free(&m);
        if (!rc) rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0) ||
            lmb_recv_bounded(fd, &m, LMB_HOST_CONTROL_REQUEST_BYTES, 2000);
    } else if (token && *token) rc = -1;
    if (!rc) { *request=m; return 0; }
    lmb_msg_free(&m);
    return -1;
}

static inline int host_read_hello(LmbSocket fd, const HostState *h) {
    LmbMsg request={0};
    int rc=host_read_request(fd,h,&request) || request.op!=LMB_HOST_HELLO || request.body_len || request.pay_len;
    lmb_msg_free(&request); return rc ? -1 : 0;
}

static inline int host_greet(LmbSocket fd, const HostState *h, int busy) {
    LmbBuf b = {0};
    lmb_buf_str(&b, h->model_type ? h->model_type : "");
    lmb_buf_str(&b, h->engine_kind ? h->engine_kind : "");
    lmb_buf_str(&b, "cpu");    /* no Segment adapter advertises a GPU yet */
    lmb_buf_u32(&b, busy ? 0u : h->sessions_free ? h->sessions_free : 1u);
    lmb_buf_u32(&b, h->speed_milli);
    lmb_buf_u32(&b, h->max_new);
    lmb_buf_u32(&b, h->max_frame);
    lmb_buf_u32(&b, 2);       /* SUBMIT/DATA/DONE, independent of model family */
    /* Optional capability word after codec. Older clients reject the extra
     * field rather than silently request unsupported stochastic sampling. */
    if (h->engine->greedy_only || h->engine->numeric_abi || h->engine->request_seed_supported || h->engine->vision_supported)
        lmb_buf_u32(&b, (uint32_t)h->engine->greedy_only | (h->engine->request_seed_supported ? 2u : 0u) | (h->engine->vision_supported ? 4u : 0u));
    if (h->engine->numeric_abi) {
        lmb_buf_u32(&b, 0x314d554e); /* NUM1, optional measured runtime metadata */
        lmb_buf_u32(&b, h->engine->numeric_abi);
        lmb_buf_str(&b, h->engine->numeric_class);
        if (h->model_root && h->model_root[0]) {
            lmb_buf_u32(&b, 0x31544f52); /* ROT1, accepted household root */
            lmb_buf_str(&b, h->model_root);
        }
    }
    int rc = lmb_send(fd, LMB_HOST_HELLO_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

typedef struct {
    char header[512];
    size_t header_len;
    uint64_t payload_left;
    int need_terminator;
    char request_id[64];
    int active;
    double started;
    LmbBuf *capture; /* pre-admission request bytes, never sent to an engine */
} HostInput;

static inline int host_input_write(HostInput *in, LmbHostEngine *e, const void *data, size_t bytes) {
    return in->capture ? lmb_buf_bytes(in->capture, data, bytes) : host_engine_write(e, data, bytes);
}

static inline int host_header(HostInput *in, LmbHostEngine *e, const HostState *h) {
    in->header[in->header_len] = 0;
    char id[64], extra;
    unsigned slot = 0, max_new = 0;
    unsigned long long bytes = 0;
    float temp = 0, top_p = 0;
    char seed_text[24]=""; uint64_t seed=0;
    int vision=!strncmp(in->header,"SUBMITV ",8);
    if (vision && !e->vision_supported) return -1;
    int fields = sscanf(in->header, vision ? "SUBMITV %63s %u %llu %u %f %f %23s %c" : "SUBMIT %63s %u %llu %u %f %f %23s %c",
                        id, &slot, &bytes, &max_new, &temp, &top_p, seed_text, &extra);
    if (fields == 6 || fields == 7) {
        if (fields==7 && (!e->request_seed_supported || lmb_request_seed(seed_text,&seed))) return -1;
        if (in->active || slot != 0 || bytes > h->max_frame || max_new < 1 ||
            max_new > h->max_new || !isfinite(temp) || !isfinite(top_p) ||
            temp < 0 || top_p <= 0 || top_p > 1) {
            fprintf(stderr, "[host] refused SUBMIT limits (slot=%u bytes=%llu max_new=%u/%u)\n", slot, bytes, max_new, h->max_new);
            return -1;
        }
        if (h->slots > 1) {
            if (id[0] == '0' && id[1]) return -1;
            for (const char *p = id; *p; p++) if (*p < '0' || *p > '9') return -1;
            errno = 0; unsigned long number = strtoul(id, NULL, 10);
            if (errno || number > UINT_MAX) return -1;
            char suffix[24]="";
            if (fields==7) snprintf(suffix,sizeof suffix," %llu",(unsigned long long)seed);
            int size = snprintf(in->header, sizeof in->header, "%s %s %u %llu %u %.9g %.9g%s\n",
                vision ? "SUBMITV" : "SUBMIT", id, h->routed_slot, bytes, max_new, (double)temp, (double)top_p,suffix);
            if (size < 0 || (size_t)size >= sizeof in->header) return -1;
            in->header_len = (size_t)size;
        }
        if (host_input_write(in, e, in->header, in->header_len)) return -1;
        snprintf(in->request_id, sizeof in->request_id, "%s", id);
        in->payload_left = bytes;
        in->need_terminator = bytes == 0;
        in->header_len = 0;
        return 0;
    }
    fields = sscanf(in->header, "CANCEL %63s %c", id, &extra);
    if (fields == 1) {
        if ((h->pooled || h->slots > 1) && (!in->active || strcmp(id, in->request_id))) return -1;
        if (h->pooled && !e->session_slots) return -1; /* legacy single: close + drain/reset */
        int rc = host_input_write(in, e, in->header, in->header_len);
        in->header_len = 0;
        return rc;
    }
    fields = sscanf(in->header, "STOP %63s %c", id, &extra);
    if (fields == 1) {
        if ((h->pooled || h->slots > 1) && (!in->active || strcmp(id, in->request_id))) return -1;
        if (h->pooled && !e->session_slots) return -1;
        int rc = host_input_write(in, e, in->header, in->header_len);
        in->header_len = 0;
        return rc;
    }
    fprintf(stderr, "[host] unrecognized codec header (%zu bytes)\n", in->header_len);
    return -1;
}

static inline int host_input(HostInput *in, LmbHostEngine *e, const HostState *h,
                      const uint8_t *data, size_t bytes) {
    size_t at = 0;
    while (at < bytes) {
        if (in->payload_left) {
            size_t take = bytes - at;
            if ((uint64_t)take > in->payload_left)
                take = (size_t)in->payload_left;
            if (host_input_write(in, e, data + at, take)) return -1;
            at += take;
            in->payload_left -= take;
            if (!in->payload_left) in->need_terminator = 1;
            continue;
        }
        if (in->need_terminator) {
            if (data[at++] != '\n' || host_input_write(in, e, "\n", 1))
                return -1;
            in->need_terminator = 0;
            in->active = 1;
            in->started = nowd();
            continue;
        }
        const uint8_t *nl = memchr(data + at, '\n', bytes - at);
        size_t take = nl ? (size_t)(nl - (data + at)) + 1 : bytes - at;
        if (take > sizeof in->header - 1 - in->header_len) return -1;
        memcpy(in->header + in->header_len, data + at, take);
        in->header_len += take;
        at += take;
        if (nl && host_header(in, e, h)) return -1;
    }
    return 0;
}

/* Observe codec boundaries, never generated text: a DATA payload may itself
 * contain "DONE <id>". Pipes can split a header or combine several frames.
 * Long telemetry lines are ignored without allocating unbounded memory. */
typedef struct {
    char header[4096];
    size_t header_len;
    uint64_t payload_left;
    int need_terminator, overflow;
    char reset_expected[65];
    int reset_done, reset_pending;
} HostOutput;

static inline int host_output(HostOutput *out, HostInput *in, const char *data,
                       size_t bytes, int *completed) {
    *completed = 0;
    for (size_t at = 0; at < bytes;) {
        if (out->payload_left) {
            size_t take = bytes - at;
            if (out->payload_left < take) take = (size_t)out->payload_left;
            out->payload_left -= take;
            at += take;
            continue;
        }
        if (out->need_terminator) {
            if (data[at++] != '\n') return -1;
            out->need_terminator = 0;
            continue;
        }
        char ch = data[at++];
        if (ch != '\n') {
            if (out->header_len < sizeof out->header - 1)
                out->header[out->header_len++] = ch;
            else out->overflow = 1;
            continue;
        }
        out->header[out->header_len] = 0;
        char id[64], extra;
        if (!strncmp(out->header, "DATA ", 5)) {
            char count[32];
            if (out->overflow || sscanf(out->header, "DATA %63s %31s %c",
                                        id, count, &extra) != 2) return -1;
            for (size_t i = 0; count[i]; ++i)
                if (count[i] < '0' || count[i] > '9') return -1;
            errno = 0;
            char *end;
            unsigned long long n = strtoull(count, &end, 10);
            if (errno || *end) return -1;
            out->payload_left = n;
            out->need_terminator = 1;
        } else if (!out->overflow && out->reset_expected[0] &&
                   sscanf(out->header, "RESET_DONE %63s %c", id, &extra) == 1 &&
                   !strcmp(id, out->reset_expected)) {
            out->reset_done = 1;
        } else if (!out->overflow && out->reset_expected[0] &&
                   sscanf(out->header, "RESET_PENDING %63s %c", id, &extra) == 1 &&
                   !strcmp(id, out->reset_expected)) {
            out->reset_pending = 1;
        } else if (((!strncmp(out->header, "DONE ", 5) &&
                     sscanf(out->header, "DONE %63s", id) == 1) ||
                    (!strncmp(out->header, "ERROR ", 6) &&
                     sscanf(out->header, "ERROR %63s", id) == 1)) &&
                   in->active && !strcmp(id, in->request_id)) {
            in->active = 0;
            *completed = 1;
        }
        out->header_len = 0;
        out->overflow = 0;
    }
    return 0;
}

static inline int host_expired(const HostInput *in, const HostState *h,
                        double last_input, double now) {
    if (in->active)
        return h->request_seconds && now - in->started > h->request_seconds ? 2 : 0;
    return h->idle_seconds && now - last_input > h->idle_seconds ? 1 : 0;
}

/* Both directions remain inside LMB_HOST_STREAM frames (authenticated and
 * encrypted whenever the transport's strict mode is enabled). */
static inline int host_bridge(LmbSocket fd, LmbHostEngine *e, const HostState *h,
                       HostInput *remaining_input, HostOutput *remaining_output) {
    char buf[16384];
    HostInput input = *remaining_input;
    HostOutput output = {0};
    double last_input = nowd();
    int broken = 0;
    for (;;) {
        int n = host_engine_wait(fd,e,1000);
        if (g_stopping) break;
        if (n < 0) { broken = 1; break; }
        int expired = host_expired(&input, h, last_input, nowd());
        if (expired) {
            fprintf(stderr, "[host] %s\n", expired == 2 ?
                    "active request deadline exceeded" : "idle session expired");
            break;
        }
        if (n & HOST_SOCKET_READ) {
            LmbMsg m = {0};
            if (lmb_recv_bounded(fd, &m, h->max_frame + sizeof input.header, 2000) || m.op != LMB_HOST_STREAM || m.body_len ||
                !m.pay_len || host_input(&input, e, h, m.pay, m.pay_len)) {
                lmb_msg_free(&m);
                fprintf(stderr, "[host] invalid or oversized client frame\n");
                break;
            }
            last_input = nowd();
            lmb_msg_free(&m);
        }
        if (n & HOST_ENGINE_READ) {
            int got = host_engine_read(e,buf,sizeof buf);
            if (got <= 0) { broken = 1; break; }
            int completed;
            if (host_output(&output, &input, buf, (size_t)got, &completed)) {
                fprintf(stderr, "[host] invalid engine codec frame\n");
                broken = 1; break;
            }
            if (completed) last_input = nowd();
            if (lmb_send(fd, LMB_HOST_STREAM, NULL, 0, buf, (uint32_t)got))
                break;
            if (completed && (h->pooled || h->slots > 1)) break;
        }
        if (n & (HOST_SOCKET_CLOSED|HOST_ENGINE_CLOSED)) break;
    }
    *remaining_input = input;
    *remaining_output = output;
    return broken ? -1 : 0;
}

/* Close the conversation, not its resident weights. Drain an already
 * accepted request before resetting; incomplete client payloads cannot be
 * repaired by injecting a command into their bytes. A fresh unpredictable
 * acknowledgement is recognized only at codec boundaries, never in DATA.
 * 0 = retired, 1 = known pending remote CLOSE (slot still owns its identities),
 * -1 = damaged/unresponsive codec. Pending is not a successful reset. */
static inline int host_reset_conversation(LmbHostEngine *e, const HostState *h,
                                   HostInput *in, HostOutput *out) {
    if (!e->segment || !e->reset_supported || in->payload_left ||
        in->need_terminator || g_stopping) return -1;
    char bytes[16384];
    double deadline = in->active ? in->started + h->request_seconds : nowd();
    if (h->slots > 1) deadline = nowd() + 30;
    int reset_sent = 0;
    for (;;) {
        if (g_stopping) return -1;
        if (!in->active && !reset_sent) {
            uint8_t nonce[16];
            lmb_random(nonce, sizeof nonce);
            lmb_hex(out->reset_expected, nonce, sizeof nonce);
            char command[96];
            int n = h->slots > 1 ? snprintf(command, sizeof command, "RESET_SLOT %s %u\n", out->reset_expected, h->routed_slot) :
                snprintf(command, sizeof command, "RESET %s\n", out->reset_expected);
            if (host_engine_write(e, command, (size_t)n)) return -1;
            reset_sent = 1;
            deadline = nowd() + 30;
        }
        if (out->reset_done) return 0;
        if (out->reset_pending) return 1;
        if (nowd() >= deadline) return -1;
        int ready=host_engine_wait(LMB_INVALID_SOCKET,e,100);
        if (ready<0) return -1;
        if (!ready) continue;
        if (!(ready & HOST_ENGINE_READ)) return -1;
        int n=host_engine_read(e,bytes,sizeof bytes);
        int completed;
        if (n <= 0 || host_output(out, in, bytes, (size_t)n, &completed)) return -1;
    }
}

#endif
