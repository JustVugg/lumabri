/* Compile with the same allocation override as an engine translation unit. */
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
void *lmb_test_native_malloc(size_t size) { return malloc(size); }
char *lmb_test_native_strdup(const char *text) { return strdup(text); }
void lmb_test_native_free(void *ptr) { free(ptr); }
void *lmb_test_native_aligned(size_t size) { return _aligned_malloc(size,64); }
void lmb_test_native_aligned_free(void *ptr) { _aligned_free(ptr); }
