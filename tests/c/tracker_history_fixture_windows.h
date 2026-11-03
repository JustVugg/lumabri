/* Native process/filesystem boundary for the shared tracker-history gate. */
#include "src/runtime/lumabri_owned_process_windows.h"
static void history_fixture_remove(const wchar_t *directory) {
    wchar_t pattern[1200];assert(swprintf(pattern,1200,L"%ls\\*",directory)>0);
    WIN32_FIND_DATAW entry;HANDLE scan=FindFirstFileW(pattern,&entry);assert(scan!=INVALID_HANDLE_VALUE);
    do {
        if (!wcscmp(entry.cFileName,L".") || !wcscmp(entry.cFileName,L"..")) continue;
        assert(!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
        wchar_t child[1400];assert(swprintf(child,1400,L"%ls\\%ls",directory,entry.cFileName)>0);
        if (entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) history_fixture_remove(child);
        else assert(DeleteFileW(child));
    } while (FindNextFileW(scan,&entry));
    FindClose(scan);assert(RemoveDirectoryW(directory));
}
static void history_fixture_race(const char *directory) {
    wchar_t self[2048];char binary[6144];assert(GetModuleFileNameW(NULL,self,2048));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,self,-1,binary,sizeof binary,NULL,NULL));
    LmbOwnedProcess children[2]={{0}};
    for (unsigned i=0;i<2;i++) {
        char value[8];snprintf(value,sizeof value,"%u",i+20);
        const char *args[]={"--race",directory,value};
        LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=3,.error=GetStdHandle(STD_ERROR_HANDLE)};
        assert(!lmb_owned_process_start(&children[i],&launch));
    }
    unsigned accepted=0;
    for (unsigned i=0;i<2;i++) {
        DWORD code;assert(!lmb_owned_process_wait(&children[i],30000,&code) && code<=1);
        accepted+=code==0;assert(!lmb_owned_process_close(&children[i]));
    }
    assert(accepted==1);
}
static void history_fixture_corrupt(TrackerHistory *history,const char *key,int truncate) {
    char path[1400];assert(!tracker_history_path_windows(history,key,0,path));
    HANDLE file=lmb_private_open(path,1,0,NULL);assert(file!=INVALID_HANDLE_VALUE);
    LARGE_INTEGER at={.QuadPart=truncate ? 12 : 8};assert(SetFilePointerEx(file,at,NULL,FILE_BEGIN));
    if (truncate) assert(SetEndOfFile(file));
    else {DWORD written;assert(WriteFile(file,"X",1,&written,NULL) && written==1);}
    assert(FlushFileBuffers(file) && CloseHandle(file));
}
