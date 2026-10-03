/* Process memory evidence, not a claim inferred from sealed checkpoint I/O.
 * Linux: proc status (fast) or smaps_rollup (detailed). macOS: TASK_VM_INFO.
 * Unknown fields remain unknown. Compression is NOT labelled disk swap.
 */
#ifndef LMB_PROCESS_MEMORY_H
#define LMB_PROCESS_MEMORY_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

enum { LMB_PM_RSS=1u, LMB_PM_SWAP=2u, LMB_PM_LOCKED=4u,
       LMB_PM_COMPRESSED=8u, LMB_PM_FOOTPRINT=16u, LMB_PM_MAJOR_FAULTS=32u };
typedef struct {
    uint32_t known;
    uint64_t rss, swap, locked, compressed, footprint, major_faults;
    const char *source;
} LmbProcessMemory;

static inline uint64_t lmb_pm_add(uint64_t a,uint64_t b) {
    return b>UINT64_MAX-a ? UINT64_MAX : a+b;
}

/* Parse only bounded unsigned kB fields. Reject duplicate/malformed evidence
 * instead of manufacturing a zero-swapped or under-budget observation. */
static inline int lmb_process_memory_parse(FILE *f,int rollup,LmbProcessMemory *out) {
    LmbProcessMemory m={0};char line[1024];unsigned lines=0;
    if(!f || !out)return -1;
    while(fgets(line,sizeof line,f)) {
        if(++lines>512 || (!strchr(line,'\n') && !feof(f)))return -1;
        const char *keys[]={rollup?"Rss:":"VmRSS:",rollup?"Swap:":"VmSwap:",rollup?"Locked:":"VmLck:"};
        uint64_t *values[]={&m.rss,&m.swap,&m.locked};
        for(unsigned k=0;k<3;k++) {
            size_t n=strlen(keys[k]);if(strncmp(line,keys[k],n))continue;
            if(m.known&(1u<<k))return -1;
            const char *p=line+n;while(*p==' '||*p=='\t')p++;
            if(*p<'0'||*p>'9')return -1;
            uint64_t v=0;
            while(*p>='0'&&*p<='9') {
                unsigned d=(unsigned)(*p++-'0');if(v>(UINT64_MAX-d)/10)return -1;v=v*10+d;
            }
            if(*p!=' '&&*p!='\t')return -1;
            while(*p==' '||*p=='\t')p++;
            if(strncmp(p,"kB",2))return -1;
            p+=2;while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
            if(*p || v>UINT64_MAX/1024)return -1;
            *values[k]=v*1024;m.known|=1u<<k;
        }
    }
    if(ferror(f) || !(m.known&LMB_PM_RSS))return -1;
    m.source=rollup?"linux_smaps_rollup":"linux_proc_status";*out=m;return 0;
}

static inline int lmb_process_memory_probe(LmbProcessMemory *out,int detailed) {
    if(!out)return -1;
    LmbProcessMemory m={0};
#ifdef __APPLE__
    (void)detailed;
    task_vm_info_data_t info={0};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)!=KERN_SUCCESS ||
       count<TASK_VM_INFO_REV0_COUNT)return -1;
    m.rss=info.resident_size;m.compressed=info.compressed;
    m.known=LMB_PM_RSS|LMB_PM_COMPRESSED;m.source="macos_task_vm_info";
    if(count>=TASK_VM_INFO_REV1_COUNT){m.footprint=info.phys_footprint;m.known|=LMB_PM_FOOTPRINT;}
    /* TASK_VM_INFO does not provide a current per-process swapped-byte field.
     * Host vm.swapusage cannot fill this gap: it includes other applications. */
#elif defined(__linux__)
    int rc=-1;FILE *f=NULL;
    if(detailed && (f=fopen("/proc/self/smaps_rollup","r"))) {
        rc=lmb_process_memory_parse(f,1,&m);fclose(f);
    }
    if(rc && (f=fopen("/proc/self/status","r"))) {
        rc=lmb_process_memory_parse(f,0,&m);fclose(f);
    }
    if(rc)return -1;
#else
    (void)detailed;return -1;
#endif
    struct rusage r;
    if(!getrusage(RUSAGE_SELF,&r) && r.ru_majflt>=0) {
        m.major_faults=(uint64_t)r.ru_majflt;m.known|=LMB_PM_MAJOR_FAULTS;
    }
    *out=m;return 0;
}

/* Swapping/compressing an allocation never creates new capacity. This is a
 * conservative charge, not an exact physical-RAM usage metric. */
static inline uint64_t lmb_process_memory_charge(const LmbProcessMemory *m) {
    if(!m || !(m->known&LMB_PM_RSS))return UINT64_MAX;
    uint64_t n=m->rss;
    if(m->known&LMB_PM_SWAP)n=lmb_pm_add(n,m->swap);
    if(m->known&LMB_PM_COMPRESSED)n=lmb_pm_add(n,m->compressed);
    if((m->known&LMB_PM_FOOTPRINT) && m->footprint>n)n=m->footprint;
    return n;
}

static inline void lmb_process_memory_number(FILE *f,uint32_t known,uint32_t bit,uint64_t value) {
    if(known&bit)fprintf(f,"%llu",(unsigned long long)value);else fputs("null",f);
}
static inline void lmb_process_memory_json(FILE *f,const LmbProcessMemory *m) {
    fprintf(f,"{\"schema\":1,\"source\":\"%s\",\"rss_bytes\":",m->source?m->source:"unavailable");
    lmb_process_memory_number(f,m->known,LMB_PM_RSS,m->rss);
    fputs(",\"swap_bytes\":",f);lmb_process_memory_number(f,m->known,LMB_PM_SWAP,m->swap);
    fputs(",\"locked_bytes\":",f);lmb_process_memory_number(f,m->known,LMB_PM_LOCKED,m->locked);
    fputs(",\"compressed_bytes\":",f);lmb_process_memory_number(f,m->known,LMB_PM_COMPRESSED,m->compressed);
    fputs(",\"footprint_bytes\":",f);lmb_process_memory_number(f,m->known,LMB_PM_FOOTPRINT,m->footprint);
    fputs(",\"major_faults\":",f);lmb_process_memory_number(f,m->known,LMB_PM_MAJOR_FAULTS,m->major_faults);
    fputs("}",f);
}
#endif
