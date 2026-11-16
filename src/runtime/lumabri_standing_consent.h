/* Standing consent: this computer's owner lets requests from the household it
 * belongs to be accepted without a person present. It replaces the click and
 * nothing else. The request still has to be an authenticated offer for this
 * household, fit what is left of the shared memory budget, and pass the same
 * machine admission (governor, compute lease, weight lease, free RAM and
 * disk, residency policy, model integrity) as a manual acceptance.
 *
 * One private record beside the keeper sockets, written only by this OS user
 * from this computer: its command line, its terminal workspace or its own
 * loopback page. No household message reads or writes it. It is bound to one
 * household: its address AND a digest of its key, so another household, or a
 * later one at the same address, does not inherit it. Unlike a bounded repeat
 * policy it names no donor incarnation and has no expiry: it survives
 * restarts and stopping and starting sharing, until the owner turns it off.
 * A missing, unreadable or unsafe record always means "ask each time". */
#ifndef LMB_STANDING_CONSENT_H
#define LMB_STANDING_CONSENT_H
#include "lumabri_donor_policy_store.h"

#define LMB_STANDING_CONSENT_NAME "standing-consent"
#define LMB_STANDING_JOURNAL_NAME "standing-consent.log"
#define LMB_STANDING_CONSENT_BYTES 88u
#define LMB_STANDING_JOURNAL_MAX 65536
#define LMB_STANDING_RECENT 16u
/* START: switched on by `lumabri start` for a household on a fresh install,
 * and never changed by a person since. OWNER: an explicit choice. */
enum { LMB_STANDING_OWNER = 1, LMB_STANDING_START = 2 };
typedef struct {
    uint32_t enabled, origin;
    uint64_t changed;
    uint8_t household[32];
} LmbStandingConsent;
typedef struct {
    uint64_t when, ram_bytes;
    uint32_t begin, end;
    char requester[65], allocation[65], model[256];
} LmbStandingAccepted;

