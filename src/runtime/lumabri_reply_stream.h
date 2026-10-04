/* Incremental serve-codec reply reader shared by terminal and API consumers.
 * No sockets, terminal rendering, conversation storage or global state here.
 * DATA boundaries are byte counts: text can contain newlines and fake headers.
 * Consumers publish success only on DONE; ERROR and truncation are failures. */
#ifndef LUMABRI_REPLY_STREAM_H
#define LUMABRI_REPLY_STREAM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define LMB_REPLY_HEADER 16384u
#define LMB_REPLY_LINE_LIMIT (4u << 20) /* legacy EMAP rows may exceed a header */
enum { LMB_REPLY_MORE = 0, LMB_REPLY_DONE = 1, LMB_REPLY_ERROR = 2,
       LMB_REPLY_INVALID = -1, LMB_REPLY_ABORTED = -2 };
typedef struct {
    int (*data)(void *, const unsigned char *, size_t);
    int (*progress)(void *, const char *);
    int (*error)(void *, const char *);
    void *user;
} LmbReplySink;
typedef struct {
    char header[LMB_REPLY_HEADER], request_id[64], stat[LMB_REPLY_HEADER];
    size_t header_length, line_bytes;
    uint64_t remaining, output_bytes, output_limit;
    int terminator, overflow, status;
    LmbReplySink sink;
} LmbReplyStream;

static inline int lmb_reply_id_valid(const char *id, size_t n) {
    if (!n || n >= 64) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static inline int lmb_reply_init(LmbReplyStream *s, const char *id, uint64_t limit, LmbReplySink sink) {
    if (!s) return -1;
    memset(s, 0, sizeof *s); s->status = LMB_REPLY_INVALID;
    if (!limit || (id && !lmb_reply_id_valid(id, strnlen(id, 64)))) return -1;
    if (id) memcpy(s->request_id, id, strlen(id) + 1);
    s->output_limit = limit; s->sink = sink; s->status = LMB_REPLY_MORE; return 0;
}

static inline int lmb_reply_line(LmbReplyStream *s) {
    char *line = s->header;
    int data = !strncmp(line, "DATA ", 5), done = !strncmp(line, "DONE ", 5);
    int error = !strncmp(line, "ERROR ", 6), progress = !strncmp(line, "PROGRESS ", 9);
    int accept = !strncmp(line, "ACCEPT ", 7);
    if (!(data || done || error || progress || accept)) {
        const char *words[] = {"DATA", "DONE", "ERROR", "PROGRESS", "ACCEPT"};
        for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
            size_t n = strlen(words[i]);
            if (!strncmp(line, words[i], n) && (!line[n] || line[n] == '\t' || line[n] == '\r'))
                return LMB_REPLY_INVALID;
        }
        return LMB_REPLY_MORE;
    }
    if (s->overflow) return LMB_REPLY_INVALID;
    char *id = line + (data || done ? 5 : error ? 6 : progress ? 9 : 7);
    char *tail = strchr(id, ' ');
    size_t n = tail ? (size_t)(tail - id) : strlen(id);
    if (!lmb_reply_id_valid(id, n)) return LMB_REPLY_INVALID;
    if (s->request_id[0]) {
        if (strlen(s->request_id) != n || memcmp(s->request_id, id, n)) return LMB_REPLY_INVALID;
    } else { memcpy(s->request_id, id, n); s->request_id[n] = 0; }
    if (tail) tail++;
    if (data) {
        if (!tail || !*tail) return LMB_REPLY_INVALID;
        uint64_t bytes = 0;
        for (const char *p = tail; *p; p++) {
            if (*p < '0' || *p > '9' || bytes > (UINT64_MAX - (unsigned)(*p-'0')) / 10)
                return LMB_REPLY_INVALID;
            bytes = bytes * 10 + (unsigned)(*p-'0');
        }
        if (bytes > s->output_limit - s->output_bytes) return LMB_REPLY_INVALID;
        s->remaining = bytes; s->terminator = 1; return LMB_REPLY_MORE;
    }
    if (done) {
        if (tail && strncmp(tail, "STAT ", 5)) return LMB_REPLY_INVALID;
        if (tail) memcpy(s->stat, tail, strlen(tail) + 1);
        return LMB_REPLY_DONE;
    }
    if (error) {
        if (s->sink.error && s->sink.error(s->sink.user, tail ? tail : "Engine request failed"))
            return LMB_REPLY_ABORTED;
        return LMB_REPLY_ERROR;
    }
    if (accept) { /* monolithic engines append the accepted prompt count */
        if (tail) {
            if (!*tail) return LMB_REPLY_INVALID;
            uint64_t count = 0;
            for (const char *p = tail; *p; p++) {
                if (*p < '0' || *p > '9' || count > (UINT32_MAX - (unsigned)(*p-'0')) / 10)
                    return LMB_REPLY_INVALID;
                count = count * 10 + (unsigned)(*p-'0');
            }
        }
        return LMB_REPLY_MORE;
    }
    if (!tail || !*tail) return LMB_REPLY_INVALID;
    return s->sink.progress && s->sink.progress(s->sink.user, line) ? LMB_REPLY_ABORTED : LMB_REPLY_MORE;
}

/* consumed leaves any bytes AFTER a terminal frame with the caller. No
 * callback is invoked again after success, failure or a consumer cancellation. */
static inline int lmb_reply_feed(LmbReplyStream *s, const void *bytes, size_t length, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!s || (!bytes && length)) return LMB_REPLY_INVALID;
    const unsigned char *p = bytes; size_t at = 0;
    while (at < length && s->status == LMB_REPLY_MORE) {
        if (s->remaining) {
            size_t n = length - at;
            if ((uint64_t)n > s->remaining) n = (size_t)s->remaining;
            if (s->sink.data && s->sink.data(s->sink.user, p+at, n)) {
                s->status = LMB_REPLY_ABORTED; break;
            }
            s->output_bytes += n; s->remaining -= n; at += n; continue;
        }
        unsigned char c = p[at++];
        if (s->terminator) {
            s->terminator = 0;
            if (c != '\n') s->status = LMB_REPLY_INVALID;
            continue;
        }
        if (c == '\n') {
            s->header[s->header_length] = 0;
            s->status = lmb_reply_line(s);
            s->header_length = s->line_bytes = 0; s->overflow = 0;
        } else if (!c || ++s->line_bytes > LMB_REPLY_LINE_LIMIT) s->status = LMB_REPLY_INVALID;
        else if (s->header_length + 1 < sizeof s->header) s->header[s->header_length++] = (char)c;
        else s->overflow = 1;
    }
    if (consumed) *consumed = at;
    return s->status;
}

static inline int lmb_reply_eof(LmbReplyStream *s) {
    if (!s) return LMB_REPLY_INVALID;
    if (s->status == LMB_REPLY_MORE) s->status = LMB_REPLY_INVALID;
    return s->status;
}
#endif
