#include <fcntl.h>
#include <unistd.h>
#include "lumabri_sign.h"

int main(void) {
    uint8_t s1[32], s2[32], pk1[32], pk2[32], sk1[64], sk2[64];
    for (int i = 0; i < 32; i++) { s1[i] = (uint8_t)i; s2[i] = (uint8_t)(i + 73); }
    lmb_sign_keypair(pk1, sk1, s1); lmb_sign_keypair(pk2, sk2, s2);
    char h1[65], h2[65]; lmb_hex(h1, pk1, 32); lmb_hex(h2, pk2, 32);

    char path[] = "/tmp/lumabri-keyring.XXXXXX";
    int fd = mkstemp(path);
    FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!fp) return 2;
    fprintf(fp, "# overlap window: old then new\n%s\n%s\n", h1, h2);
    if (fclose(fp)) return 2;

    LmbTrustKeys overlap = {0}, new_only = {0};
    int bad = lmb_trust_load_spec(&overlap, path) ||
              lmb_trust_add_hex(&new_only, h2);
    unlink(path);
    const uint8_t msg[] = "manual-rotation-test";
    uint8_t a[64], b[64];
    lmb_sign(a, msg, sizeof msg, sk1); lmb_sign(b, msg, sizeof msg, sk2);
    bad |= overlap.n != 2 || lmb_trust_verify(&overlap, a, msg, sizeof msg) ||
           lmb_trust_verify(&overlap, b, msg, sizeof msg) ||
           lmb_trust_match(&overlap, a, msg, sizeof msg) != 0 ||
           lmb_trust_match(&overlap, b, msg, sizeof msg) != 1 ||
           lmb_trust_verify(&new_only, b, msg, sizeof msg) ||
           lmb_trust_verify(&new_only, a, msg, sizeof msg) == 0;
    if (bad) { fputs("key rotation trust-set failure\n", stderr); return 1; }
    puts("KEY ROTATION: PASS (old+new overlap, then old revoked manually)");
    return 0;
}
