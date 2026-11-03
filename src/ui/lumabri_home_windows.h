/* Native household launcher. Services retain resource ownership after the
 * console detaches; editable values are committed only after confirmation. */
#ifndef LMB_HOME_WINDOWS_H
#define LMB_HOME_WINDOWS_H
#include "src/runtime/lumabri_home_settings_windows.h"
#include "src/runtime/lumabri_household_windows.h"

static void home_windows_close(LmbConsole *console) {
    fputs("\033[?25h\033[?1049l",stdout);fflush(stdout);lmb_console_close(console);
}
static int home_windows_open(LmbConsole *console) {
    if (lmb_console_open(console)) return -1;
    fputs("\033[?1049h\033[?25l",stdout);return 0;
}
static void home_windows_message(const char *title,const char *detail) {
    LmbConsole console={0};
    if (home_windows_open(&console)) {fprintf(stderr,"%s: %s\n",title,detail);return;}
    for (;;) {
        ui_begin("your workspace");ui_text(5,4,UI_SAND,title);
        int width=ui_w>12 ? ui_w-8 : 10;const char *p=detail;
        for (int row=8;*p && row<ui_h-4;row++) {
            char line[256];size_t n=strlen(p);
            if (n>(size_t)width) n=(size_t)width;
            if (n>=sizeof line) n=sizeof line-1;
            if (p[n]) {size_t space=n;while (space && p[space]!=' ') space--;if (space) n=space;}
            memcpy(line,p,n);line[n]=0;ui_text(row,4,UI_TEXT,line);p+=n;while (*p==' ') p++;
        }
        ui_footer("Settings change only after a successful save.","Enter continue   Esc back");ui_present();
        int resized=0,key=lmb_console_key(&console,100,&resized);
        if (key=='\r' || key=='\n' || key==27 || key==3) break;
    }
    home_windows_close(&console);
}

static int home_windows_field(const char *label,char *value,size_t cap,int secret) {
    LmbConsole console={0};char edit[1200];
    if (!cap || cap>sizeof edit || strnlen(value,cap)==cap || home_windows_open(&console)) return -1;
    strcpy(edit,value);size_t used=strlen(edit);int result=-1;
    for (;;) {
        ui_begin("household settings");ui_text(5,4,UI_TEXT,label);
        char shown[1200];
        if (secret) {size_t n=used<60 ? used : 60;memset(shown,'*',n);shown[n]=0;}
        else snprintf(shown,sizeof shown,"%s",edit);
        ui_text(9,4,UI_SAND,shown[0] ? shown : "Type here…");
        ui_text(12,4,UI_MUTED,secret ? "The household key stays hidden." : "Your saved value is editable.");
        ui_footer("Backspace removes the last character. Ctrl-U clears the field.","Enter confirm   Esc cancel");
        int compact=ui_w<60 || ui_h<24;
        if (compact) {ui_begin("household settings");ui_text(5,4,UI_SAND,"Resize to at least 60 × 24. Esc cancels.");}
        ui_present();int resized=0,key=lmb_console_input(&console,100,&resized,1);
        if (key==27 || key==3) break;
        if (compact) continue;
        if (key=='\r' || key=='\n') {memcpy(value,edit,used+1);result=0;break;}
        if (key==21) {used=0;edit[0]=0;}
        else if (key==8 || key==127) {
            if (used) {do {used--;} while (used && ((unsigned char)edit[used]&0xc0)==0x80);edit[used]=0;}
        } else if (key>=32 && key<=0x10ffff && !(key>=0xd800 && key<=0xdfff)) {
            char bytes[4];size_t n;
            if (key<0x80) {bytes[0]=(char)key;n=1;}
            else if (key<0x800) {bytes[0]=(char)(0xc0|(key>>6));bytes[1]=(char)(0x80|(key&63));n=2;}
            else if (key<0x10000) {bytes[0]=(char)(0xe0|(key>>12));bytes[1]=(char)(0x80|((key>>6)&63));bytes[2]=(char)(0x80|(key&63));n=3;}
            else {bytes[0]=(char)(0xf0|(key>>18));bytes[1]=(char)(0x80|((key>>12)&63));bytes[2]=(char)(0x80|((key>>6)&63));bytes[3]=(char)(0x80|(key&63));n=4;}
            if (used+n<cap) {memcpy(edit+used,bytes,n);used+=n;edit[used]=0;}
        }
    }
    home_windows_close(&console);SecureZeroMemory(edit,sizeof edit);return result;
}

