/* Operating-system entropy only. Never substitute time, rand or a cached seed. */
#ifndef LUMABRI_RANDOM_H
#define LUMABRI_RANDOM_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib,"bcrypt.lib")
#endif
#endif

static void lmb_random(void *buf,size_t n) {
#ifdef _WIN32
    unsigned char *p=buf;
    while (n) {
        ULONG chunk=n>UINT32_MAX ? UINT32_MAX : (ULONG)n;
        if (BCryptGenRandom(NULL,p,chunk,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) {
            fputs("[lumabri] system entropy is unavailable\n",stderr); abort();
        }
        p+=chunk; n-=chunk;
    }
#else
    FILE *ur=fopen("/dev/urandom","rb");
    if (!ur || fread(buf,1,n,ur)!=n) {
        fprintf(stderr,"[lumabri] cannot read %zu random bytes from /dev/urandom\n",n);
        if (ur) fclose(ur);
        abort();
    }
    fclose(ur);
#endif
}
#endif
