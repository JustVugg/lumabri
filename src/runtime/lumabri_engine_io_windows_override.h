#ifndef LMB_ENGINE_IO_WINDOWS_OVERRIDE_H
#define LMB_ENGINE_IO_WINDOWS_OVERRIDE_H
#include "lumabri_engine_io_windows.h"
#ifdef _WIN32
/* Parse the native declarations before aliasing, including MinGW's stdio
 * inline functions. The build audit rejects any unwrapped input imports. */
#define ReadFile lmb_engine_ReadFile
#define fread lmb_engine_fread
#endif
#endif