static int home_windows_join(HomeSettings *settings) {
    HomeSettings next=*settings;next.owner=0;
    int rc=-1;
    if (home_windows_field("Household address (host:port)",next.tracker,sizeof next.tracker,0) ||
        home_windows_field("Household key from its owner",next.token,sizeof next.token,1)) goto done;
    rc=home_settings_join_windows(settings,&next);
    if (rc) home_windows_message("Could not join the household",rc==-4 ?
        "Stop local sharing before joining another household. Your saved settings are unchanged." : rc==-3 ?
        "Settings changed in another workspace or could not be saved. Reopen this workspace; no saved household was overwritten." : rc==-2 ?
        "Household authentication failed. Check the key and tracker address; no resources were shared." :
        "Connection or secure handshake failed. Check the household address and LAN access; no resources were shared.");
done:
    SecureZeroMemory(&next,sizeof next);return rc;
}

static void home_windows_settings(HomeSettings *settings) {
    HomeSettings next=*settings;
    if (home_windows_field("Folder containing your models",next.models,sizeof next.models,0) ||
        home_windows_field("Maximum RAM to share, in GB",next.ram,sizeof next.ram,0)) goto done;
    char *end;double ram=strtod(next.ram,&end);
    if (!*next.ram || *end || !isfinite(ram) || ram<=0 || ram>1048576) {
        home_windows_message("Invalid RAM limit","Enter a positive number of GB. Active allocations were not changed.");goto done;
    }
    if (home_settings_save_windows(&next)) home_windows_message("Could not save settings","Check access to your private Lumabri settings folder. Previous settings are unchanged.");
    else *settings=next;
done:
    SecureZeroMemory(&next,sizeof next);
}

static int home_windows_share(HomeSettings *settings) {
    HomeServiceSnapshot live;
    if (!home_service_query("donor",HOME_SVC_STATUS,NULL,&live)) return lmb_donor_ui_windows(live.tracker);
    if ((!settings->tracker[0] || !settings->token[0]) && home_windows_join(settings)) return -1;
    if (home_settings_activate_windows(settings)) return -1;
    char *args[]={"--join",settings->tracker,"--ram-gb",settings->ram};
    int rc=home_donor_entry_windows(4,args,0);
    if (rc) home_windows_message("Sharing could not start","Check household connectivity and the installed Segment runtime. No new request was approved.");
    return rc;
}

static void home_windows_create(HomeSettings *settings) {
    int rc=home_create_windows(settings);
    if (rc) {
        home_windows_message("Could not create the household",rc==-4 ?
            "Local services or settings changed. Stop sharing before creating another household, then reopen the workspace." : rc==-3 ?
            "Settings changed in another workspace or could not be saved. The new index was stopped. Reopen the workspace." : rc==-6 ?
            "Startup was not confirmed and shutdown could not be verified. Check service status before retrying." :
            "Check the installed tracker runtime, LAN address and household port. Your saved settings are unchanged.");
        return;
    }
    LmbConsole console={0};if (home_windows_open(&console)) return;
    int reveal=0;
    for (;;) {
        ui_begin("household ready");ui_text(5,4,UI_TEXT,"Connect your other computers");
        ui_text(8,4,UI_MUTED,"Household address");ui_text(9,4,UI_SAND,settings->tracker);
        ui_text(12,4,UI_MUTED,"Household key");ui_text(13,4,UI_TEXT,reveal ? settings->token : "Hidden — press r to reveal");
        ui_text(17,4,UI_TEXT,"On each computer choose Join a household, then Share resources.");
        ui_text(19,4,UI_MUTED,"Each donor still approves its own model allocations.");
        ui_footer("The household keeps running when you close this workspace.","r show/hide key   Enter continue   Esc back");
        if (ui_w<76 || ui_h<25) {ui_begin("household ready");ui_text(5,4,UI_SAND,"Resize to 76 × 25 to view complete pairing details. Esc returns.");}
        ui_present();
        int resized=0,key=lmb_console_key(&console,100,&resized);
        if (key=='r') reveal=!reveal;
        if (key=='\r' || key=='\n' || key==27 || key==3) break;
    }
    home_windows_close(&console);
}

