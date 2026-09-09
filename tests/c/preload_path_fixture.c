#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#if defined(TEST_PRELOAD_LIBRARY)
__attribute__((constructor)) static void loaded(void) {
    setenv("LMB_PRELOAD_PATH_TEST", "loaded", 1);
}
#elif defined(TEST_PRELOAD_LAUNCHER)
#include "src/runtime/lumabri_preload.h"
int main(int argc, char **argv) {
    if (argc != 3 || lmb_preload_file(argv[1])) return 2;
    execl(argv[2], argv[2], (char *)NULL);
    return 3;
}
#else
int main(void) {
    const char *value = getenv("LMB_PRELOAD_PATH_TEST");
    return !value || strcmp(value, "loaded");
}
#endif
