/* Immutable household Hybrid permissions, independent of discovery. */
#ifndef LUMABRI_HOME_HYBRID_H
#define LUMABRI_HOME_HYBRID_H
#include "lumabri_proto.h"
#include <stddef.h>
#include <arpa/inet.h>
#define LMB_HOME_HYBRID_MAX 31u
#define LMB_HOME_HYBRID_ENV 10000u
enum { LMB_HYBRID_NONE, LMB_HYBRID_COORDINATOR, LMB_HYBRID_ACCELERATOR };
typedef struct {
    uint8_t key[32];
    uint32_t begin, end;
    char addr[64];
} LmbHybridPeer;
typedef struct {
    uint8_t allocation[32], root[32];
    uint32_t count;
    LmbHybridPeer peers[LMB_HOME_HYBRID_MAX];
} LmbHybridRoutes;
static LMB_MAYBE_UNUSED int lmb_hybrid_nonzero(const uint8_t *p, size_t n) {
    uint8_t v = 0; while (n--) v |= *p++; return v != 0;
}
static LMB_MAYBE_UNUSED int lmb_hybrid_routes_valid(const LmbHybridRoutes *r, int endpoints) {
    if (!r || !r->count || r->count > LMB_HOME_HYBRID_MAX ||
        !lmb_hybrid_nonzero(r->allocation, 32) || !lmb_hybrid_nonzero(r->root, 32)) return 0;
    for (uint32_t i = 0; i < r->count; i++) {
        const LmbHybridPeer *p = &r->peers[i];
        if (!lmb_hybrid_nonzero(p->key, 32) || p->begin >= p->end || p->end > 512 ||
            (i && p->begin < r->peers[i-1].end)) return 0;
        for (uint32_t j = 0; j < i; j++) if (!memcmp(p->key, r->peers[j].key, 32)) return 0;
        if (endpoints) {
            char ip[64]; memcpy(ip, p->addr, sizeof ip); ip[63] = 0;
            char *colon = strchr(ip, ':');
            if (!memchr(p->addr, 0, sizeof p->addr) || !colon) return 0;
            *colon++ = 0; char *end; errno = 0;
            long port = strtol(colon, &end, 10); struct in_addr parsed;
            if (errno || !*colon || *end || port < 1 || port > 65535 || inet_pton(AF_INET, ip, &parsed) != 1) return 0;
        } else if (p->addr[0]) return 0;
    }
    return 1;
}
static LMB_MAYBE_UNUSED int lmb_hybrid_routes_pack(LmbBuf *b, const LmbHybridRoutes *r, int endpoints) {
    if (!lmb_hybrid_routes_valid(r, endpoints) || lmb_buf_bytes(b, r->allocation, 32) ||
        lmb_buf_bytes(b, r->root, 32) || lmb_buf_u32(b, r->count)) return -1;
    for (uint32_t i = 0; i < r->count; i++) {
        const LmbHybridPeer *p = &r->peers[i];
        if (lmb_buf_bytes(b, p->key, 32) || lmb_buf_u32(b, p->begin) ||
            lmb_buf_u32(b, p->end) || (endpoints && lmb_buf_str(b, p->addr))) return -1;
    }
    return 0;
}
static LMB_MAYBE_UNUSED int lmb_hybrid_routes_unpack(LmbCur *c, LmbHybridRoutes *r, int endpoints) {
    memset(r, 0, sizeof *r);
    if (lmb_cur_bytes(c, r->allocation, 32) || lmb_cur_bytes(c, r->root, 32) ||
        lmb_cur_u32(c, &r->count) || r->count > LMB_HOME_HYBRID_MAX) return -1;
    for (uint32_t i = 0; i < r->count; i++) {
        LmbHybridPeer *p = &r->peers[i];
        if (lmb_cur_bytes(c, p->key, 32) || lmb_cur_u32(c, &p->begin) ||
            lmb_cur_u32(c, &p->end)) return -1;
        if (endpoints) {
            uint16_t len;
            if (lmb_cur_u16(c, &len) || !len || len >= sizeof p->addr ||
                lmb_cur_bytes(c, p->addr, len) || memchr(p->addr, 0, len)) return -1;
            p->addr[len] = 0;
        }
    }
    return !lmb_hybrid_routes_valid(r, endpoints) ? -1 : 0;
}
static LMB_MAYBE_UNUSED int lmb_hybrid_routes_match(const LmbHybridRoutes *approved, const LmbHybridRoutes *ready) {
    if (!lmb_hybrid_routes_valid(approved, 0) || !lmb_hybrid_routes_valid(ready, 1) ||
        approved->count != ready->count || memcmp(approved->allocation, ready->allocation, 32) ||
        memcmp(approved->root, ready->root, 32)) return 0;
    for (uint32_t i = 0; i < approved->count; i++)
        if (memcmp(approved->peers[i].key, ready->peers[i].key, 32) ||
            approved->peers[i].begin != ready->peers[i].begin || approved->peers[i].end != ready->peers[i].end) return 0;
    return 1;
}
#endif