static int home_windows_resident_chat(const LmbResidentPlan *p) {
    LmbWindowsObservation observation={.plan=p,.source=LMB_CAL_SOURCE_SESSION};
    int rc=chat_windows_run_observed(p->host,p->host_key,p->root,NULL,p->max_new,&observation);
    if (rc) home_windows_message("Chat could not complete","The saved host is unavailable or rejected the request. Keep its donors sharing, or prepare another plan. Resident weights were not released.");
    return rc;
}
static void home_windows_calibrate(const LmbResidentPlan *plan) {
    LmbConsole console={0};if (home_windows_open(&console)) return;
    ui_begin("measure resident model");ui_text(5,4,UI_TEXT,"Checking the approved host…");ui_present();
    LmbWindowsTurn *turn=calloc(1,sizeof *turn);LmbWindowsObservation observation={.plan=plan,.source=LMB_CAL_SOURCE_PROBE};
    HANDLE worker=NULL;int ok=0;char failure[256]="";
    if (!turn) goto done;
    InitializeCriticalSection(&turn->lock);turn->fd=LMB_INVALID_SOCKET;
    turn->observation=&observation;turn->deadline_ms=(uint32_t)(LMB_QUICK_PROBE_SECONDS*1000);
    turn->max_new=LMB_QUICK_PROBE_TOKENS;strcpy(turn->id,"1");
    if (lmb_host_client_open(plan->host,NULL,plan->host_key,plan->root,&turn->fd,&turn->greeting) ||
        turn->greeting.max_new<LMB_QUICK_PROBE_TOKENS ||
        lmb_host_turn_payload(&turn->payload,&turn->greeting,"","Write a short greeting.")) goto done;
    worker=CreateThread(NULL,0,chat_windows_worker,turn,0,NULL);if (!worker) goto done;
    while (WaitForSingleObject(worker,0)==WAIT_TIMEOUT) {
        ui_begin("measure resident model");ui_text(5,4,UI_TEXT,"Measuring the approved resident plan");
        ui_text(9,4,UI_SAND,"At most 8 output tokens; generation stops after 20 seconds.");
        ui_text(12,4,UI_MUTED,"No reload. This observation applies only to these computers and this workload.");
        ui_footer("The full checkpoint must already be prepared in RAM.","Esc cancel");ui_present();
        int resized=0,key=lmb_console_key(&console,75,&resized);
        if (key==27 || key==3) {InterlockedExchange(&turn->cancelled,1);shutdown(turn->fd,SD_BOTH);}
    }
    ok=!turn->result && observation.saved;
done:
    if (worker) {WaitForSingleObject(worker,INFINITE);CloseHandle(worker);}
    if (turn) {
        if (turn->error[0]) snprintf(failure,sizeof failure,"%s",turn->error);
        if (turn->fd!=LMB_INVALID_SOCKET) lmb_close(turn->fd);
        DeleteCriticalSection(&turn->lock);free(turn->reply.p);free(turn->payload.p);free(turn);
    }
    home_windows_close(&console);
    char detail[256];
    if (ok) snprintf(detail,sizeof detail,"%.2f tok/s observed from %u generated tokens. The catalogue invalidates this value when the plan or workload changes.",
        observation.seed.decode_tok_s,observation.seed.generated_tokens);
    else snprintf(detail,sizeof detail,"No valid speed saved. %.180s Approved weights remain in RAM.",failure[0] ? failure : "The host may be busy, timed out, or the execution inventory changed.");
    home_windows_message(ok ? "Measurement saved" : "Measurement unavailable",detail);
}

