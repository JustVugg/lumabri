/* Native input for the common donor view. Only the same-user keeper API can
 * approve or release an allocation. Closing this view leaves sharing alive. */
#ifndef LMB_DONOR_UI_WINDOWS_H
#define LMB_DONOR_UI_WINDOWS_H
#include "src/runtime/lumabri_service.h"
#include "lumabri_donor_view.h"

static int lmb_donor_ui_windows(const char *tracker) {
    LmbConsole console={0};
    if (lmb_console_open(&console)) {
        fputs("Share resources needs an interactive Windows console. Use --headless for the service only.\n",stderr);
        return 1;
    }
    fputs("\033[?1049h\033[?25l",stdout);
    HomeServiceSnapshot s={0};uint8_t viewed[32]={0};
    uint64_t seen=UINT64_MAX;int choice=1,rc=0;const char *error=NULL;
    for (;;) {
        if (home_service_query("donor",HOME_SVC_STATUS,NULL,&s)) {
            error="Sharing service unavailable. No request was approved.";rc=1;break;
        }
        if (tracker && strcmp(tracker,s.tracker)) {
            error="Stop the sharing service before joining another household.";rc=1;break;
        }
        if (seen!=s.revision || memcmp(viewed,s.offer.id,32)) {
            choice=1;seen=s.revision;memcpy(viewed,s.offer.id,32);
        }
        LmbDonorView view={.thread_capacity=s.threads,.display_models=s.model_count,
            .display_active=s.compute_active,.display_queued=s.compute_queued,
            .display_reserved=s.reserved_total,.retained=(int)s.retained,
            .persistent=1,.resident=1,.display_compute=s.compute_enabled,.log=""};
        view.transaction.phase=(LmbHomePhase)s.phase;view.transaction.offer=s.offer;
        snprintf(view.transaction.reason,sizeof view.transaction.reason,"%.159s",s.detail);
        lmb_donor_screen(&view,s.name,s.ram,1,choice);
        int resized=0,key=lmb_console_key(&console,100,&resized);
        if (key==27 || key==3 || key=='q') break;
        if (key=='j' || key=='k') choice=1-choice;
        if ((key=='\r' || key=='\n') && ui_w>=60 && ui_h>=28) key=choice ? 'n' : 'y';
        uint32_t op=key=='\t' ? HOME_SVC_NEXT_MODEL : key=='s' ? HOME_SVC_STOP :
            key=='x' ? HOME_SVC_UNLOAD : key=='y' && s.phase==LMB_HOME_PENDING ? HOME_SVC_ACCEPT :
            key=='n' && s.phase==LMB_HOME_PENDING ? HOME_SVC_DECLINE : HOME_SVC_STATUS;
        if (op && home_service_query("donor",op,&s,&s)) {
            error="Request changed or the service did not confirm the action. Reopen Share resources to refresh.";rc=1;break;
        }
        if (op==HOME_SVC_STOP) break;
    }
    fputs("\033[?25h\033[?1049l",stdout);fflush(stdout);lmb_console_close(&console);
    if (error) fprintf(stderr,"%s\n",error);
    return rc;
}
#endif
