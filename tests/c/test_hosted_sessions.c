#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "lumabri_secure.h"
#include "lumabri_metrics.h"
#include "src/runtime/lumabri_session_limits.h"
#include <assert.h>
#include <pthread.h>
#include <time.h>

/* Real authenticated Hosted clients, not mock engine responses. Run only
 * against a private test allocation and its expected authenticated identity. */
typedef struct {
    int fd, done, failed, newline;
    unsigned id, tokens;
    char line[8192], text[65536];
    size_t line_len, text_len, remaining;
    double submitted, first_token, previous_token, gaps[4096];
    unsigned gaps_count;
    LmbGenerationMetrics metrics;
} Client;

static double seconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static int connect_host(const char *address, const uint8_t key[32], unsigned *free_slots) {
    int fd = lmb_connect_ms_io(address, 5000, 120000);
    if (fd < 0) return -1;
    if (!lmb_secure_peer_matches(fd, key) || lmb_auth(fd) ||
        lmb_send(fd, LMB_HOST_HELLO, NULL, 0, NULL, 0)) { lmb_close(fd); return -1; }
    LmbMsg m = {0}; char model[128], engine[128], backend[64];
    int bad = lmb_recv_limited(fd, &m, 4096) || m.op != LMB_HOST_HELLO_R || m.pay_len;
    LmbCur c = {m.body, m.body_len, 0};
    if (!bad) bad = lmb_cur_str(&c, model, sizeof model) || lmb_cur_str(&c, engine, sizeof engine) ||
        lmb_cur_str(&c, backend, sizeof backend) || lmb_cur_u32(&c, free_slots);
    lmb_msg_free(&m);
    if (bad) { lmb_close(fd); return -1; }
    return fd;
}

static void consume(Client *c, const uint8_t *p, size_t size) {
    for (size_t at = 0; at < size && !c->failed; at++) {
        if (c->remaining) {
            if (c->text_len == sizeof c->text) { c->failed = 1; break; }
            c->text[c->text_len++] = (char)p[at];
            if (!--c->remaining) c->newline = 1;
        } else if (c->newline) {
            if (p[at] != '\n') c->failed = 1;
            c->newline = 0;
        } else if (p[at] != '\n') {
            if (c->line_len + 1 >= sizeof c->line) { c->failed = 1; break; }
            c->line[c->line_len++] = (char)p[at];
        } else {
            c->line[c->line_len] = 0;
            unsigned id = 0, count = 0; size_t bytes = 0;
            if (sscanf(c->line, "DATA %u %zu", &id, &bytes) == 2) {
                if (id != c->id || bytes > sizeof c->text - c->text_len) c->failed = 1;
                c->remaining = bytes; c->newline = !bytes;
            } else if (sscanf(c->line, "PROGRESS %u DECODE %u", &id, &count) == 2) {
                double now = seconds();
                if (id != c->id || count != c->tokens + 1) c->failed = 1;
                if (!c->tokens) c->first_token = now;
                else if (c->gaps_count < 4096) c->gaps[c->gaps_count++] = now - c->previous_token;
                c->previous_token = now; c->tokens = count;
            } else if (sscanf(c->line, "DONE %u", &id) == 1) {
                if (id != c->id || lmb_metrics_parse(c->line, &c->metrics)) c->failed = 1;
                c->done = 1;
            } else if (!strncmp(c->line, "ERROR ", 6)) {
                fprintf(stderr, "%s\n", c->line); c->failed = 1;
            }
            c->line_len = 0;
        }
    }
}

static void submit(Client *c, unsigned id, const char *prompt) {
    int fd = c->fd; memset(c, 0, sizeof *c); c->fd = fd; c->id = id;
    char header[128]; int n = snprintf(header, sizeof header, "SUBMIT %u 0 %zu 8 0 1\n", id, strlen(prompt));
    LmbBuf b = {0}; assert(!lmb_buf_bytes(&b, header, (size_t)n));
    assert(!lmb_buf_bytes(&b, prompt, strlen(prompt)) && !lmb_buf_bytes(&b, "\n", 1));
    c->submitted = seconds();
    assert(!lmb_send(c->fd, LMB_HOST_STREAM, NULL, 0, b.p, (uint32_t)b.len)); free(b.p);
}

