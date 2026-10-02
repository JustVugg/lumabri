/* Optional resource facts. A missing reading is not zero load/free energy.
 * Prices and power estimates are operator declarations, never measurements.
 * Keep the wire representation integer-only and independent of C padding. */
#ifndef LUMABRI_RESOURCE_FACTS_H
#define LUMABRI_RESOURCE_FACTS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lumabri_families.h"

enum { LMB_FACT_LOAD = 1u, LMB_FACT_PRICE = 2u, LMB_FACT_POWER = 4u };
typedef struct {
    uint32_t known;
    uint32_t load_milli;           /* OS one-minute load; NOT percent CPU */
    uint64_t price_micro_per_hour; /* declared currency units / whole machine */
    uint32_t power_milliwatts;     /* declared average, not live sensor output */
    char currency[4];
} LmbResourceFacts;

static LMB_UNUSED int lmb_resource_facts_valid(const LmbResourceFacts *f) {
    if (!f || (f->known & ~7u) || f->load_milli > 65536000u ||
        f->power_milliwatts > 100000000u || f->price_micro_per_hour > UINT64_C(1000000000000)) return 0;
    if (!(f->known & LMB_FACT_LOAD) && f->load_milli) return 0;
    if (!(f->known & LMB_FACT_POWER) && f->power_milliwatts) return 0;
    if (f->known & LMB_FACT_PRICE) {
        if (f->currency[3]) return 0;
        for (unsigned i = 0; i < 3; i++)
            if (f->currency[i] < 'A' || f->currency[i] > 'Z') return 0;
    } else if (f->price_micro_per_hour || memcmp(f->currency, "\0\0\0", 4)) return 0;
    return 1;
}

/* Bounded fixed-point parsing avoids locale-dependent prices and NaN/Inf.
 * A malformed optional declaration stays unknown, including a lone currency. */
static LMB_UNUSED int lmb_resource_decimal(const char *s, unsigned decimals,
    uint64_t maximum, uint64_t *out) {
    if (!s || !*s || !out || decimals > 6 || strlen(s) > 32) return -1;
    uint64_t whole = 0, fraction = 0, scale = 1;
    for (unsigned i = 0; i < decimals; i++) scale *= 10;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        unsigned digit = (unsigned)(*s++ - '0');
        if (digit > maximum || whole > maximum / 10 || whole * 10 > maximum - digit) return -1;
        whole = whole * 10 + digit;
    }
    unsigned digits = 0;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (++digits > decimals) return -1;
            fraction = fraction * 10 + (unsigned)(*s++ - '0');
        }
        if (!digits) return -1;
    }
    if (*s) return -1;
    while (digits++ < decimals) fraction *= 10;
    if (fraction > maximum || whole > (maximum - fraction) / scale) return -1;
    *out = whole * scale + fraction;
    return 0;
}

static LMB_UNUSED LmbResourceFacts lmb_resource_facts_local(double load) {
    LmbResourceFacts f = {0};
    if (isfinite(load) && load >= 0 && load <= 65536) {
        f.known |= LMB_FACT_LOAD;
        f.load_milli = (uint32_t)(load * 1000);
    }
    const char *price = getenv("LUMABRI_COST_PER_HOUR"), *currency = getenv("LUMABRI_COST_CURRENCY");
    uint64_t value;
    if (currency && strlen(currency) == 3 &&
        !lmb_resource_decimal(price, 6, UINT64_C(1000000000000), &value)) {
        f.known |= LMB_FACT_PRICE; f.price_micro_per_hour = value;
        memcpy(f.currency, currency, 3);
        if (!lmb_resource_facts_valid(&f)) {
            f.known &= ~LMB_FACT_PRICE; f.price_micro_per_hour = 0; memset(f.currency, 0, 4);
        }
    }
    if (!lmb_resource_decimal(getenv("LUMABRI_ESTIMATED_POWER_WATTS"), 3, 100000000u, &value)) {
        f.known |= LMB_FACT_POWER; f.power_milliwatts = (uint32_t)value;
    }
    return f;
}
#endif
