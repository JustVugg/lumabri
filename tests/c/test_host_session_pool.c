/* Deterministic queue/cancellation faults against the real host pool. The
 * codec below is deliberately a mock; model correctness is exercised by
 * household_service_flow_test.py with test_hosted_sessions and real OLMoE. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>

static void fake_codec(int input, int output) {
    FILE *in = fdopen(input, "r"), *out = fdopen(output, "w"); assert(in && out);
    setvbuf(in, NULL, _IONBF, 0); setvbuf(out, NULL, _IONBF, 0);
    char line[512], nonce[65]; unsigned id, slot, active = 0; size_t bytes;
    while (fgets(line, sizeof line, in)) {
        if (sscanf(line, "RESET_SLOT %64s %u", nonce, &slot) == 2) {
            assert(slot < 3 && !active); fprintf(out, "RESET_DONE %s\n", nonce);
        } else if (sscanf(line, "SUBMIT %u %u %zu", &id, &slot, &bytes) == 3) {
            assert(!active && slot < 3 && bytes < 32);
            char prompt[32] = {0}; assert(fread(prompt, 1, bytes, in) == bytes && fgetc(in) == '\n');
            fprintf(out, "ACCEPT %u\n", id);
            if (!strcmp(prompt, "hold")) active = id;
            else fprintf(out, "DATA %u 2\nok\nDONE %u STAT 1 0\n", id, id);
        } else if (sscanf(line, "CANCEL %u", &id) == 1) {
            if (active) { assert(id == active); active = 0; fprintf(out, "ERROR %u cancelled\n", id); }
        } else assert(!"unexpected engine command");
    }
    fclose(in); fclose(out); _exit(0);
}

static int client_connect(const char *address) {
    int fd = lmb_connect_ms_io(address, 1000, 2000); assert(fd >= 0);
    assert(!lmb_send(fd, LMB_HOST_HELLO, NULL, 0, NULL, 0));
    LmbMsg m = {0}; assert(!lmb_recv(fd, &m) && m.op == LMB_HOST_HELLO_R);
    LmbCur c = {m.body, m.body_len, 0}; char field[128]; uint32_t slots;
    for (unsigned i = 0; i < 3; i++) assert(!lmb_cur_str(&c, field, sizeof field));
    assert(!lmb_cur_u32(&c, &slots) && slots); lmb_msg_free(&m); return fd;
}

static void send_text(int fd, const char *text) {
    assert(!lmb_send(fd, LMB_HOST_STREAM, NULL, 0, text, (uint32_t)strlen(text)));
}

static void expect(int fd, const char *needle) {
    char seen[8192] = ""; size_t used = 0; double deadline = nowd()+5;
    while (!strstr(seen, needle)) {
        assert(nowd() < deadline);
        LmbMsg m = {0}; assert(!lmb_recv(fd, &m) && m.op == LMB_HOST_STREAM);
        assert(m.pay_len < sizeof seen-used);
        memcpy(seen+used, m.pay, m.pay_len); used += m.pay_len; seen[used] = 0;
        lmb_msg_free(&m);
    }
}

int main(void) {
    unsetenv("LUMABRI_TOKEN"); unsetenv("LUMABRI_ENCRYPT"); unsetenv("LUMABRI_READY_FD");
    signal(SIGPIPE, SIG_IGN);
    /* A byte-at-a-time sender must not extend an absolute frame deadline. */
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    pid_t slow = fork(); assert(slow >= 0);
    if (!slow) {
        close(pair[0]); uint8_t header[16] = {0}; lmb_put32(header, LMB_MAGIC);
        lmb_put32(header+4, LMB_HOST_STREAM); lmb_put32(header+12, 4);
        for (size_t i = 0; i < sizeof header; i++) {
            if (write(pair[1], header+i, 1) != 1) break;
            struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
        }
        close(pair[1]); _exit(0);
    }
    close(pair[1]); LmbMsg limited = {0}; double began = nowd();
    assert(lmb_recv_bounded(pair[0], &limited, 1024, 100) && errno == ETIMEDOUT);
    assert(nowd()-began < 1 && !lmb_read_deadline_ms && lmb_rx_frame_limit == UINT32_MAX);
    lmb_msg_free(&limited); close(pair[0]); assert(waitpid(slow, NULL, 0) == slow);
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (struct sockaddr *)&addr, sizeof addr) && !listen(listener, 8));
    socklen_t length = sizeof addr; assert(!getsockname(listener, (struct sockaddr *)&addr, &length));
    char address[64]; snprintf(address, sizeof address, "127.0.0.1:%u", ntohs(addr.sin_port));
    pid_t host = fork(); assert(host >= 0);
    if (!host) {
        install_chat_signal_handlers();
        int to[2], from[2]; assert(!pipe(to) && !pipe(from));
        pid_t engine = fork(); assert(engine >= 0);
        if (!engine) { close(to[1]); close(from[0]); close(listener); fake_codec(to[0], from[1]); }
        close(to[0]); close(from[1]);
        Engine e = {.pid=engine, .to=to[1], .from=from[0], .segment=1, .reset_supported=1, .session_slots=3};
        HostState h = {.engine=&e, .slots=3, .max_frame=1024, .max_new=8, .idle_seconds=10, .request_seconds=10};
        int rc = host_sessions_run(listener, &h, 400);
        close(listener); close(e.to); close(e.from); kill(engine, SIGTERM); waitpid(engine, NULL, 0);
        _exit(rc);
    }
    close(listener);
    int a = client_connect(address), b = client_connect(address), c = client_connect(address);
    send_text(a, "SUBMIT 1 0 4 8 0 1\nhold\n"); expect(a, "ACCEPT 1\n");
    send_text(b, "SUBMIT 2 0 2 8 0 1\nhi\n"); expect(b, "QUEUED");
    send_text(c, "SUBMIT 3 0 2 8 0 1\nhi\n"); expect(c, "QUEUED");
    send_text(b, "CANCEL 2\n"); LmbMsg m = {0}; assert(lmb_recv(b, &m)); lmb_msg_free(&m); lmb_close(b);
    expect(c, "ERROR 3 host queue deadline exceeded\n"); lmb_close(c);
    send_text(a, "CANCEL 1\n"); expect(a, "ERROR 1 cancelled\n");
    send_text(a, "SUBMIT 4 0 2 8 0 1\nhi\n"); expect(a, "DONE 4 ");
    b = client_connect(address);
    send_text(a, "SUBMIT 5 0 4 8 0 1\nhold\n"); expect(a, "ACCEPT 5\n");
    send_text(b, "SUBMIT 6 0 2 8 0 1\nhi\n"); expect(b, "QUEUED");
    lmb_close(a); expect(b, "DONE 6 ");
    /* A legal generic frame which exceeds this host's application bound is
     * refused BEFORE allocation or waiting for its deliberately absent body. */
    c = client_connect(address);
    uint8_t header[16]; lmb_put32(header, LMB_MAGIC); lmb_put32(header+4, LMB_HOST_STREAM);
    lmb_put32(header+8, 0); lmb_put32(header+12, 1u<<20);
    assert(!lmb_write_full(c, header, sizeof header));
    assert(lmb_recv(c, &m)); lmb_msg_free(&m); lmb_close(c);
    send_text(b, "SUBMIT 7 0 2 8 0 1\nhi\n"); expect(b, "DONE 7 "); lmb_close(b);
    kill(host, SIGTERM); int status;
    assert(waitpid(host, &status, 0) == host && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("HOST SESSION POOL: PASS (active/queued cancellation, timeout, disconnect, isolated reset, frame limits)");
    return 0;
}
