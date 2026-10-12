/* Bounded gateway demand observations. Owned by the parent process; children
 * send small, nonblocking datagrams over private inherited socket pairs.
 * No prompts, usernames, transcript bytes, weight I/O or persistent counters.
 * A restarted observer starts a new window, never invents an idle history. */
#ifndef LMB_DEMAND_H
#define LMB_DEMAND_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define LMB_DEMAND_MODELS 64u
#define LMB_DEMAND_WINDOW 60u
#define LMB_DEMAND_SAMPLES 64u
enum { LMB_DEMAND_START=1, LMB_DEMAND_ADMIT, LMB_DEMAND_END };
enum { LMB_DEMAND_COMPLETE, LMB_DEMAND_CAPACITY, LMB_DEMAND_FAILED,
       LMB_DEMAND_CANCELLED, LMB_DEMAND_INVALID, LMB_DEMAND_INTERRUPTED,
       LMB_DEMAND_OUTCOMES };
enum { LMB_DEMAND_STARTED=LMB_DEMAND_OUTCOMES, LMB_DEMAND_ADMITTED,
       LMB_DEMAND_COUNTERS };
typedef struct {
    uint32_t sequence, kind, outcome, reserved;
    uint8_t model[32];
    uint64_t ttft_us, completion_us;
} LmbDemandEvent;
_Static_assert(sizeof(LmbDemandEvent)<=128,"demand events must remain small");
typedef struct {
    uint64_t second, count[LMB_DEMAND_COUNTERS];
} LmbDemandBucket;
typedef struct { uint64_t at, ttft_us, completion_us; } LmbDemandSample;
typedef struct {
    uint8_t id[32]; uint32_t pending, admitted, sample_cursor;
    uint64_t last_activity;
    LmbDemandBucket buckets[LMB_DEMAND_WINDOW];
    LmbDemandSample samples[LMB_DEMAND_SAMPLES];
} LmbDemandModel;
typedef struct {
    uint64_t started_ms, captured_ms, loss_ms, loss_count, refused_connections;
    uint32_t uncertain_workers;
    uint8_t instance[32];
    LmbDemandModel models[LMB_DEMAND_MODELS];
} LmbDemand;
typedef struct { int model; uint32_t sequence, admitted, ended, uncertain; } LmbDemandWorker;
typedef struct {
    uint64_t counts[LMB_DEMAND_COUNTERS], ttft_p50_us, ttft_p95_us,
        completion_p50_us, completion_p95_us;
    uint32_t ttft_samples, completion_samples;
} LmbDemandSummary;
static inline void lmb_demand_loss(LmbDemand *s,uint64_t now) {
    s->loss_ms=now; s->loss_count++;
}
static inline void lmb_demand_worker_loss(LmbDemand *s,LmbDemandWorker *w,uint64_t now) {
    if (!w->uncertain) { w->uncertain=1; s->uncertain_workers++; }
    lmb_demand_loss(s,now);
}
static inline int lmb_demand_window_complete(const LmbDemand *s,uint64_t now) {
    return s->started_ms && !s->uncertain_workers && now>=s->started_ms &&
        now-s->started_ms>=LMB_DEMAND_WINDOW*1000 && now>=s->captured_ms && now-s->captured_ms<=1000 &&
        (!s->loss_count || (now>=s->loss_ms && now-s->loss_ms>=(LMB_DEMAND_WINDOW+1)*1000));
}
static inline void lmb_demand_count(LmbDemandModel *m,unsigned counter,uint64_t now) {
    uint64_t second=now/1000;
    LmbDemandBucket *b=&m->buckets[second%LMB_DEMAND_WINDOW];
    if (b->second!=second) { memset(b,0,sizeof *b); b->second=second; }
    b->count[counter]++; m->last_activity=now;
}
static inline int lmb_demand_model(LmbDemand *s,const uint8_t id[32],uint64_t now) {
    unsigned nonzero=0; for (unsigned i=0;i<32;i++) nonzero|=id[i];
    if (!nonzero) return -1;
    int vacant=-1;
    for (unsigned i=0;i<LMB_DEMAND_MODELS;i++) {
        LmbDemandModel *m=&s->models[i];
        if (!memcmp(m->id,id,32)) return (int)i;
        if (!m->pending && !m->admitted && (!m->last_activity ||
            (now>=m->last_activity && now-m->last_activity>=(LMB_DEMAND_WINDOW+1)*1000))) vacant=(int)i;
    }
    if (vacant>=0) { memset(&s->models[vacant],0,sizeof s->models[vacant]); memcpy(s->models[vacant].id,id,32); }
    return vacant;
}
static inline int lmb_demand_event(LmbDemand *s,LmbDemandWorker *w,const LmbDemandEvent *e,uint64_t now) {
    if (e->sequence!=w->sequence+1 || w->ended || e->reserved ||
        e->kind<LMB_DEMAND_START || e->kind>LMB_DEMAND_END) goto bad;
    if (e->kind==LMB_DEMAND_START) {
        if (w->sequence || w->model>=0 || e->ttft_us || e->completion_us || e->outcome) goto bad;
        int index=lmb_demand_model(s,e->model,now); if (index<0) goto bad;
        w->model=index; s->models[index].pending++;
        lmb_demand_count(&s->models[index],LMB_DEMAND_STARTED,now);
    } else {
        if (w->model<0 || w->model>=(int)LMB_DEMAND_MODELS) goto bad;
        LmbDemandModel *m=&s->models[w->model];
        if (memcmp(e->model,m->id,32)) goto bad;
        if (e->kind==LMB_DEMAND_ADMIT) {
            if (w->admitted || !m->pending || e->outcome || e->ttft_us || e->completion_us) goto bad;
            m->pending--; m->admitted++; w->admitted=1;
            lmb_demand_count(m,LMB_DEMAND_ADMITTED,now);
        } else {
            if (e->outcome>=LMB_DEMAND_OUTCOMES ||
                (e->outcome==LMB_DEMAND_COMPLETE && (!w->admitted || !e->completion_us)) ||
                (e->outcome==LMB_DEMAND_CAPACITY && w->admitted) ||
                e->ttft_us>e->completion_us || e->completion_us>UINT64_C(3600000000)) goto bad;
            uint32_t *active=w->admitted ? &m->admitted : &m->pending;
            if (!*active) goto bad;
            (*active)--; w->ended=1;
            lmb_demand_count(m,e->outcome,now);
            if (e->outcome==LMB_DEMAND_COMPLETE) {
                m->samples[m->sample_cursor++%LMB_DEMAND_SAMPLES]=(LmbDemandSample){now,e->ttft_us,e->completion_us};
                if (m->sample_cursor==UINT32_MAX) m->sample_cursor%=LMB_DEMAND_SAMPLES;
            }
        }
    }
    w->sequence=e->sequence; return 0;
bad:
    lmb_demand_worker_loss(s,w,now); return -1;
}
static inline void lmb_demand_reap(LmbDemand *s,LmbDemandWorker *w,int uncertain,uint64_t now) {
    if (uncertain) lmb_demand_loss(s,now);
    if (w->model>=0 && !w->ended) {
        LmbDemandModel *m=&s->models[w->model];
        uint32_t *active=w->admitted ? &m->admitted : &m->pending;
        if (*active) (*active)--; else lmb_demand_loss(s,now);
        lmb_demand_count(m,LMB_DEMAND_INTERRUPTED,now);
        lmb_demand_loss(s,now);
    }
    if (w->uncertain && s->uncertain_workers) s->uncertain_workers--;
    *w=(LmbDemandWorker){.model=-1};
}
static inline int lmb_demand_compare(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b; return (x>y)-(x<y);
}
static inline void lmb_demand_summary(const LmbDemandModel *m,uint64_t now,LmbDemandSummary *out) {
    memset(out,0,sizeof *out); uint64_t second=now/1000,ttft[LMB_DEMAND_SAMPLES],total[LMB_DEMAND_SAMPLES];
    for (unsigned i=0;i<LMB_DEMAND_WINDOW;i++) {
        const LmbDemandBucket *b=&m->buckets[i];
        if (second<b->second || second-b->second>=LMB_DEMAND_WINDOW) continue;
        for (unsigned j=0;j<LMB_DEMAND_COUNTERS;j++) out->counts[j]+=b->count[j];
    }
    for (unsigned i=0;i<LMB_DEMAND_SAMPLES;i++) {
        const LmbDemandSample *p=&m->samples[i];
        if (!p->at || now<p->at || second-p->at/1000>=LMB_DEMAND_WINDOW) continue;
        if (p->ttft_us) ttft[out->ttft_samples++]=p->ttft_us;
        total[out->completion_samples++]=p->completion_us;
    }
    qsort(ttft,out->ttft_samples,sizeof *ttft,lmb_demand_compare);
    qsort(total,out->completion_samples,sizeof *total,lmb_demand_compare);
    if (out->ttft_samples) {
        out->ttft_p50_us=ttft[(out->ttft_samples*50+99)/100-1];
        out->ttft_p95_us=ttft[(out->ttft_samples*95+99)/100-1];
    }
    if (out->completion_samples) {
        out->completion_p50_us=total[(out->completion_samples*50+99)/100-1];
        out->completion_p95_us=total[(out->completion_samples*95+99)/100-1];
    }
}
#endif
