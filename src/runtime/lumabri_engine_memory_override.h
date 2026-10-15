/* Compile-time namespacing, never process-wide malloc interposition. Linux
 * declarations retain their types/attributes and source feature-test macros.
 * Darwin is built with _DARWIN_C_SOURCE: parse libc before the aliases there,
 * so SDK asm labels cannot redirect a renamed wrapper back to native libc. */
#ifndef LMB_ENGINE_MEMORY_OVERRIDE_H
#define LMB_ENGINE_MEMORY_OVERRIDE_H
#ifdef __APPLE__
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "lumabri_engine_memory.h"
#endif
#define malloc lmb_engine_malloc
#define calloc lmb_engine_calloc
#define realloc lmb_engine_realloc
#define free lmb_engine_free
#define posix_memalign lmb_engine_posix_memalign
#define aligned_alloc lmb_engine_aligned_alloc
#define strdup lmb_engine_strdup
#define strndup lmb_engine_strndup
#define mmap lmb_engine_mmap
#define munmap lmb_engine_munmap
#define mlock lmb_engine_mlock
#define munlock lmb_engine_munlock
#define mlockall lmb_engine_mlockall
#define munlockall lmb_engine_munlockall
#define madvise lmb_engine_madvise
#endif
