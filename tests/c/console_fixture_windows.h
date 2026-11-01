/* Native ConPTY boundary for product UI acceptance tests. */
#ifndef LMB_CONSOLE_FIXTURE_WINDOWS_H
#define LMB_CONSOLE_FIXTURE_WINDOWS_H
typedef struct {
    HANDLE pipe;
    CRITICAL_SECTION mutex;
    char text[2*1024*1024];
    size_t used;
} WinUiCapture;
static DWORD WINAPI winui_capture(void *arg) {
    WinUiCapture *c = arg; char bytes[8192]; DWORD n;
    while (ReadFile(c->pipe, bytes, sizeof bytes, &n, NULL) && n) {
        EnterCriticalSection(&c->mutex);
        size_t take = n < sizeof c->text-c->used-1 ? n : sizeof c->text-c->used-1;
        memcpy(c->text+c->used, bytes, take); c->used += take; c->text[c->used] = 0;
        LeaveCriticalSection(&c->mutex);
    }
    return 0;
}
static int winui_contains(WinUiCapture *c, const char *text) {
    EnterCriticalSection(&c->mutex);
    int found = strstr(c->text, text) != NULL;
    LeaveCriticalSection(&c->mutex); return found;
}
static void winui_expect(WinUiCapture *c, const char *text) {
    ULONGLONG until = GetTickCount64()+15000;
    while (!winui_contains(c, text) && GetTickCount64() < until) Sleep(10);
    if (!winui_contains(c, text)) {
        fprintf(stderr, "Missing console output: %s\n", text); abort();
    }
}
static void winui_send_keys(HANDLE input, const char *keys) {
    DWORD written; size_t n = strlen(keys);
    assert(WriteFile(input, keys, (DWORD)n, &written, NULL) && written == n);
}

typedef struct {
    HPCON console;
    HANDLE input,output,reader,job,process;
    WinUiCapture *capture;
} WinUi;
static void winui_open_args(WinUi *ui,const char *binary,const wchar_t *arguments,const char *expected) {
    memset(ui,0,sizeof *ui);HANDLE in,out;
    assert(CreatePipe(&in,&ui->input,NULL,0) && CreatePipe(&ui->output,&out,NULL,0));
    assert(SUCCEEDED(CreatePseudoConsole((COORD){104,38},in,out,0,&ui->console)));
    CloseHandle(in);CloseHandle(out);
    ui->capture=calloc(1,sizeof *ui->capture);assert(ui->capture);ui->capture->pipe=ui->output;
    InitializeCriticalSection(&ui->capture->mutex);
    ui->reader=CreateThread(NULL,0,winui_capture,ui->capture,0,NULL);assert(ui->reader);
    ui->job=CreateJobObjectW(NULL,NULL);assert(ui->job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    /* Own the console process only. A real launcher may create independent
     * keepers, whose startup/worker ownership is enforced by product jobs. */
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    assert(SetInformationJobObject(ui->job,JobObjectExtendedLimitInformation,&limits,sizeof limits));
    SIZE_T size=0;InitializeProcThreadAttributeList(NULL,2,0,&size);
    STARTUPINFOEXW start={0};start.StartupInfo.cb=sizeof start;
    start.lpAttributeList=malloc(size);assert(start.lpAttributeList);
    assert(InitializeProcThreadAttributeList(start.lpAttributeList,2,0,&size));
    assert(UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,ui->console,sizeof ui->console,NULL,NULL));
    assert(UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&ui->job,sizeof ui->job,NULL,NULL));
    wchar_t *exe=lmb_process_wide(binary),args[32768];assert(exe);
    assert(swprintf(args,32768,L"\"%ls\" %ls",exe,arguments)>0);PROCESS_INFORMATION pi;
    assert(CreateProcessW(exe,args,NULL,NULL,FALSE,EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&start.StartupInfo,&pi));
    free(exe);DeleteProcThreadAttributeList(start.lpAttributeList);free(start.lpAttributeList);
    CloseHandle(pi.hThread);ui->process=pi.hProcess;winui_expect(ui->capture,expected);
}
static void winui_open(WinUi *ui,const char *binary) {
    winui_open_args(ui,binary,L"share","share resources");
}
static void winui_close(WinUi *ui) {
    winui_send_keys(ui->input,"\x1b");
    assert(WaitForSingleObject(ui->process,10000)==WAIT_OBJECT_0);
    DWORD code;assert(GetExitCodeProcess(ui->process,&code) && code==0);
    CloseHandle(ui->process);CloseHandle(ui->input);CloseHandle(ui->job);ClosePseudoConsole(ui->console);
    assert(WaitForSingleObject(ui->reader,10000)==WAIT_OBJECT_0);
    CloseHandle(ui->reader);CloseHandle(ui->output);DeleteCriticalSection(&ui->capture->mutex);free(ui->capture);
}
#endif