/* Reuse an exact approved placement for a short probe. A live connection
 * still has to authenticate its pinned model root before receiving text. */
static int home_windows_matching_resident(const LmbTuiState *st,int index,LmbResidentPlan *out) {
    if (index<0 || index>=st->nmodels) return -1;
    LmbCalKey wanted;
    if (catalog_calibration_key(st,&st->models[index],1,"comparison",&wanted)) return -1;
    LmbResidentPlan *plans=calloc(64,sizeof *plans);if (!plans) return -1;
    size_t count=home_resident_library_list(st->tracker,plans,64);int found=-1;
    for (size_t i=0;i<count;i++) {
        LmbCalKey actual;if (home_resident_observation_key(&plans[i],&actual)) continue;
        actual.adapter_abi=1;strcpy(actual.numeric_class,"comparison");
        memset(actual.workload,0,sizeof actual.workload);memset(wanted.workload,0,sizeof wanted.workload);
        if (lmb_cal_matches(&actual,&wanted)) {*out=plans[i];found=0;break;}
    }
    free(plans);return found;
}

static void home_windows_residents(const HomeSettings *settings) {
    LmbResidentPlan *plans=calloc(64,sizeof *plans);if (!plans) return;
    size_t count=home_resident_library_list(settings->tracker,plans,64);
    if (!count) {free(plans);home_windows_message("No saved resident models","Choose Explore models to prepare a checkpoint. Approved models appear here after the entire chain is ready.");return;}
    LmbConsole console={0};if (home_windows_open(&console)) {free(plans);return;}
    int choice=0,action=-1,calibrate=0;
    for (;;) {
        ui_begin("resident models");ui_text(5,4,UI_TEXT,"Continue with an approved model");
        int rows=(ui_h-12)/3;if (rows<1) rows=1;
        int first=choice>=rows ? choice-rows+1 : 0;
        for (int i=first;i<(int)count && i<first+rows;i++) {
            char detail[160];snprintf(detail,sizeof detail,"%u computer(s) · %u session slot(s) · host %.64s",
                plans[i].execution.count,plans[i].sessions ? plans[i].sessions : 1,plans[i].host);
            ui_item(7+(i-first)*3,i==choice,plans[i].model,detail);
        }
        ui_footer("The host's identity and checkpoint are checked again before sending text.","↑ ↓ move   Enter chat   C measure   Esc back");ui_present();
        int resized=0,key=lmb_console_key(&console,100,&resized);
        if (key==27 || key==3) break;
        if (key=='j') choice=(choice+1)%(int)count;
        if (key=='k') choice=(choice+(int)count-1)%(int)count;
        if (key=='c') {action=choice;calibrate=1;break;}
        if (key=='\r' || key=='\n') {action=choice;break;}
    }
    home_windows_close(&console);
    if (action>=0) {if (calibrate) home_windows_calibrate(&plans[action]);else home_windows_resident_chat(&plans[action]);}
    free(plans);
}

