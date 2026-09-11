/* Bounded observer of our checkpoint source's versioned progress records.
 * These counters describe source work, not donor RAM residency or chat READY. */
#ifndef LUMABRI_PREPARE_PROGRESS_H
#define LUMABRI_PREPARE_PROGRESS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    uint64_t done, total, elapsed_ms;
    double rate, observed_at;
    unsigned samples;
} LmbPrepareCounter;

typedef struct {
    LmbPrepareCounter index, transfer;
    off_t offset;
    dev_t device;
    ino_t inode;
    char line[192];
    size_t used;
    int overflow, opened;
} LmbPrepareProgress;

static void lmb_prepare_record(LmbPrepareProgress *p, const char *line, double now) {
    char kind[16], extra;
    unsigned long long done, total, ms;
    if (strncmp(line, "LMB_PREPARE_V1 ", 15) || strchr(line, '-') ||
        sscanf(line, "LMB_PREPARE_V1 %15s %llu %llu %llu %c",
               kind, &done, &total, &ms, &extra) != 4) return;
    LmbPrepareCounter *c;
    if (!strcmp(kind, "INDEX")) {
        if (!total || done > total) return;
        c = &p->index;
    } else if (!strcmp(kind, "TRANSFER")) {
        if (total) return; /* source traffic has no known download denominator */
        c = &p->transfer;
    } else return;
    if (c->samples && (done < c->done || ms < c->elapsed_ms || total != c->total)) {
        memset(c, 0, sizeof *c); /* restarted source: yesterday's ETA is invalid */
    }
    if (c->samples && ms > c->elapsed_ms && done > c->done) {
        double rate = (double)(done - c->done) * 1000 / (ms - c->elapsed_ms);
        c->rate = c->rate > 0 ? .5 * c->rate + .5 * rate : rate;
    }
    if (!c->samples || done != c->done) c->observed_at = now;
    c->done = done; c->total = total; c->elapsed_ms = ms;
    if (c->samples < 1000000) c->samples++;
}

static void lmb_prepare_read(LmbPrepareProgress *p, const char *path, double now) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return; }
    if (p->opened && (st.st_dev != p->device || st.st_ino != p->inode || st.st_size < p->offset))
        memset(p, 0, sizeof *p);
    p->device = st.st_dev; p->inode = st.st_ino; p->opened = 1;
    char buf[8192]; /* one bounded read per UI tick, even for a noisy engine */
    ssize_t n = pread(fd, buf, sizeof buf, p->offset);
    close(fd);
    if (n <= 0) return;
    p->offset += n;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            p->line[p->used] = 0;
            if (!p->overflow) lmb_prepare_record(p, p->line, now);
            p->used = 0; p->overflow = 0;
        } else if (p->used < sizeof p->line - 1) p->line[p->used++] = buf[i];
        else p->overflow = 1;
    }
}

static void lmb_prepare_display(const LmbPrepareCounter *c, int indexing,
                                double now, char bar[29], char *detail, size_t cap) {
    int known = indexing && c->total;
    unsigned filled = known ? (unsigned)(26.0 * c->done / c->total) : 0;
    if (filled > 26) filled = 26;
    if (known && c->done < c->total && filled == 26) filled = 25;
    unsigned moving = isfinite(now) && now >= 0 && now < 1e12 ?
                      (unsigned)((uint64_t)(now * 4) % 24) : 0;
    bar[0] = '[';
    for (unsigned i = 0; i < 26; i++)
        bar[i + 1] = known ? (i < filled ? '=' : ' ') :
                     (i >= moving && i < moving + 3 ? '=' : ' ');
    bar[27] = ']'; bar[28] = 0;
    if (!indexing) {
        snprintf(detail, cap, "%.1f MB served from this computer | remaining time unavailable",
                 c->done / 1e6);
        return;
    }
    if (!known) { snprintf(detail, cap, "Determining checkpoint size | estimating..."); return; }
    char eta[64] = "estimating...";
    if (c->done == c->total) snprintf(eta, sizeof eta, "identity checks finishing");
    else if (c->samples >= 2 && c->rate > 0 && now - c->observed_at <= 10) {
        double seconds = (c->total - c->done) / c->rate;
        if (seconds < 60) snprintf(eta, sizeof eta, "about %.0f s remaining", seconds < 1 ? 1 : seconds);
        else if (seconds < 3600) snprintf(eta, sizeof eta, "about %.0f min remaining", seconds / 60);
        else snprintf(eta, sizeof eta, "about %.1f h remaining", seconds / 3600);
    } else if (c->samples >= 2 && now - c->observed_at > 10)
        snprintf(eta, sizeof eta, "waiting for progress; ETA unavailable");
    unsigned percent = (unsigned)(100.0 * c->done / c->total);
    if (c->done < c->total && percent >= 100) percent = 99;
    snprintf(detail, cap, "%u%% | %.1f / %.1f MB verified | %s",
             percent, c->done / 1e6, c->total / 1e6, eta);
}
#endif
