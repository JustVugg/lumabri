/* Platform differences at the process/libc boundary, never model arithmetic. */
#ifndef LUMABRI_PLATFORM_H
#define LUMABRI_PLATFORM_H
#ifdef __APPLE__
#define LMB_SHIM_NAME "liblumabri.dylib"
#define LMB_PRELOAD_ENV "DYLD_INSERT_LIBRARIES"
#else
#define LMB_SHIM_NAME "liblumabri.so"
#define LMB_PRELOAD_ENV "LD_PRELOAD"
#endif
#endif
