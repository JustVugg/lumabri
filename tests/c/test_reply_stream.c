#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "src/runtime/lumabri_reply_stream.h"

typedef struct { unsigned char text[512]; size_t n; int progress, errors, abort; } Capture;
static int data(void *arg, const unsigned char *p, size_t n) {
    Capture *c = arg;
    if (c->abort) return -1;
    assert(n <= sizeof c->text - c->n);
    memcpy(c->text+c->n, p, n); c->n += n; return 0;
}
static int progress(void *arg, const char *text) {
    Capture *c = arg; c->progress++; assert(!strncmp(text, "PROGRESS 7 ", 11)); return 0;
}
static int error(void *arg, const char *text) {
    Capture *c = arg; c->errors++; assert(!strcmp(text, "cancelled")); return 0;
}
static void start(LmbReplyStream *s, Capture *c, size_t limit) {
    memset(c, 0, sizeof *c);
    LmbReplySink sink = {data, progress, error, c};
    assert(!lmb_reply_init(s, "7", limit, sink));
}
int main(void) {
    LmbReplyStream s; Capture c;
    const char payload[] = "ciao\nDONE 7\n\xc3\xa8";
    char wire[512];
    int n = snprintf(wire, sizeof wire, "EMAP ignored\nACCEPT 7 24\nPROGRESS 7 PREFILL 24 24\nDATA 7 %zu\n%s\nDATA 7 0\n\nDONE 7 STAT 8 1 0 0 24 0\n",
        strlen(payload), payload);
    assert(n > 0 && (size_t)n < sizeof wire);
    for (size_t chunk = 1; chunk <= (size_t)n; chunk++) {
        start(&s, &c, 512);
        for (size_t at = 0; at < (size_t)n;) {
            size_t size = (size_t)n-at < chunk ? (size_t)n-at : chunk, used = 0;
            int rc = lmb_reply_feed(&s, wire+at, size, &used);
            assert(used == size && rc == (at+size == (size_t)n ? LMB_REPLY_DONE : LMB_REPLY_MORE));
            at += size;
        }
        assert(c.n == strlen(payload) && !memcmp(c.text, payload, c.n));
        assert(c.progress == 1 && !c.errors && !strcmp(s.stat, "STAT 8 1 0 0 24 0"));
        assert(lmb_reply_eof(&s) == LMB_REPLY_DONE);
    }
    for (size_t prefix = 0; prefix < (size_t)n; prefix++) {
        start(&s, &c, 512);
        assert(lmb_reply_feed(&s, wire, prefix, NULL) == LMB_REPLY_MORE);
        assert(lmb_reply_eof(&s) == LMB_REPLY_INVALID);
    }
    start(&s, &c, 512);
    const char *failed = "DATA 7 3\nabc\nERROR 7 cancelled\nDONE 7\n";
    size_t used;
    assert(lmb_reply_feed(&s, failed, strlen(failed), &used) == LMB_REPLY_ERROR);
    assert(c.n == 3 && c.errors == 1 && !s.stat[0] && !strcmp(failed+used, "DONE 7\n"));
    assert(lmb_reply_feed(&s, "DATA 7 1\nx\n", 11, &used) == LMB_REPLY_ERROR && used == 0 && c.n == 3);
    start(&s, &c, 512); c.abort = 1;
    assert(lmb_reply_feed(&s, "DATA 7 1\nx\n", 11, NULL) == LMB_REPLY_ABORTED && c.n == 0);
    const char *bad[] = {"DATA 7 -1\n", "DATA 7 18446744073709551616\n", "DATA 7 1 x\n",
        "DATA 7 513\n", "DATA 7 1\nx!", "DATA 8 1\nx\n", "DONE 8\n", "ERROR 8 nope\n",
        "DONE 7 nonsense\n", "DATA\nDONE 7\n", "DONE\t7\n", "PROGRESS 8 PREFILL 1 1\n",
        "ACCEPT 7 -1\n", "ACCEPT 7 4294967296\n", "ACCEPT 7 1 extra\n"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        start(&s, &c, 512);
        assert(lmb_reply_feed(&s, bad[i], strlen(bad[i]), NULL) == LMB_REPLY_INVALID);
    }
    start(&s, &c, 4);
    assert(lmb_reply_feed(&s, "DATA 7 3\nabc\nDATA 7 2\n", 22, NULL) == LMB_REPLY_INVALID);
    start(&s, &c, 512);
    char *long_line = malloc(20000); assert(long_line);
    memset(long_line, 'x', 20000); memcpy(long_line, "EMAP ", 5); long_line[19999] = '\n';
    assert(lmb_reply_feed(&s, long_line, 20000, NULL) == LMB_REPLY_MORE);
    assert(lmb_reply_feed(&s, "DONE 7\nnext", 11, &used) == LMB_REPLY_DONE && used == 7);
    start(&s, &c, 512); memcpy(long_line, "DONE 7 STAT ", 12);
    assert(lmb_reply_feed(&s, long_line, 20000, NULL) == LMB_REPLY_INVALID);
    free(long_line);
    start(&s, &c, 512);
    char flood[4096]; memset(flood, 'x', sizeof flood);
    for (size_t i = 0; i < LMB_REPLY_LINE_LIMIT / sizeof flood; i++)
        assert(lmb_reply_feed(&s, flood, sizeof flood, NULL) == LMB_REPLY_MORE);
    assert(lmb_reply_feed(&s, "x", 1, NULL) == LMB_REPLY_INVALID);
    start(&s, &c, 512);
    assert(lmb_reply_feed(&s, "DONE 7\0\n", 9, NULL) == LMB_REPLY_INVALID);
    LmbReplySink empty = {0};
    assert(lmb_reply_init(&s, "bad id", 512, empty));
    assert(lmb_reply_init(&s, "7", 0, empty));
    puts("REPLY STREAM: PASS (fragmentation, request isolation, bounds, terminal errors, callback cancellation)");
    return 0;
}
