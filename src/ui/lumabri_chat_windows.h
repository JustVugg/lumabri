/* Native thin chat. The worker owns a single turn; the UI can interrupt its
 * socket, but closes it only after joining. Completed history is session-local. */
#ifndef LUMABRI_CHAT_WINDOWS_H
#define LUMABRI_CHAT_WINDOWS_H
#include "src/runtime/lumabri_host_turn.h"
#include "src/ui/lumabri_visual.h"
#include "lumabri_metrics.h"
#include "src/runtime/lumabri_calibration_windows.h"
#include "src/runtime/lumabri_probe_deadline.h"

typedef struct {
    LmbSocket fd;
    LmbHostGreeting greeting;
    Cap payload,reply;
    char id[64],error[256],stat[LMB_REPLY_HEADER];
    unsigned max_new;
    CRITICAL_SECTION lock;
    volatile LONG cancelled;
    int result;
    uint32_t deadline_ms;
    double started,ttft;
    LmbWindowsObservation *observation;
} LmbWindowsTurn;
static int chat_windows_data(void *arg,const unsigned char *text,size_t n) {
    LmbWindowsTurn *t=arg;
    if (memchr(text,0,n)) return -1;
    if (n && !t->ttft) t->ttft=nowd()-t->started;
    EnterCriticalSection(&t->lock);
    int rc=t->reply.len>LMB_CLIENT_TEXT_LIMIT || n>LMB_CLIENT_TEXT_LIMIT-t->reply.len ||
        cap_add(&t->reply,(const char *)text,n);
    LeaveCriticalSection(&t->lock);return rc;
}
static int chat_windows_error(void *arg,const char *error) {
    LmbWindowsTurn *t=arg;snprintf(t->error,sizeof t->error,"%s",error);return 0;
}
static DWORD WINAPI chat_windows_worker(void *arg) {
    LmbWindowsTurn *t=arg;LmbReplyStream stream;
    LmbReplySink sink={chat_windows_data,NULL,chat_windows_error,t};t->result=-1;
    if (t->observation) (void)catalog_observe_begin_windows(t->observation,&t->greeting);
    t->started=nowd();t->ttft=0;
    if (lmb_reply_init(&stream,t->id,LMB_CLIENT_TEXT_LIMIT,sink) ||
        lmb_host_turn_send(t->fd,&t->greeting,t->id,&t->payload,t->max_new)) {
        snprintf(t->error,sizeof t->error,"Could not submit this request (transport error %d).",errno);return 0;
    }
    uint64_t until=GetTickCount64()+(t->deadline_ms ? t->deadline_ms : 300000);
    while (!InterlockedCompareExchange(&t->cancelled,0,0) && stream.status==LMB_REPLY_MORE) {
        uint64_t now=GetTickCount64();if(now>=until) break;
        int ready=lmb_socket_wait(t->fd,0,100);
        if(ready<0) {snprintf(t->error,sizeof t->error,"Host closed the connection (transport error %d).",errno);break;}
        if(!ready) continue;
        uint64_t remaining=until-now;
        if (lmb_host_turn_read(t->fd,&stream,(uint32_t)(remaining<5000 ? remaining : 5000))) {
            snprintf(t->error,sizeof t->error,"Host reply ended before completion (stream status %d, transport error %d).",stream.status,errno);break;
        }
    }
    if (stream.status==LMB_REPLY_DONE && !InterlockedCompareExchange(&t->cancelled,0,0)) {
        memcpy(t->stat,stream.stat,strlen(stream.stat)+1);t->result=0;
        if (t->observation) (void)catalog_observe_end_windows(t->observation,&t->greeting,t->stat,t->ttft);
    }
    return 0;
}
static size_t chat_windows_utf8(int scalar,char bytes[4]) {
    if (scalar<32 || scalar>0x10ffff || (scalar>=0xd800 && scalar<=0xdfff)) return 0;
    if (scalar<0x80) {bytes[0]=(char)scalar;return 1;}
    if (scalar<0x800) {bytes[0]=(char)(0xc0|(scalar>>6));bytes[1]=(char)(0x80|(scalar&63));return 2;}
    if (scalar<0x10000) {bytes[0]=(char)(0xe0|(scalar>>12));bytes[1]=(char)(0x80|((scalar>>6)&63));bytes[2]=(char)(0x80|(scalar&63));return 3;}
    bytes[0]=(char)(0xf0|(scalar>>18));bytes[1]=(char)(0x80|((scalar>>12)&63));
    bytes[2]=(char)(0x80|((scalar>>6)&63));bytes[3]=(char)(0x80|(scalar&63));return 4;
}

/* Wrap by Unicode display width, never execute terminal escapes from a peer.
 * Keep a bounded viewport of the full in-memory transcript; Ctrl-P/N scroll. */
