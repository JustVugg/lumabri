/* Native household launcher. Services retain resource ownership after the
 * console detaches; editable values are committed only after confirmation. */
#ifndef LMB_HOME_WINDOWS_H
#define LMB_HOME_WINDOWS_H
#include "src/runtime/lumabri_home_settings_windows.h"

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
        "The connection succeeded, but private settings could not be saved. Your previous household is unchanged." : rc==-2 ?
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
            static const char *titles[]={"Share resources","Join a household","Settings","Stop local services","Exit workspace"};
            static const char *details[]={"Review requests; keep approved models in RAM.","Connect using the address and key from its owner.",
                "Model folder and maximum RAM to share.","Review before stopping sharing and releasing local models.","Background services keep running."};
            for (int i=0;i<5;i++) ui_item(8+i*3,i==choice,titles[i],details[i]);
            ui_footer(settings.tracker[0] ? settings.tracker : "Join your household to get started.","↑ ↓ move   Enter select   Esc exit");
            int compact=ui_w<60 || ui_h<28;
            if (compact) {ui_begin("your workspace");ui_text(5,4,UI_SAND,"Resize to at least 60 × 28. Esc exits.");}
            ui_present();int resized=0,key=lmb_console_key(&console,100,&resized);
            if (key==27 || key==3) {action=4;break;}
            if (key=='k') choice=(choice+4)%5;
            if (key=='j') choice=(choice+1)%5;
            if (!compact && (key=='\r' || key=='\n')) {action=choice;break;}
        }
        home_windows_close(&console);
        if (action==4) break;
        if (!action) (void)home_windows_share(&settings);
        else if (action==1) (void)home_windows_join(&settings);
        else if (action==2) home_windows_settings(&settings);
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