static void home_windows_prepare_wait(const HomeServiceSnapshot *expected,int calibrate) {
    LmbConsole console={0};if (home_windows_open(&console)) return;
    HomeServiceSnapshot status=*expected;int complete=0;
    for (;;) {
        HomeServiceSnapshot next;
        int live=!home_service_query("prepare",HOME_SVC_STATUS,NULL,&next);
        if (!live && home_service_record("prepare",&next)) break;
        if (memcmp(next.instance,expected->instance,32)) break;
        status=next;
        if (status.state!=HOME_SVC_RUNNING) {complete=1;break;}
        ui_begin("prepare model");ui_text(5,4,UI_TEXT,status.name);
        int width=ui_w-8;if (width>150) width=150;if (width<10) width=10;
        for (size_t at=0;at<strlen(status.detail) && at<(size_t)width*4;at+=(size_t)width) {
            char line[160];snprintf(line,sizeof line,"%.*s",width,status.detail+at);ui_text(8+(int)(at/(size_t)width),4,UI_SAND,line);
        }
        HomeServiceSnapshot donor={0};int local_pending=!home_service_query("donor",HOME_SVC_STATUS,NULL,&donor) &&
            donor.phase==LMB_HOME_PENDING && donor.has_offer;
        int belongs=0;
        for (uint32_t i=0;i<status.preparation_result_count;i++)
            if (!memcmp(donor.offer.id,status.preparation_results[i].allocation,32)) belongs=1;
        local_pending=local_pending && belongs;
        if (local_pending) {
            ui_text(15,4,UI_TEXT,"This computer needs your approval too.");
            ui_printf(17,4,UI_SAND,"Layers %u–%u · %.2f GB RAM · %s",donor.offer.begin,donor.offer.end-1,
                donor.offer.ram_bytes/1e9,donor.offer.runs_edge ? "hosts chat" : "runs a segment");
            ui_text(19,4,UI_MUTED,"A accepts this allocation. D declines it.");
        }
        ui_footer("Chat opens after every donor approves and the entire chain is ready.","Esc detach   X cancel preparation");ui_present();
        int resized=0,key=lmb_console_key(&console,100,&resized);
        if (key==27 || key==3) break;
        if (key=='x') {HomeServiceSnapshot reply;(void)home_service_query("prepare",HOME_SVC_CANCEL,&status,&reply);}
        if (local_pending && (key=='a' || key=='d')) {HomeServiceSnapshot reply;(void)home_service_query("donor",key=='a' ? HOME_SVC_ACCEPT : HOME_SVC_DECLINE,&donor,&reply);}
    }
    home_windows_close(&console);
    if (!complete) return;
    if (status.state!=HOME_SVC_DONE || !status.preparation_result_count) {
        home_windows_message("Preparation did not complete",status.detail);return;
    }
    if (status.preparation_result_count>1) {
        home_windows_message("Selected models ready","Open Resident models to start private conversations. Every approved model has a separate reservation.");return;
    }
    LmbResidentPlan saved;
    if (status.preparation_results[0].ready_observed &&
        !home_resident_library_find(status.tracker,status.preparation_results[0].allocation,&saved)) {
        if (calibrate) home_windows_calibrate(&saved);else home_windows_resident_chat(&saved);
    } else home_windows_message("Model prepared","The model reached READY, but its saved plan could not be found. Open Resident models; do not repeat preparation blindly.");

}

static void home_windows_explore(HomeSettings *settings) {
    HomeServiceSnapshot running;
    int pending=home_prepare_idle_windows(&running);
    if (pending>0) {home_windows_prepare_wait(&running,0);return;}
    if (pending<0) {home_windows_message("Preparation status unavailable","The last operation has not closed yet. Try again shortly; no new plan was started.");return;}
    if ((!settings->tracker[0] || !settings->token[0]) && home_windows_join(settings)) return;
    if (home_settings_activate_windows(settings)) return;
    /* Make local capacity available for selection, without approving a model. */
    char *donor_args[]={"--join",settings->tracker,"--ram-gb",settings->ram,"--headless"};
    if (home_donor_entry_windows(5,donor_args,0)) {
        home_windows_message("Local sharing unavailable","Other donors may still be used. This computer will not load weights without a working donor and your approval.");
    }
    LmbTuiState *st=calloc(1,sizeof *st);if (!st) return;
    snprintf(st->root,sizeof st->root,"%s",settings->models);snprintf(st->disk,sizeof st->disk,"%s",settings->models);
    snprintf(st->tracker,sizeof st->tracker,"%s",settings->tracker);
    st->context=(uint32_t)strtoul(settings->context,NULL,10);st->sessions=(uint32_t)strtoul(settings->sessions,NULL,10);st->max_new=256;
    st->refresh=catalog_refresh_windows;catalog_refresh_windows(st,NULL);
    for (;;) {
        int action=lmb_tui_run(st,0,NULL);
        if (action!=LMB_TUI_REQUEST_CHAT && action!=LMB_TUI_REQUEST_CALIBRATION && action!=LMB_TUI_REQUEST_JOINT) break;
        int calibrate=action==LMB_TUI_REQUEST_CALIBRATION;
        LmbResidentPlan existing;
        if (calibrate && !home_windows_matching_resident(st,st->action_model,&existing)) {
            home_windows_calibrate(&existing);catalog_refresh_windows(st,NULL);continue;
        }
        HomeServiceSnapshot created;
        if (home_prepare_start_windows(st,action==LMB_TUI_REQUEST_JOINT ? -1 : st->action_model,&created))
            home_windows_message("Plan could not start","Refresh the selected computers and checkpoint. Another preparation may already be running; no new approval was granted.");
        else home_windows_prepare_wait(&created,calibrate);
        catalog_refresh_windows(st,NULL);
    }
    free(st);
}