static unsigned chat_windows_lines(const char *text,unsigned first,unsigned height,int paint) {
    char line[1024];size_t used=0;unsigned row=0;int cols=0,width=ui_w-10;
    for(const unsigned char *p=(const unsigned char *)text;*p;) {
        size_t n=*p<0x80 ? 1 : *p>=0xc2 && *p<=0xdf ? 2 : *p>=0xe0 && *p<=0xef ? 3 : *p>=0xf0 && *p<=0xf4 ? 4 : 1;
        uint32_t cp=*p&(n==1 ? 0x7f : n==2 ? 0x1f : n==3 ? 0xf : 7);int valid=1;
        for(size_t i=1;i<n;i++) {if(!p[i] || (p[i]&0xc0)!=0x80) {valid=0;break;}cp=(cp<<6)|(p[i]&63);}
        if(!valid) {p++;continue;}
        int cells=lmb_console_width(cp);
        if(*p=='\n' || (cells>0 && cols+cells>width) || used+n>=sizeof line) {
            line[used]=0;
            if(paint && row>=first && row-first<height) ui_text(6+(int)(row-first),4,UI_TEXT,line);
            row++;used=0;cols=0;
            if(*p=='\n') {p++;continue;}
        }
        if(cells>=0) {memcpy(line+used,p,n);used+=n;cols+=cells;}
        p+=n;
    }
    line[used]=0;
    if(paint && row>=first && row-first<height) ui_text(6+(int)(row-first),4,UI_TEXT,line);
    return row+1;
}
static void chat_windows_transcript(const char *text,unsigned *scroll) {
    unsigned height=ui_h>14 ? (unsigned)(ui_h-14) : 1;
    unsigned total=chat_windows_lines(text,0,0,0),maximum=total>height ? total-height : 0;
    if(*scroll>maximum) *scroll=maximum;
    unsigned first=maximum-*scroll;
    (void)chat_windows_lines(text,first,height,1);
}