static inline void lmb_standing_household(const char *tracker,const char *token,uint8_t out[32]) {
    static const char domain[]="lumabri-standing-consent-v1";
    LmbSha sha; lmb_sha_init(&sha);
    lmb_sha_update(&sha,domain,sizeof domain);
    lmb_sha_update(&sha,tracker,strlen(tracker)+1);
    lmb_sha_update(&sha,token ? token : "",strlen(token ? token : "")+1);
    lmb_sha_final(&sha,out);
}
static inline int lmb_standing_dir(const char *service_directory) {
    int dir=open(service_directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir>=0 && lmb_donor_policy_private(dir,1)) { close(dir); dir=-1; }
    return dir;
}
/* 0: a valid record. 1: none. -1: unsafe or damaged, treated as none. */
static inline int lmb_standing_consent_load(const char *service_directory,LmbStandingConsent *c) {
    memset(c,0,sizeof *c);
    int dir=lmb_standing_dir(service_directory);
    if (dir<0) return -1;
    int fd=openat(dir,LMB_STANDING_CONSENT_NAME,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int missing=fd<0 && errno==ENOENT;
    close(dir);
    if (fd<0) return missing ? 1 : -1;
    struct stat st; uint8_t bytes[LMB_STANDING_CONSENT_BYTES],hash[32];
    int bad=lmb_donor_policy_private(fd,0) || fstat(fd,&st) || st.st_size!=(off_t)sizeof bytes ||
        lmb_read_full(fd,bytes,sizeof bytes);
    close(fd);
    if (bad || memcmp(bytes,"LMBSCON1",8)) return -1;
    lmb_donor_policy_digest(bytes,sizeof bytes-32,hash);
    if (memcmp(hash,bytes+sizeof bytes-32,32)) return -1;
    LmbCur cur={bytes,sizeof bytes-32,8}; LmbStandingConsent read={0};
    if (lmb_cur_u32(&cur,&read.enabled) || lmb_cur_u32(&cur,&read.origin) ||
        lmb_cur_u64(&cur,&read.changed) || lmb_cur_bytes(&cur,read.household,32) || cur.off!=cur.len ||
        read.enabled>1 || (read.origin!=LMB_STANDING_OWNER && read.origin!=LMB_STANDING_START)) return -1;
    *c=read; return 0;
}
static inline int lmb_standing_write_all(int fd,const void *data,size_t n) {
    const char *p=data; size_t at=0;
    while (at<n) {
        ssize_t wrote=write(fd,p+at,n-at);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) return -1;
        at+=(size_t)wrote;
    }
    return 0;
}
static inline int lmb_standing_consent_store(const char *service_directory,const LmbStandingConsent *c) {
    static const char pending[]=LMB_STANDING_CONSENT_NAME ".pending";
    LmbBuf b={0}; uint8_t hash[32];
    int bad=c->enabled>1 || (c->origin!=LMB_STANDING_OWNER && c->origin!=LMB_STANDING_START) ||
        lmb_buf_bytes(&b,"LMBSCON1",8) || lmb_buf_u32(&b,c->enabled) || lmb_buf_u32(&b,c->origin) ||
        lmb_buf_u64(&b,c->changed) || lmb_buf_bytes(&b,c->household,32);
    if (!bad) { lmb_donor_policy_digest(b.p,b.len,hash); bad=lmb_buf_bytes(&b,hash,32) || b.len!=LMB_STANDING_CONSENT_BYTES; }
    int dir=bad ? -1 : lmb_standing_dir(service_directory);
    if (dir<0) bad=1;
    struct stat st;
    if (!bad && !fstatat(dir,pending,&st,AT_SYMLINK_NOFOLLOW))
        bad=!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || unlinkat(dir,pending,0);
    else if (!bad && errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,pending,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    if (!bad) bad=lmb_standing_write_all(fd,b.p,b.len) || fsync(fd);
    if (fd>=0 && close(fd)) bad=1;
    if (!bad) bad=renameat(dir,pending,dir,LMB_STANDING_CONSENT_NAME) || fsync(dir);
    if (dir>=0) close(dir);
    free(b.p); return bad ? -1 : 0;
}
/* The record as it applies to ONE household. Returns 1 only for an enabled
 * record of exactly this household; *found tells a caller whether any valid
 * record of this household exists (so "off" is distinguishable from "never
 * chosen"). */
static inline int lmb_standing_consent_for(const char *service_directory,const char *tracker,const char *token,
                                           LmbStandingConsent *c,int *found) {
    uint8_t household[32]; LmbStandingConsent read;
    if (found) *found=0;
    if (c) memset(c,0,sizeof *c);
    if (!tracker || !*tracker || lmb_standing_consent_load(service_directory,&read)) return 0;
    lmb_standing_household(tracker,token,household);
    if (memcmp(household,read.household,32)) return 0;
    if (found) *found=1;
    if (c) *c=read;
    return read.enabled==1;
}
static inline int lmb_standing_consent_set(const char *service_directory,const char *tracker,const char *token,
                                           int enabled,uint32_t origin) {
    if (!tracker || !*tracker) return -1;
    LmbStandingConsent c={.enabled=enabled ? 1u : 0u,.origin=origin,.changed=(uint64_t)time(NULL)};
    lmb_standing_household(tracker,token,c.household);
    return lmb_standing_consent_store(service_directory,&c);
}

/* What was accepted without a click: one line per load, newest last, in a
 * private bounded text file. Only the donor keeper appends. */
static inline int lmb_standing_journal_append(const char *service_directory,const LmbStandingAccepted *a) {
    char line[512],model[256]; size_t n=0;
    for (const char *p=a->model; *p && n+1<sizeof model; p++)
        model[n++]=(unsigned char)*p<0x20 || *p==0x7f ? '?' : *p;
    model[n]=0;
    int length=snprintf(line,sizeof line,"%llu %.64s %.64s %llu %u %u %s\n",(unsigned long long)a->when,
        a->requester,a->allocation,(unsigned long long)a->ram_bytes,a->begin,a->end,model);
    if (length<0 || (size_t)length>=sizeof line) return -1;
    int dir=lmb_standing_dir(service_directory);
    if (dir<0) return -1;
    int fd=openat(dir,LMB_STANDING_JOURNAL_NAME,O_RDWR|O_CREAT|O_APPEND|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    close(dir);
    if (fd<0) return -1;
    struct stat st;
    int bad=lmb_donor_policy_private(fd,0) || fstat(fd,&st);
    /* Bounded: start again rather than grow without limit. */
    if (!bad && st.st_size>LMB_STANDING_JOURNAL_MAX) bad=ftruncate(fd,0);
    if (!bad) bad=lmb_standing_write_all(fd,line,(size_t)length);
    if (close(fd)) bad=1;
    return bad ? -1 : 0;
}
/* The newest entries, oldest first. Returns how many were read. */
static inline unsigned lmb_standing_journal_recent(const char *service_directory,LmbStandingAccepted *out,unsigned cap) {
    int dir=lmb_standing_dir(service_directory);
    if (dir<0) return 0;
    int fd=openat(dir,LMB_STANDING_JOURNAL_NAME,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    close(dir);
    if (fd<0) return 0;
    struct stat st; char *text=NULL; unsigned count=0;
    if (!lmb_donor_policy_private(fd,0) && !fstat(fd,&st) && st.st_size>0 && st.st_size<=2*LMB_STANDING_JOURNAL_MAX &&
        (text=malloc((size_t)st.st_size+1)) && !lmb_read_full(fd,text,(size_t)st.st_size)) {
        text[st.st_size]=0;
        for (char *line=text,*next; line && *line; line=next) {
            next=strchr(line,'\n');
            if (!next) break; /* an unfinished line is not an entry */
            *next++=0;
            LmbStandingAccepted a={0}; unsigned long long when,ram; int used=0;
            if (sscanf(line,"%llu %64s %64s %llu %u %u %n",&when,a.requester,a.allocation,&ram,&a.begin,&a.end,&used)!=6 ||
                !used || strlen(a.requester)!=64 || strlen(a.allocation)!=64) continue;
            a.when=when; a.ram_bytes=ram; snprintf(a.model,sizeof a.model,"%s",line+used);
            if (count==cap) { memmove(out,out+1,(cap-1)*sizeof *out); count--; }
            out[count++]=a;
        }
    }
    free(text); close(fd);
    return count;
}
#endif
