/* Native Windows checkpoint-input boundary for dedicated model processes.
 * Compile the implementation without either engine override header. */
#ifndef LMB_ENGINE_IO_WINDOWS_H
#define LMB_ENGINE_IO_WINDOWS_H
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef struct {
    uint64_t read_calls, read_bytes, denied_reads;
    int sealed;
} LmbEngineInput;
/* A dedicated worker may read a verified remote checkpoint during preparation.
 * A provider owns every engine input: unknown handles fail, never fall back to
 * sparse placeholder contents. Installation is one-shot, before the first read. */
typedef int (*LmbEngineInputRead)(void *,HANDLE,void *,size_t,uint64_t);
typedef void (*LmbEngineInputClose)(void *);
int lmb_engine_input_provider(void *,LmbEngineInputRead,LmbEngineInputClose);
BOOL WINAPI lmb_engine_ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
size_t lmb_engine_fread(void *,size_t,size_t,FILE *);
int lmb_engine_input_seal(void);
int lmb_engine_input_lock_status(uint64_t *retained_bytes);
void lmb_engine_input_snapshot(LmbEngineInput *);
#endif
#endif