static int home_windows_run(int share) {
    HomeServiceSnapshot live;
    if (share && !home_service_query("donor",HOME_SVC_STATUS,NULL,&live)) return lmb_donor_ui_windows(live.tracker);
    HomeSettings settings;
    if (home_settings_load_windows(&settings)) {
        home_windows_message("Cannot read household settings","The private settings file is unreadable or invalid. It has not been overwritten.");return 1;
    }
    if (_putenv_s("LUMABRI_ENCRYPT","1") || lmb_secure_init() || !lmb_secure_enabled() || home_settings_activate_windows(&settings))
        return home_fail("Cannot activate encrypted household access.");
    int rc=0;
    if (share) {rc=home_windows_share(&settings);goto done;}
    int choice=0;
    for (;;) {
        LmbConsole console={0};if (home_windows_open(&console)) {rc=home_fail("Lumabri needs an interactive Windows console.");break;}
        int action=-1;
        for (;;) {
            ui_begin("your workspace");
            ui_text(5,5,UI_TEXT,"Your computers. One private household.");
            static const char *titles[]={"Share resources","Join a household","Settings","Stop local services","Create a household","Exit workspace","Explore models","Resident models"};
            static const char *details[]={"Review requests; keep approved models in RAM.","Connect using the address and key from its owner.",
                "Model folder and maximum RAM to share.","Review before stopping sharing and releasing local models.",
                "Start a private household here, or view its pairing details.","Background services keep running.","Choose a checkpoint and the computers that will run it.","Chat with models already prepared and approved."};
            int rows=(ui_h-11)/3;if (rows<1) rows=1;
            int first=choice>=rows ? choice-rows+1 : 0;
            for (int i=first;i<8 && i<first+rows;i++) ui_item(7+(i-first)*3,i==choice,titles[i],details[i]);
            ui_footer(settings.tracker[0] ? settings.tracker : "Create or join your household to get started.","↑ ↓ move   Enter select   Esc exit");
            int compact=ui_w<60 || ui_h<28;
            if (compact) {ui_begin("your workspace");ui_text(5,4,UI_SAND,"Resize to at least 60 × 28. Esc exits.");}
            ui_present();int resized=0,key=lmb_console_key(&console,100,&resized);
            if (key==27 || key==3) {action=5;break;}
            if (key=='k') choice=(choice+7)%8;
            if (key=='j') choice=(choice+1)%8;
            if (!compact && (key=='\r' || key=='\n')) {action=choice;break;}
        }
        home_windows_close(&console);
        if (action==5) break;
        if (!action) (void)home_windows_share(&settings);
        else if (action==1) (void)home_windows_join(&settings);
        else if (action==2) home_windows_settings(&settings);
        else if (action==4) home_windows_create(&settings);
        else if (action==6) home_windows_explore(&settings);
        else if (action==7) home_windows_residents(&settings);
        else if (action==3) {
            char confirm[32]="";
            if (!home_windows_field("Type stop to release all local models",confirm,sizeof confirm,0) && !strcmp(confirm,"stop")) {
                char *args[]={"stop"};
                if (cmd_service(1,args)) home_windows_message("Services did not confirm shutdown","Check service status before retrying. No remote computers were stopped.");
            }
        }
    }
done:
    SecureZeroMemory(&settings,sizeof settings);return rc;
}
#endif
