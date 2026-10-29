#ifndef LUMABRI_IDENTITY_WINDOWS_H
#define LUMABRI_IDENTITY_WINDOWS_H
#include "src/runtime/lumabri_private_file_windows.h"

static int lmb_windows_peer_identity(const char *path, uint8_t sk[64], uint8_t pk[32]) {
    for (int attempt = 0; attempt < 200; attempt++) {
        int created = 0;
        HANDLE h = lmb_private_open(path, 1, 1, &created);
        if (h == INVALID_HANDLE_VALUE) return -1;
        if (lmb_private_lock(h, 1)) { CloseHandle(h); return -1; }
        LARGE_INTEGER size; int rc = -1;
        if (!GetFileSizeEx(h, &size)) errno = EIO;
        else if (!size.QuadPart && created) {
            uint8_t seed[32]; char text[130]; DWORD wrote = 0;
            lmb_random(seed, sizeof seed); lmb_sign_keypair(pk, sk, seed);
            SecureZeroMemory(seed, sizeof seed);
            lmb_hex(text, sk, 64); text[128] = '\n';
            if (WriteFile(h, text, 129, &wrote, NULL) && wrote == 129 && FlushFileBuffers(h)) rc = 0;
            else errno = EIO;
            SecureZeroMemory(text, sizeof text);
        } else if (!size.QuadPart) {
            /* The exclusive creator may not have acquired its lock yet. */
            lmb_private_unlock(h); CloseHandle(h); Sleep(3); continue;
        } else if (size.QuadPart == 128 || size.QuadPart == 129) {
            char text[130] = {0}; DWORD got = 0;
            if (ReadFile(h, text, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart &&
                (got == 128 || text[128] == '\n') && !lmb_unhex(sk, text, 64)) {
                uint8_t expected_sk[64], expected_pk[32];
                lmb_sign_keypair(expected_pk, expected_sk, sk);
                if (!memcmp(expected_pk, sk + 32, 32)) { memcpy(pk, expected_pk, 32); rc = 0; }
                SecureZeroMemory(expected_sk, sizeof expected_sk);
            }
            SecureZeroMemory(text, sizeof text);
            if (rc) errno = EINVAL;
        } else errno = EINVAL;
        lmb_private_unlock(h); CloseHandle(h);
        if (rc) { SecureZeroMemory(sk, 64); SecureZeroMemory(pk, 32); }
        return rc;
    }
    errno = ETIMEDOUT; return -1;
}
#endif
