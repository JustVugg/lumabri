/* What a computer measured about itself and about its links, as facts.
 *
 * The speed is a bounded machine probe, per role: the rate at which this
 * computer streams weight bytes through a row kernel with the thread count
 * its installed engine would use. It is not a model benchmark and never a
 * tok/s figure by itself; the planner turns it into an estimate and prefers
 * an observation of the actual checkpoint whenever one exists.
 *
 * A link is this computer's own authenticated echo to another household
 * computer: application round trip and effective byte rate, as in
 * lumabri_link_evidence.h. Integer-only on the wire; zero means unknown. */
#ifndef LUMABRI_MACHINE_SPEED_H
#define LUMABRI_MACHINE_SPEED_H
#include <stdint.h>
#include <string.h>

enum { LMB_SPEED_SEGMENT = 1u, LMB_SPEED_EDGE = 2u };
#define LMB_MACHINE_SPEED_TTL_SECONDS 604800u  /* a week; hardware rarely changes */
#define LMB_MACHINE_LINKS_MAX 31u
#define LMB_MACHINE_LINK_TTL_SECONDS 900u

typedef struct {
    uint32_t known;                /* LMB_SPEED_* roles that were measured */
    uint64_t segment_bytes_per_s;  /* int8 row kernel over a buffer larger than cache */
    uint64_t edge_bytes_per_s;     /* float32 head-style projection */
    uint32_t threads;              /* threads the probe ran with */
    uint32_t age_seconds;          /* since the measurement */
} LmbMachineSpeed;

typedef struct {
    uint8_t peer[32];
    uint32_t rtt_micros;           /* median application round trip */
    uint64_t bytes_per_s;          /* 64 KiB echo, both directions counted */
    uint32_t age_seconds;
} LmbMachineLink;

typedef struct {
    uint32_t count;
    LmbMachineLink links[LMB_MACHINE_LINKS_MAX];
} LmbMachineLinks;

static inline int lmb_machine_speed_valid(const LmbMachineSpeed *s) {
    if (!s || (s->known & ~3u)) return 0;
    if (!s->known) return !s->segment_bytes_per_s && !s->edge_bytes_per_s && !s->threads && !s->age_seconds;
    if (!s->threads || s->threads > 256 || s->age_seconds > LMB_MACHINE_SPEED_TTL_SECONDS) return 0;
    if (!!(s->known & LMB_SPEED_SEGMENT) != !!s->segment_bytes_per_s ||
        !!(s->known & LMB_SPEED_EDGE) != !!s->edge_bytes_per_s) return 0;
    /* Above a terabyte per second is not a measurement of a home computer. */
    return s->segment_bytes_per_s <= UINT64_C(1000000000000) && s->edge_bytes_per_s <= UINT64_C(1000000000000);
}

static inline int lmb_machine_links_valid(const LmbMachineLinks *l, const uint8_t self[32]) {
    if (!l || l->count > LMB_MACHINE_LINKS_MAX) return 0;
    for (uint32_t i = 0; i < l->count; i++) {
        const LmbMachineLink *x = &l->links[i];
        uint8_t any = 0;
        for (unsigned j = 0; j < 32; j++) any |= x->peer[j];
        if (!any || (self && !memcmp(x->peer, self, 32)) || !x->rtt_micros || x->rtt_micros > 60000000u ||
            !x->bytes_per_s || x->bytes_per_s > UINT64_C(1000000000000) ||
            x->age_seconds > LMB_MACHINE_LINK_TTL_SECONDS) return 0;
        for (uint32_t j = 0; j < i; j++) if (!memcmp(x->peer, l->links[j].peer, 32)) return 0;
    }
    return 1;
}
#endif
