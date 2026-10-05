/* Bounded, exact-plan observations. The legacy latest-per-checkpoint record
 * remains available to old catalogue consumers. These slots retain alternate
 * replica observations instead of letting one replica erase another's data.
 * No prompts, outputs, tokens, credentials or fabricated capacity are stored. */
#ifndef LMB_CALIBRATION_PROFILES_H
#define LMB_CALIBRATION_PROFILES_H
#include "lumabri_calibration_store.h"
#include <sys/file.h>

#define LMB_CAL_PROFILE_SLOTS 16u

static LMB_UNUSED int lmb_cal_profile_private(int fd, int directory) {
    struct stat st;
    return fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&077) ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode) || st.st_nlink!=1) ? -1 : 0;
}
static LMB_UNUSED int lmb_cal_profile_dir(const char *directory, const char *root, int create) {
    char name[80], check[69];
    if (lmb_cal_filename(root,check)) return -1;
    int parent=lmb_cal_directory(directory,create); if (parent<0) return -1;
    snprintf(name,sizeof name,"%s.profiles",root);
    if (create && mkdirat(parent,name,0700) && errno!=EEXIST) { close(parent); return -1; }
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); close(parent);
    if (fd>=0 && lmb_cal_profile_private(fd,1)) { close(fd); return -1; }
    return fd;
}
/* Missing or corrupt slots are not observations. Unsafe files are never read
 * or truncated. Readers see atomic records without waiting for the writer. */
static LMB_UNUSED int lmb_cal_profile_read(int dir, unsigned slot, LmbCalibration *out) {
    memset(out,0,sizeof *out); char name[24]; snprintf(name,sizeof name,"%u.cal",slot);
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return -1;
    struct stat st; int rc=-1;
    if (!lmb_cal_profile_private(fd,0) && !fstat(fd,&st) && st.st_size>=40 && st.st_size<=LMB_CAL_RECORD_MAX) {
        unsigned char *bytes=malloc((size_t)st.st_size); size_t at=0;
        if (bytes) {
            while (at<(size_t)st.st_size) {
                ssize_t n=read(fd,bytes+at,(size_t)st.st_size-at);
                if (n<0 && errno==EINTR) continue;
                if (n<=0) break;
                at+=(size_t)n;
            }
            unsigned char extra;
            if (at==(size_t)st.st_size && read(fd,&extra,1)==0) rc=lmb_cal_decode(bytes,at,out);
            free(bytes);
        }
    }
    close(fd); return rc;
}
static LMB_UNUSED int lmb_cal_profile_load(const char *directory, const LmbCalKey *key, LmbCalibration *out) {
    if (!out) return -1;
    memset(out,0,sizeof *out);
    if (!lmb_cal_key_valid(key)) return -1;
    int dir=lmb_cal_profile_dir(directory,key->model_root,0); if (dir<0) return -1;
    LmbCalibration record; int found=0;
    for (unsigned i=0;i<LMB_CAL_PROFILE_SLOTS;i++)
        if (!lmb_cal_profile_read(dir,i,&record) && lmb_cal_matches(key,&record.key) &&
            (!found || record.measured_at>out->measured_at)) { *out=record; found=1; }
    close(dir); return found ? 0 : -1;
}
static LMB_UNUSED int lmb_cal_profile_store(const char *directory, const LmbCalibration *record) {
    if (!lmb_cal_valid(record)) return -1;
    int dir=lmb_cal_profile_dir(directory,record->key.model_root,1); if (dir<0) return -1;
    int guard=openat(dir,"write.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600),rc=-1;
    if (guard<0 || lmb_cal_profile_private(guard,0)) goto done;
    if (flock(guard,LOCK_EX|LOCK_NB)) goto done; /* telemetry must not queue inference */
    unsigned target=0; double oldest=HUGE_VAL; LmbCalibration prior;
    for (unsigned i=0;i<LMB_CAL_PROFILE_SLOTS;i++) {
        if (lmb_cal_profile_read(dir,i,&prior)) {
            if (oldest!=0) { oldest=0; target=i; }
            continue;
        }
        if (lmb_cal_matches(&record->key,&prior.key)) {
            /* A delayed writer may not roll a newer observation back. */
            if (prior.measured_at>record->measured_at) { rc=0; goto done; }
            target=i; break;
        }
        if (prior.measured_at<oldest) { oldest=prior.measured_at; target=i; }
    }
    LmbBuf bytes={0};
    if (lmb_cal_encode(record,&bytes)) goto done;
    /* Fixed private transaction name bounds crash debris as well as history. */
    struct stat st;
    if (!fstatat(dir,"pending",&st,AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077) || st.st_nlink!=1 ||
            unlinkat(dir,"pending",0)) { free(bytes.p); goto done; }
    } else if (errno!=ENOENT) { free(bytes.p); goto done; }
    int fd=openat(dir,"pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600),bad=fd<0;
    for (size_t at=0;!bad && at<bytes.len;) {
        ssize_t n=write(fd,bytes.p+at,bytes.len-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    free(bytes.p);
    if (fd>=0) { if (!bad && fsync(fd)) bad=1; if (close(fd)) bad=1; }
    char name[24]; snprintf(name,sizeof name,"%u.cal",target);
    if (!bad && renameat(dir,"pending",dir,name)) bad=1;
    if (!bad && fsync(dir)) bad=1;
    if (bad) (void)unlinkat(dir,"pending",0);
    rc=bad ? -1 : 0;
done:
    if (guard>=0) close(guard);
    close(dir); return rc;
}
#endif
