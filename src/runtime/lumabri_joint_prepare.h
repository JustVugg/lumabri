/* Apply a reviewed joint plan through the existing per-model keeper workflow.
 * This is deliberately a sequence, not a distributed atomic transaction:
 * READY models survive rejection/failure of a later model. No approval replay. */
#ifndef LUMABRI_JOINT_PREPARE_H
#define LUMABRI_JOINT_PREPARE_H
#include "src/runtime/lumabri_joint_review.h"

static int home_prepare_portfolio_begin(const LmbTuiState *reviewed,
    const HomePreparationObserver *observer, HomeServiceSnapshot *started) {
    home_error[0] = 0;
    if (home_service_foreground() || !home_resident_required())
        return home_fail("Joint preparation requires the resident background service.");
    g_stopping = 0; install_chat_signal_handlers();
    if (!reviewed->tracker[0] || home_private_network()) return home_fail("Joint preparation needs an authenticated household.");
    LmbTuiState *current = malloc(sizeof *current);
    if (!current) return home_fail("Cannot allocate the preparation snapshot.");
    memcpy(current, reviewed, sizeof *current);
    uint32_t mapping[LMB_CLUSTER_MAX_NODES]; char why[384];
    int inventory = catalog_inventory(current);
    if (inventory || home_joint_validate(reviewed, current, 0, mapping, why, sizeof why)) {
        free(current); return home_fail("%s", inventory ? "Cannot refresh household inventory; no model was loaded." : why);
    }
    if (home_service_ensure()) { free(current); return home_fail("Cannot start the private preparation service."); }
    HomeService job;
    if (home_service_open(&job, "prepare")) {
        free(current); return home_fail("Another preparation is running. No second operation was requested.");
    }
    if (observer) {
        job.observer = *observer;
        memcpy(job.snapshot.instance, observer->instance, 32);
    }
    job.batch_total = reviewed->joint.plan.model_count;
    job.snapshot.preparation_results_known=1;
    snprintf(job.snapshot.name, sizeof job.snapshot.name, "Joint resident plan");
    snprintf(job.snapshot.tracker, sizeof job.snapshot.tracker, "%s", reviewed->tracker);
    home_service_preparation_detail(&job, "Preparing reviewed models; separate donor approvals required");
    if (home_service_save(&job)) {
        home_service_close(&job); free(current); return home_fail("Cannot persist the joint preparation operation.");
    }
    int detached = home_service_detach(&job, observer ? observer->fd : -1);
    if (detached < 0) { home_service_close(&job); free(current); return home_fail("Cannot start joint preparation keeper."); }
    if (!detached) {
        home_background_job = &job;
        int rc = 0;
        for (uint32_t model = 0; model < job.batch_total; model++) {
            if (g_stopping || home_service_key() == 3) { rc = home_fail("Preparation cancelled before the next model."); break; }
            memcpy(current, reviewed, sizeof *current);
            if (catalog_inventory(current)) { rc = home_fail("Cannot refresh the household before the next model."); break; }
            int index = home_joint_prepare_next(reviewed, current, model, why, sizeof why);
            if (index < 0) { rc = home_fail("%s", why); break; }
            current->quick_calibration = 0;
            rc = home_request_chat_direct(current, index);
            if (rc) break;
            job.batch_completed++;
            home_service_preparation_detail(&job, "Approved model ready; its allocation is saved in Resident models");
            if (home_service_save(&job)) { rc = home_fail("Cannot persist joint preparation progress."); break; }
        }
        job.snapshot.state = rc ? HOME_SVC_FAILED : HOME_SVC_DONE;
        char outcome[480];
        if (rc) snprintf(outcome, sizeof outcome,
            "%.300s Ready models remain loaded; incomplete allocations cancelled. See Resident models.", home_error);
        else snprintf(outcome, sizeof outcome, "All selected models ready. Open conversations from Resident models.");
        home_service_preparation_detail(&job, outcome);
        if (home_service_save(&job)) rc = 1;
        home_service_answer(&job); home_service_close(&job); free(current); _exit(rc != 0);
    }
    free(current);
    *started = job.snapshot; free(job.last);
    return 0;
}

static int home_request_portfolio(const LmbTuiState *reviewed) {
    HomeServiceSnapshot started;
    if (home_prepare_portfolio_begin(reviewed, NULL, &started)) return 1;
    HomeServiceSnapshot status;
    int rc = home_prepare_wait(&started, "Prepare selected models together", &status);
    return rc < 0 ? 1 : 0;
}
#endif