static void *receive_turn(void *arg) {
    Client *c = arg;
    while (!c->done && !c->failed) {
        LmbMsg m = {0};
        if (lmb_recv_limited(c->fd, &m, 65536) || m.op != LMB_HOST_STREAM || m.body_len)
            c->failed = 1;
        else consume(c, m.pay, m.pay_len);
        lmb_msg_free(&m);
    }
    return NULL;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    assert(argc == 4); unsigned slots = (unsigned)strtoul(argv[3], NULL, 10);
    assert(slots >= 1 && slots <= LMB_HOST_MAX_SESSIONS);
    uint8_t key[32]; assert(strlen(argv[2]) == 64 && !lmb_unhex(key, argv[2], 32));
    signal(SIGPIPE, SIG_IGN); assert(!lmb_secure_init());
    Client *clients = calloc(slots, sizeof *clients), *oracle = calloc(1, sizeof *oracle);
    assert(clients && oracle);
    unsigned available=0;
    for (unsigned i = 0; i < slots; i++) {
        clients[i].fd = connect_host(argv[1], key, &available);
        /* The previous TUI returning to its menu is not an acknowledgement
         * that the host worker has observed TCP EOF. Wait for an actually
         * idle host before measuring; never relax the exact slot counts or
         * hide a persistent leaked reservation. Later opens remain strict. */
        if (!i) {
            double deadline=seconds()+5;
            while (clients[i].fd>=0 && available<slots && seconds()<deadline) {
                lmb_close(clients[i].fd);
                struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
                clients[i].fd=connect_host(argv[1],key,&available);
            }
        }
        if (clients[i].fd<0 || available!=slots-i)
            fprintf(stderr,"initial Hosted admission: client=%u expected=%u available=%u fd=%d\n",
                    i,slots-i,available,clients[i].fd);
        assert(clients[i].fd >= 0 && available == slots-i);
    }
    int extra = connect_host(argv[1], key, &available);
    assert(extra >= 0 && available == 0); lmb_close(extra);
    /* Keep the same established slots for an idle oracle, then interleave
     * unique prompts. Replacing a prompt must not leak another slot's KV. */
    submit(&clients[0], 1, "hello"); receive_turn(&clients[0]);
    assert(!clients[0].failed && clients[0].done && clients[0].tokens);
    *oracle = clients[0];
    for (unsigned turn = 0; turn < 3; turn++) {
        pthread_t workers[LMB_HOST_MAX_SESSIONS];
        for (unsigned i = 0; i < slots; i++) {
            char unique[64]; snprintf(unique, sizeof unique, "private conversation %u", i);
            submit(&clients[i], 10 + turn * slots + i, turn == 1 ? unique : "hello");
            assert(!pthread_create(&workers[i], NULL, receive_turn, &clients[i]));
        }
        for (unsigned i = 0; i < slots; i++) {
            pthread_join(workers[i], NULL); Client *c = &clients[i];
            assert(!c->failed && c->done && c->tokens == c->metrics.generated_tokens);
            if (turn != 1) assert(c->text_len == oracle->text_len && !memcmp(c->text, oracle->text, c->text_len));
            qsort(c->gaps, c->gaps_count, sizeof(double), compare_double);
            unsigned n = c->gaps_count;
            printf("{\"slots\":%u,\"slot\":%u,\"turn\":%u,\"tokens\":%u,\"ttft_seconds\":%.9f,"
                   "\"token_notification_gap_p50_seconds\":%.9f,\"token_notification_gap_p95_seconds\":%.9f,"
                   "\"decode_tokens_per_second\":%.6f}\n", slots, i, turn, c->tokens,
                   c->first_token-c->submitted, n ? c->gaps[(n-1)/2] : 0,
                   n ? c->gaps[(95*n+99)/100-1] : 0, lmb_metrics_decode_rate(&c->metrics));
        }
    }
    for (unsigned i = 0; i < slots; i++) lmb_close(clients[i].fd);
    /* Reusing a disconnected slot must reset only that slot. */
    int reopened = -1;
    for (unsigned attempt = 0; attempt < 50; attempt++) {
        reopened = connect_host(argv[1], key, &available);
        if (reopened >= 0 && available) break;
        if (reopened >= 0) lmb_close(reopened);
        struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL);
    }
    assert(reopened >= 0 && available);
    clients[0].fd = reopened; submit(&clients[0], 200, "hello"); receive_turn(&clients[0]);
    assert(!clients[0].failed && clients[0].text_len == oracle->text_len &&
           !memcmp(clients[0].text, oracle->text, oracle->text_len));
    lmb_close(reopened); free(oracle); free(clients);
    puts("HOSTED SESSIONS: PASS (bounded admission, isolated real turns, reset/reuse, per-client latency)");
    return 0;
}
