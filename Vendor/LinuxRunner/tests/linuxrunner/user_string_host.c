// Actual user-copy production implementation with guarded host backing. This
// isolates page translation cost; it does not predict browser/device speed.
#include <assert.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include "kernel/calls.h"
#ifndef USER_COPY_SOURCE
#define USER_COPY_SOURCE "kernel/user.c"
#endif
#include USER_COPY_SOURCE
__thread struct task *current;
int current_pid(void) { return 1; }
static unsigned long translations;
static char *backing;
void *mem_ptr(struct mem *m, addr_t p, int type) {
    (void)m; (void)type; translations++;
    if (p < 0x10000 || p >= 0x12000) return NULL;
    return backing + p - 0x10000;
}
static double now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void) {
    struct task task={0};struct mem memory={0};current=&task;task.mem=&memory;wrlock_init(&memory.lock);
    // A real host guard page ensures memchr never speculatively reaches beyond
    // the accessible backing when the guest string ends at a page boundary.
    size_t hostpage=sysconf(_SC_PAGESIZE);
    char *area=mmap(NULL,hostpage*3,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(area!=MAP_FAILED);backing=area+hostpage*2-8192;
    assert(mprotect(area,hostpage*2,PROT_READ|PROT_WRITE)==0);
    char out[8192];memset(backing,'A',8192);backing[8191]=0;
    assert(user_read_string(0x11fff,out,2)==0 && out[0]==0);
    backing[8191]='B';assert(user_read_string(0x11fff,out,2)==1 && out[0]=='B');
    backing[4095]=0;assert(user_read_string(0x10ffe,out,3)==0 && !strcmp(out,"A"));
    backing[4095]='C';backing[4096]='D';backing[4097]=0;
    assert(user_read_string(0x10ffe,out,4)==0 && !strcmp(out,"ACD"));
    assert(user_read_string(0,out,1)==1);
    assert(user_read_string(0x12000,out,1)==1);
    assert(user_read_string(0x12000,out,0)==0);
    memset(out,0x55,sizeof out);assert(user_read_string(0x10000,out,3)==0 && out[3]==0x55 && !memcmp(out,"AAA",3));
    assert(user_read(UINT64_MAX-3,out,8)==1);
    assert(user_write(UINT64_MAX-3,out,8)==1);
    assert(user_read(UINT64_MAX,out,0)==0);
    task.mem=NULL;assert(user_read_string(0x10000,out,3)==1);task.mem=&memory;
    memset(backing,'a',8192);backing[1023]=0;translations=0;
    double start=now();for(int i=0;i<200000;i++)assert(user_read_string(0x10000,out,1024)==0);
    printf("USER_STRING_PASS seconds=%.6f translations=%lu copies=200000 bytes=1024\n",now()-start,translations);
    // Production exec argument/environment writes must preserve partial fault
    // behavior, including the terminator. Translation counts are measured,
    // not asserted: the experimental chunked writer is not enabled in 856.
    memset(backing,0x55,8192);assert(user_write_string(0x10ffe,"abc")==0&&!memcmp(backing+4094,"abc",4));
    assert(user_write_string(0x11fff,"xy")==1&&backing[8191]=='x');
    assert(user_write_string(0x11fff,"")==0&&backing[8191]==0);
    assert(user_write_string(0x12000,"x")==1&&user_write_string(0,"")==1);
    task.mem=NULL;assert(user_write_string(0x10000,"x")==1);task.mem=&memory;
    memset(out,'w',1023);out[1023]=0;translations=0;start=now();
    for(int i=0;i<200000;i++)assert(user_write_string(0x10000,out)==0);
    printf("USER_STRING_WRITE_PASS seconds=%.6f translations=%lu copies=200000 bytes=1024\n",now()-start,translations);
    munmap(area,hostpage*3);return 0;
}