static int chat_windows_run_observed(const char *address,const char *key,const char *root,const char *model,unsigned max_new,LmbWindowsObservation *observation) {
    LmbConsole console={0};if(lmb_console_open(&console)) return home_fail("Chat needs an interactive Windows console.");
    fputs("\033[?1049h\033[?25l",stdout);
    LmbSocket fd=LMB_INVALID_SOCKET;LmbHostGreeting greeting={0};
    Cap history={0},transcript={0};char input[4096]="",status[256]="Connects only to the selected host; no checkpoint download.";
    size_t used=0;unsigned sequence=0,scroll=0;int rc=0,needs_new=0;
    LmbWindowsTurn *turn=calloc(1,sizeof *turn);HANDLE thread=NULL;
    if(!turn) {rc=1;goto done;}InitializeCriticalSection(&turn->lock);turn->observation=observation;
    for (;;) {
        if(thread && WaitForSingleObject(thread,0)==WAIT_OBJECT_0) {
            CloseHandle(thread);thread=NULL;
            if(turn->reply.len && cap_add(&transcript,turn->reply.p,turn->reply.len)) {rc=1;break;}
            if (!turn->result) {
                Cap next={0};
                if(cap_str(&next,history.p ? history.p : "") ||
                    serve2_turn(&next,greeting.kind,!history.len,input,turn->reply.p ? turn->reply.p : "") || next.len>LMB_CLIENT_TEXT_LIMIT) {
                    free(next.p);snprintf(status,sizeof status,"History limit reached. Use /new before another turn.");
                    lmb_close(fd);fd=LMB_INVALID_SOCKET;needs_new=1;
                } else {free(history.p);history=next;
                    LmbGenerationMetrics m;
                    if(!lmb_metrics_parse(turn->stat,&m) && lmb_metrics_decode_rate(&m)>0)
                        snprintf(status,sizeof status,"Reply %u · %.2f tok/s · first token %.2fs · %u tokens · measured on host",sequence,lmb_metrics_decode_rate(&m),turn->ttft,m.generated_tokens);
                    else snprintf(status,sizeof status,"Reply %u complete · speed not measured by this host",sequence);
                }
            } else {
                const char *why=turn->cancelled ? "Cancelled. Partial reply is not included in the next prompt." :
                    turn->error[0] ? turn->error : "Connection or reply failed. Completed conversation is retained. Send a new message to reconnect.";
                snprintf(status,sizeof status,"%s",why);
                if(cap_str(&transcript,"\n[Reply interrupted]\n")) {rc=1;break;}
                lmb_close(fd);fd=LMB_INVALID_SOCKET;
            }
            free(turn->reply.p);turn->reply=(Cap){0};free(turn->payload.p);turn->payload=(Cap){0};
            input[0]=0;used=0;
        }
        ui_begin("conversation");ui_printf(4,4,UI_MUTED,"%s · %s",model && *model ? model : greeting.model_type,address);
        Cap view={0};int failed=cap_str(&view,transcript.p ? transcript.p : "");
        if(thread) {EnterCriticalSection(&turn->lock);if(!failed) failed=cap_str(&view,turn->reply.p ? turn->reply.p : "");LeaveCriticalSection(&turn->lock);}
        if(failed) {free(view.p);rc=1;break;}
        chat_windows_transcript(view.p ? view.p : "",&scroll);free(view.p);
        ui_text(ui_h-7,4,UI_MUTED,thread ? "Generating… Esc or Ctrl-C cancels this reply." : status);
        ui_text(ui_h-5,4,UI_SAND,thread ? "" : input[0] ? input : "Message…");
        ui_footer("/new new conversation   /quit return   Tab completes commands", "Enter send   Ctrl-P/N scroll   Esc back");
        if(ui_w<60 || ui_h<24) {ui_begin("conversation");ui_text(5,4,UI_SAND,"Resize to at least 60 × 24. Esc cancels or returns.");}
        ui_present();int resized=0,k=lmb_console_input(&console,75,&resized,1);
        if(k==16) {if(scroll<UINT_MAX-5) scroll+=5;continue;}
        if(k==14) {scroll=scroll>5 ? scroll-5 : 0;continue;}
        if(thread) {
            if(k==27 || k==3) {InterlockedExchange(&turn->cancelled,1);shutdown(fd,SD_BOTH);}
            continue;
        }
        if(k==27 || k==3) break;
        if(ui_w<60 || ui_h<24) continue;
        if(k==9 && input[0]=='/') {
            const char *commands[]={"/new","/quit","/help"};
            for(unsigned i=0;i<3;i++) if(!strncmp(commands[i],input,used)) {strcpy(input,commands[i]);used=strlen(input);break;}
        } else if(k==21) {used=0;input[0]=0;}
        else if(k==8 || k==127) {if(used) {do {used--;} while(used && ((unsigned char)input[used]&0xc0)==0x80);input[used]=0;}}
        else if(k=='\r' || k=='\n') {
            if(!used) continue;
            if(!strcmp(input,"/quit") || !strcmp(input,"/exit")) break;
            if(!strcmp(input,"/new")) {
                if(fd!=LMB_INVALID_SOCKET) lmb_close(fd);
                fd=LMB_INVALID_SOCKET;
                free(history.p);history=(Cap){0};free(transcript.p);transcript=(Cap){0};
                used=0;input[0]=0;scroll=0;needs_new=0;snprintf(status,sizeof status,"New private conversation.");continue;
            }
            if(input[0]=='/') {snprintf(status,sizeof status,"Commands: /new clears this conversation; /quit returns. Tab completes.");used=0;input[0]=0;continue;}
            if(needs_new || transcript.len>LMB_CLIENT_TEXT_LIMIT) {snprintf(status,sizeof status,"Conversation limit reached. Use /new to continue.");continue;}
            if(fd==LMB_INVALID_SOCKET) {
                ui_text(ui_h-7,4,UI_SAND,"Connecting and verifying the selected host…");ui_present();
                int connected=lmb_host_client_open(address,model,key,root,&fd,&greeting);
                if(connected || greeting.codec!=2) {
                    if(fd!=LMB_INVALID_SOCKET) lmb_close(fd);
                fd=LMB_INVALID_SOCKET;
                    snprintf(status,sizeof status,connected==HOST_CONNECT_BUSY ? "Host busy. Try again when a session is available." :
                        "Host identity, model or connection could not be verified. No message sent.");continue;
                }
            }
            if(lmb_host_turn_payload(&turn->payload,&greeting,history.p ? history.p : "",input)) {
                free(turn->payload.p);turn->payload=(Cap){0};snprintf(status,sizeof status,"Prompt exceeds the host limit. Shorten it or start /new.");continue;
            }
            turn->fd=fd;turn->greeting=greeting;turn->cancelled=0;turn->error[0]=0;
            turn->max_new=greeting.max_new && max_new>greeting.max_new ? greeting.max_new : max_new;
            if(!++sequence) ++sequence;
            snprintf(turn->id,sizeof turn->id,"%u",sequence);
            if(cap_str(&transcript,"\nYou\n") || cap_str(&transcript,input) || cap_str(&transcript,"\n\nAssistant\n")) {rc=1;break;}
            scroll=0;thread=CreateThread(NULL,0,chat_windows_worker,turn,0,NULL);
            if(!thread) {rc=1;break;}
        } else {char bytes[4];size_t n=chat_windows_utf8(k,bytes);if(n && used+n<sizeof input) {memcpy(input+used,bytes,n);used+=n;input[used]=0;}}
    }
done:
    if(thread) {InterlockedExchange(&turn->cancelled,1);shutdown(fd,SD_BOTH);WaitForSingleObject(thread,INFINITE);CloseHandle(thread);}
    if(fd!=LMB_INVALID_SOCKET) lmb_close(fd);
    if(turn) {DeleteCriticalSection(&turn->lock);free(turn->payload.p);free(turn->reply.p);free(turn);}
    free(history.p);free(transcript.p);
    fputs("\033[?25h\033[?1049l",stdout);fflush(stdout);lmb_console_close(&console);return rc;
}
static int chat_windows_run(const char *address,const char *key,const char *root,const char *model,unsigned max_new) {
    return chat_windows_run_observed(address,key,root,model,max_new,NULL);
}
#endif
