#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint32_t *word;
static atomic_int entered;
static int result, failure;
static void *waiter(void *unused) {
    (void)unused;
    struct timespec timeout={.tv_nsec=250000000};
    atomic_store(&entered,1);
    result=syscall(SYS_futex,word,FUTEX_WAIT_PRIVATE,0,&timeout,NULL,0);
    failure=errno;
    return NULL;
}
int main(void) {
    alarm(15);
    for(int mode=0;mode<3;mode++)for(int repeat=0;repeat<5;repeat++) {
        word=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(word==MAP_FAILED)return 1;
        atomic_store(&entered,0);
        pthread_t thread;
        if(pthread_create(&thread,NULL,waiter,NULL))return 2;
        while(!atomic_load(&entered))usleep(1000);
        // Cover both a mapping removed after wait registration and an intact
        // mapping made unreadable. Never dereference it in the guest afterward.
        usleep(30000);
        if(mode==1&&mprotect(word,4096,PROT_NONE))return 3;
        if(mode==2&&munmap(word,4096))return 4;
        if(pthread_join(thread,NULL))return 5;
        // Linux can retain the queued key until timeout; LinuxRunner also
        // detects an invalidated mapping during its periodic safety check.
        if(result!=-1||(failure!=ETIMEDOUT&&(mode==0||failure!=EFAULT)))return 6;
        if(mode!=2&&munmap(word,4096))return 7;
    }
    puts("FUTEX_WAIT_MAPPING_PASS intact,PROT_NONE,unmap=15");
    return 0;
}
