#include "src/planner/lumabri_catalogue_advice.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    LmbAdviceCandidate c[] = {{1, 20, 10, 3}, {1, 40, 30, 7}, {0, 1, 100, 999}};
    uint32_t f[3];
    lmb_catalogue_advice(c, 3, f);
    assert(f[0] == LMB_ADVICE_LOWEST_RAM);
    assert(f[1] == (LMB_ADVICE_LARGEST_CHECKPOINT | LMB_ADVICE_FASTEST_OBSERVED));
    assert(!f[2]); /* Unrunnable/unsupported entries never win advice. */
    c[1].measured_tok_s = 0; /* changed plan or no measurement */
    lmb_catalogue_advice(c, 3, f);
    assert(!(f[0] & LMB_ADVICE_FASTEST_OBSERVED) && !(f[1] & LMB_ADVICE_FASTEST_OBSERVED));
    c[1].measured_tok_s = NAN; lmb_catalogue_advice(c, 3, f);
    assert(!(f[1] & LMB_ADVICE_FASTEST_OBSERVED));
    c[1].measured_tok_s = INFINITY; lmb_catalogue_advice(c, 3, f);
    assert(!(f[1] & LMB_ADVICE_FASTEST_OBSERVED));
    c[1].reserved_bytes = UINT64_MAX; lmb_catalogue_advice(c, 3, f);
    assert(!f[0] && !f[1] && !f[2]);
    c[1] = c[0]; lmb_catalogue_advice(c, 3, f);
    assert(f[0] == 7 && !f[1]); /* stable first-in-catalogue tie-break */
    c[0].eligible = c[1].eligible = 0; lmb_catalogue_advice(c, 3, f);
    assert(!f[0] && !f[1] && !f[2]);
    f[0] = 7; lmb_catalogue_advice(NULL, 3, f); assert(!f[0]);
    lmb_catalogue_advice(NULL, 0, NULL);
    assert(!strcmp(lmb_advice_text(0), ""));
    assert(!strcmp(lmb_advice_text(LMB_ADVICE_LOWEST_RAM), "lowest RAM reservation"));
    puts("CATALOGUE ADVICE: PASS (admitted plans only; no invented speeds or quality rankings)");
    return 0;
}
