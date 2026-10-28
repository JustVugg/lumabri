/* Persistent peer identities and public trust files. The cryptographic
 * primitives and their original attribution live in lumabri_sign_core.h. */
#ifndef LUMABRI_SIGN_H
#define LUMABRI_SIGN_H
#include "src/crypto/lumabri_sign_core.h"
#include "src/crypto/lumabri_random.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* A keyring file contains one 64-hex public key per whitespace-separated
 * field.  A literal spec may contain the same keys separated by commas.
 * Calls append, so command-line programs may also repeat --pubkey. */
static int lmb_trust_load_stream(LmbTrustKeys *t, FILE *fp) {
    char word[256];
    LmbTrustKeys add = {0};
    while (t && fp && fscanf(fp, "%255s", word) == 1) {
        if (word[0] == '#') { int ch; while ((ch = fgetc(fp)) != '\n' && ch != EOF) {} continue; }
        if (lmb_trust_add_hex(&add, word)) return -1;
    }
    if (!t || !add.n) return -1;
    size_t before = t->n;
    for (size_t i = 0; i < add.n; i++) {
        char hex[65]; lmb_hex(hex, add.key[i], 32);
        if (lmb_trust_add_hex(t, hex)) { t->n = before; return -1; }
    }
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_trust_load_spec(LmbTrustKeys *t,
                                                const char *spec) {
    if (!t || !spec || !*spec) return -1;
    FILE *fp = fopen(spec, "r");
    if (fp) {
        int rc = lmb_trust_load_stream(t, fp);
        fclose(fp);
        return rc;
    }
    char *copy = strdup(spec);
    if (!copy) return -1;
    LmbTrustKeys add = {0};
    size_t before = t->n;
    char *save = NULL;
    for (char *p = strtok_r(copy, ",", &save); p; p = strtok_r(NULL, ",", &save))
        if (lmb_trust_add_hex(&add, p)) { free(copy); return -1; }
    free(copy);
    if (!add.n) return -1;
    for (size_t i = 0; i < add.n; i++) {
        char hex[65]; lmb_hex(hex, add.key[i], 32);
        if (lmb_trust_add_hex(t, hex)) { t->n = before; return -1; }
    }
    return 0;
}

/* ---- peer identity ------------------------------------------------------
 * A peer authenticates its own name to the tracker with an Ed25519 key that
 * is NOT the operator key: the operator key signs the model, this key only
 * says "this name is mine". The tracker binds a name to the first key that
 * claims it and refuses any later claim under a different key, so a stranger
 * can no longer register under an honest peer's name and evict it.
 */

/* Load this machine's peer key from `path`, or create it on first use (0600).
 * One key per machine: every role it runs shares the identity, and each of
 * their names is bound to it. Returns 0, or -1 if the file is unreadable and
 * cannot be created. */
static LMB_MAYBE_UNUSED int lmb_peer_identity(const char *path, uint8_t sk[64],
                                              uint8_t pk[32]) {
    /* Two roles on one machine (a serve's maintainer and expert node) start
     * together and both may find no key. Exclusive-create on the real path so
     * exactly one writes it; the loser re-reads the winner's file instead of
     * writing a second, different key that would fail its own name after a
     * restart. A brief spin covers the window where the file exists but the
     * winner has not finished writing it. */
    for (int attempt = 0; attempt < 200; attempt++) {
        char hex[200] = "";
        int rflags = O_RDONLY;
#ifdef O_NOFOLLOW
        rflags |= O_NOFOLLOW;
#endif
        int rfd = open(path, rflags);
        if (rfd >= 0) {
            struct stat st;
            if (fstat(rfd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                (st.st_mode & 077) != 0) { close(rfd); errno = EACCES; return -1; }
            FILE *f = fdopen(rfd, "r");
            if (!f) { close(rfd); return -1; }
            int got = fscanf(f, "%198s", hex) == 1;
            fclose(f);
            if (got && strlen(hex) == 128 && !lmb_unhex(sk, hex, 64)) {
                memcpy(pk, sk + 32, 32);
                return 0;
            }
            usleep(3000);        /* created but not yet written: wait and retry */
            continue;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) { usleep(3000); continue; }
            return -1;
        }
        uint8_t seed[32];
        lmb_random(seed, sizeof seed);
        lmb_sign_keypair(pk, sk, seed);
        memset(seed, 0, sizeof seed);
        char out[130];
        lmb_hex(out, sk, 64);
        out[128] = '\n'; out[129] = 0;
        size_t off = 0;
        while (off < 129) {
            ssize_t n = write(fd, out + off, 129 - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += (size_t)n;
        }
        int wrote = off == 129;
        if (fsync(fd)) wrote = 0;
        close(fd);
        if (!wrote) { unlink(path); return -1; }
        return 0;
    }
    return -1;
}

/* This machine's default peer-key path: $LUMABRI_PEER_KEY, else
 * $HOME/.lumabri/peer.key, else ./.lumabri_peer.key. */
static LMB_MAYBE_UNUSED const char *lmb_peer_key_path(char *buf, size_t cap) {
    const char *e = getenv("LUMABRI_PEER_KEY");
    if (e && e[0]) { snprintf(buf, cap, "%s", e); return buf; }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[400];
        snprintf(dir, sizeof dir, "%s/.lumabri", home);
        mkdir(dir, 0700);
        snprintf(buf, cap, "%s/peer.key", dir);
    } else snprintf(buf, cap, "./.lumabri_peer.key");
    return buf;
}

/* The exact bytes both sides sign for a registration: a domain tag so this
 * signature can never be replayed as any other, the tracker's per-connection
 * nonce so it cannot be replayed on another connection, then the identity
 * this REGISTER claims. Built in one place so the tracker and the peer cannot
 * disagree by a byte. Returns the length, or 0 if it would not fit. */
#define LMB_PEER_AUTH_TAG "lumabri-peer-auth-v1"
static LMB_MAYBE_UNUSED size_t lmb_peer_auth_msg(const uint8_t nonce[32],
                                                 const char *name,
                                                 const char *model,
                                                 const char *addr,
                                                 uint8_t *out, size_t cap) {
    size_t nl = strlen(name), ml = strlen(model), al = strlen(addr);
    size_t need = sizeof(LMB_PEER_AUTH_TAG) + 32 + nl + 1 + ml + 1 + al + 1;
    if (need > cap) return 0;
    size_t o = 0;
    memcpy(out + o, LMB_PEER_AUTH_TAG, sizeof(LMB_PEER_AUTH_TAG));
    o += sizeof(LMB_PEER_AUTH_TAG);
    memcpy(out + o, nonce, 32); o += 32;
    memcpy(out + o, name, nl);  o += nl; out[o++] = 0;
    memcpy(out + o, model, ml); o += ml; out[o++] = 0;
    memcpy(out + o, addr, al);  o += al; out[o++] = 0;
    return o;
}

#endif /* LUMABRI_SIGN_H */
