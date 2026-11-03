#ifndef LMB_DOCTOR_WINDOWS_H
#define LMB_DOCTOR_WINDOWS_H
/* Side-effect-free runtime probes. Do not start keepers, load checkpoints or
 * infer GPU support from a detected device. */
static int cmd_doctor_windows(int argc,char **argv) {
    int json=0;
    for (int i=0;i<argc;i++) {
        if (!strcmp(argv[i],"--json")) json=1;
        else if (strcmp(argv[i],"--household")) return home_fail("Use doctor --household [--json].");
    }
    const wchar_t *names[]={L"tracker.exe",L"maintainer.exe",L"segment_node.exe",L"segment_chat.exe"};
    const char *labels[]={"tracker","checkpoint-source","segment-runtime","edge-runtime"};
    int ok[4]={0},passed=1;unsigned threads[4]={0};char binary[6144];
    for (unsigned i=0;i<4;i++) {
        if (!home_sibling_windows(names[i],binary,sizeof binary)) {
            wchar_t *wide=lmb_process_wide(binary);
            DWORD attrs=wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;free(wide);
            ok[i]=attrs!=INVALID_FILE_ATTRIBUTES && !(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
            if (ok[i] && i>=2) ok[i]=!lmb_runtime_thread_capacity(binary,&threads[i]);
        }
        passed &= ok[i];
    }
    if (json) fputs("{\"schema\":1,\"platform\":\"windows\",\"checks\":[",stdout);
    else puts("Lumabri Windows runtime check (no weights loaded)");
    for (unsigned i=0;i<4;i++) {
        const char *detail=!ok[i] ? "Runtime missing, blocked, or incompatible. Reinstall the complete verified package." :
            i>=2 ? "Installed executable answered a bounded thread-capacity probe." : "Packaged executable present.";
        if (json) {
            if (i) putchar(',');
            fputs("{\"name\":",stdout);doctor_json_string(labels[i]);
            printf(",\"ok\":%s,\"threads\":%u,\"detail\":",ok[i] ? "true" : "false",threads[i]);
            doctor_json_string(detail);putchar('}');
        } else printf("  %s  %s: %s\n",ok[i] ? "OK" : "FAIL",labels[i],detail);
    }
    if (json) printf("],\"ok\":%s}\n",passed ? "true" : "false");
    return passed ? 0 : 1;
}
#endif
