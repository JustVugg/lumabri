#ifndef LMB_ENGINE_IO_WINDOWS_OVERRIDE_H
#define LMB_ENGINE_IO_WINDOWS_OVERRIDE_H
#include "lumabri_engine_io_windows.h"
#ifdef _WIN32
#include <io.h>
#include <unistd.h>
/* Parse the native declarations before aliasing, including MinGW's stdio
 * inline functions. The build audit rejects any unwrapped input imports. */
#define ReadFile lmb_engine_ReadFile
#define fread lmb_engine_fread
#define read lmb_engine_read
#define _read lmb_engine_read
#endif
#endif
