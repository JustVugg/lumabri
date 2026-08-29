#include "lumabri_scheduler.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    LmbLatencyPredictor fast, slow;
    lmb_predict_init(&fast, 1000);
    lmb_predict_init(&slow, 5000);
    for (int i = 0; i < 24; i++) {
        lmb_predict_observe(&fast, i == 8 ? 50000 : 1000);
        lmb_predict_observe(&slow, 5000);
    }
    assert(lmb_predict_score(&fast, 0) < lmb_predict_score(&slow, 0));
    assert(lmb_predict_score(&fast, 8) > lmb_predict_score(&slow, 0));
    assert(lmb_predict_hedge_ms(&fast) > 0);
    lmb_predict_failure(&fast, 100);
    lmb_predict_failure(&fast, 100);
    assert(lmb_predict_available(&fast, 100));
    lmb_predict_failure(&fast, 100);
    assert(!lmb_predict_available(&fast, 100));
    assert(lmb_predict_available(&fast, 1100));
    lmb_predict_observe(&fast, 900);
    assert(lmb_predict_available(&fast, 100));
    puts("PREDICTIVE SCHEDULER: PASS (EWMA, queue, p95 hedge, circuit breaker)");
    return 0;
}
