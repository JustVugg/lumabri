/* Verified remote input for a dedicated native resident worker. */
#ifndef LMB_CHECKPOINT_WINDOWS_H
#define LMB_CHECKPOINT_WINDOWS_H
#ifdef _WIN32
#include <stdint.h>
int lmb_checkpoint_windows_prepare(const char *directory,const char *tracker,
    const char *model,const uint8_t signer[32],const uint8_t root[32],uint64_t bytes);
#endif
#endif
