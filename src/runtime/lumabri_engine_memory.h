/* Owned CPU engine allocation contract. This is not a Colibri ABI extension
 * and does not claim that libc, driver, stack or code pages are locked. */
#ifndef LMB_ENGINE_MEMORY_H
#define LMB_ENGINE_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    uint64_t allocations, requested_bytes, locked_bytes;
    int enabled, fault;
} LmbEngineMemory;
void lmb_engine_memory_snapshot(LmbEngineMemory *out);
void *lmb_engine_malloc(size_t bytes);
void *lmb_engine_calloc(size_t count, size_t bytes);
void *lmb_engine_realloc(void *ptr, size_t bytes);
void lmb_engine_free(void *ptr);
int lmb_engine_posix_memalign(void **ptr, size_t alignment, size_t bytes);
void *lmb_engine_aligned_alloc(size_t alignment, size_t bytes);
char *lmb_engine_strdup(const char *text);
char *lmb_engine_strndup(const char *text, size_t max);
void *lmb_engine_mmap(void *addr, size_t bytes, int prot, int flags, int fd, off_t offset);
int lmb_engine_munmap(void *addr, size_t bytes);
int lmb_engine_mlock(const void *addr, size_t bytes);
int lmb_engine_munlock(const void *addr, size_t bytes);
int lmb_engine_mlockall(int flags);
int lmb_engine_munlockall(void);
int lmb_engine_madvise(void *addr, size_t bytes, int advice);
#endif
